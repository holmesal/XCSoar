// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#import "BluetoothManager.h"
#include "BluetoothHelper.hpp"
#include "BluetoothUuids.hpp"
#include "PortBridge.hpp"
#include "LogFile.hpp"
#include "thread/Mutex.hxx"

#include <mutex>
#include <set>
#include <stdexcept>

#include <string.h>

BluetoothHelper *bluetooth_helper;

/**
 * NSUserDefaults key for the identifier -> name cache.  CoreBluetooth
 * only knows names of peripherals it has seen since boot, but XCSoar
 * wants to display the configured device's name right at startup.
 */
static NSString *const kNamesDefaultsKey = @"XCSoarBluetoothDeviceNames";

void *_Nonnull const XCSBluetoothQueueKey = (void *)&XCSBluetoothQueueKey;

/**
 * Bit set in the per-peripheral "already reported" value when the
 * report included a name.
 */
static constexpr uint64_t REPORTED_WITH_NAME = 1ULL << 63;

static uint64_t
FeaturesFromServiceUuids(NSArray<CBUUID *> *uuids) noexcept
{
  if (uuids == nil)
    return 0;

  static CBUUID *const nus = [CBUUID UUIDWithString:@(BluetoothUuids::NORDIC_UART_SERVICE)];
  static CBUUID *const issc = [CBUUID UUIDWithString:@(BluetoothUuids::ISSC_UART_SERVICE)];
  static CBUUID *const hm10 = [CBUUID UUIDWithString:@(BluetoothUuids::HM10_SERVICE)];
  static CBUUID *const heart_rate = [CBUUID UUIDWithString:@(BluetoothUuids::HEART_RATE_SERVICE)];
  static CBUUID *const sensbox = [CBUUID UUIDWithString:@(BluetoothUuids::FLYTEC_SENSBOX_SERVICE)];

  uint64_t features = 0;
  for (CBUUID *uuid in uuids) {
    if ([uuid isEqual:nus] || [uuid isEqual:issc] || [uuid isEqual:hm10])
      features |= DetectDeviceListener::FEATURE_BLE_SERIAL;
    else if ([uuid isEqual:heart_rate])
      features |= DetectDeviceListener::FEATURE_HEART_RATE;
    else if ([uuid isEqual:sensbox])
      features |= DetectDeviceListener::FEATURE_FLYTEC_SENSBOX;
  }

  return features;
}

static const char *
StateName(CBManagerState state) noexcept
{
  switch (state) {
  case CBManagerStatePoweredOn: return "powered on";
  case CBManagerStatePoweredOff: return "powered off";
  case CBManagerStateUnsupported: return "unsupported";
  case CBManagerStateUnauthorized: return "unauthorized";
  case CBManagerStateResetting: return "resetting";
  case CBManagerStateUnknown: break;
  }
  return "unknown";
}

@implementation XCSBluetoothManager {
  dispatch_queue_t _queue;
  CBCentralManager *_central;

  /** protects listeners, names and reported */
  Mutex mutex;

  std::set<DetectDeviceListener *> listeners;

  /** peripheral identifier -> advertised name */
  NSMutableDictionary<NSString *, NSString *> *names;
  bool namesDirty;

  /**
   * Peripherals already reported to the listeners during the
   * current scan, with the features (and REPORTED_WITH_NAME) they
   * were reported with.  Used to throttle the flood of duplicate
   * advertisements.
   */
  NSMutableDictionary<NSString *, NSNumber *> *reported;

  /** only accessed on the queue */
  NSMutableArray<XCSBleSerialPort *> *ports;
  BOOL scanning;
}

@synthesize queue = _queue;
@synthesize central = _central;

- (instancetype)init
{
  self = [super init];
  if (self) {
    _queue = dispatch_queue_create("org.xcsoar.bluetooth", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(_queue, XCSBluetoothQueueKey,
                                XCSBluetoothQueueKey, nullptr);
    ports = [NSMutableArray array];
    reported = [NSMutableDictionary dictionary];
    namesDirty = false;

    NSDictionary *saved = [[NSUserDefaults standardUserDefaults]
                            dictionaryForKey:kNamesDefaultsKey];
    names = saved != nil
      ? [saved mutableCopy]
      : [NSMutableDictionary dictionary];

    /* no system "turn on Bluetooth" alert; XCSoar has its own
       "Bluetooth is disabled" message */
    NSDictionary *options = @{CBCentralManagerOptionShowPowerAlertKey: @NO};
    _central = [[CBCentralManager alloc] initWithDelegate:self
                                                    queue:_queue
                                                  options:options];
  }
  return self;
}

- (BOOL)isPoweredOn
{
  return _central.state == CBManagerStatePoweredOn;
}

- (BOOL)isUnsupported
{
  return _central.state == CBManagerStateUnsupported;
}

/** caller must hold the mutex */
- (void)persistNamesLocked
{
  if (!namesDirty)
    return;

  namesDirty = false;
  [[NSUserDefaults standardUserDefaults] setObject:[names copy]
                                            forKey:kNamesDefaultsKey];
}

- (nullable NSString *)nameForIdentifier:(NSString *)identifier
{
  {
    const std::lock_guard lock{mutex};
    NSString *name = names[identifier];
    if (name != nil)
      return name;
  }

  if (![self isPoweredOn])
    return nil;

  NSUUID *uuid = [[NSUUID alloc] initWithUUIDString:identifier];
  if (uuid == nil)
    return nil;

  NSArray<CBPeripheral *> *known =
    [_central retrievePeripheralsWithIdentifiers:@[uuid]];
  NSString *name = known.firstObject.name;
  if (name.length == 0)
    return nil;

  const std::lock_guard lock{mutex};
  names[identifier] = name;
  namesDirty = true;
  [self persistNamesLocked];
  return name;
}

- (void)addDetectDeviceListener:(DetectDeviceListener *)listener
{
  {
    const std::lock_guard lock{mutex};
    listeners.insert(listener);
  }

  dispatch_async(_queue, ^{ [self updateScanning]; });
}

- (void)removeDetectDeviceListener:(DetectDeviceListener *)listener
{
  {
    const std::lock_guard lock{mutex};
    listeners.erase(listener);
  }

  dispatch_async(_queue, ^{ [self updateScanning]; });
}

- (void)updateScanning
{
  bool need;
  {
    const std::lock_guard lock{mutex};
    need = !listeners.empty();
  }

  if (!need)
    for (XCSBleSerialPort *port in ports)
      if ([port needsScan]) {
        need = true;
        break;
      }

  if (need && !scanning && [self isPoweredOn]) {
    {
      const std::lock_guard lock{mutex};
      [reported removeAllObjects];
    }

    LogFmt("BLE: scan started");
    /* allow duplicates so we see the scan response carrying the
       device name, which is often missing from the first
       advertisement; didDiscoverPeripheral throttles what reaches
       the listeners */
    [_central scanForPeripheralsWithServices:nil
                                     options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @YES}];
    scanning = YES;
  } else if (!need && scanning) {
    LogFmt("BLE: scan stopped");
    [_central stopScan];
    scanning = NO;
  }
}

- (XCSBleSerialPort *)openBleSerialPort:(NSString *)address
{
  NSUUID *uuid = [[NSUUID alloc] initWithUUIDString:address];
  XCSBleSerialPort *port =
    [[XCSBleSerialPort alloc] initWithManager:self
                                   identifier:uuid
                                   targetName:uuid == nil ? address : nil];

  dispatch_sync(_queue, ^{
    [ports addObject:port];
    [port start];
    [self updateScanning];
  });

  return port;
}

- (void)portClosed:(XCSBleSerialPort *)port
{
  [ports removeObject:port];
  [self updateScanning];
}

- (nullable XCSBleSerialPort *)portForPeripheral:(CBPeripheral *)peripheral
{
  for (XCSBleSerialPort *port in ports)
    if (port.peripheral == peripheral ||
        [port.identifier isEqual:peripheral.identifier])
      return port;
  return nil;
}

/* CBCentralManagerDelegate */

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
  LogFmt("BLE: Bluetooth {}", StateName(central.state));

  if (central.state == CBManagerStatePoweredOn) {
    for (XCSBleSerialPort *port in [ports copy])
      [port managerPoweredOn];
    [self updateScanning];
  } else {
    /* CoreBluetooth forgets all scans and connections when the
       radio goes away */
    scanning = NO;
    for (XCSBleSerialPort *port in [ports copy])
      [port managerPoweredOff];
  }
}

- (void)centralManager:(CBCentralManager *)central
 didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *, id> *)advertisementData
                  RSSI:(NSNumber *)RSSI
{
  NSString *identifier = peripheral.identifier.UUIDString;

  NSString *name = advertisementData[CBAdvertisementDataLocalNameKey];
  if (name.length == 0)
    name = peripheral.name;
  if (name.length == 0)
    name = nil;

  uint64_t features =
    FeaturesFromServiceUuids(advertisementData[CBAdvertisementDataServiceUUIDsKey]) |
    FeaturesFromServiceUuids(advertisementData[CBAdvertisementDataOverflowServiceUUIDsKey]);

  if (name != nil) {
    const std::lock_guard lock{mutex};
    if (![names[identifier] isEqualToString:name]) {
      names[identifier] = name;
      namesDirty = true;
      [self persistNamesLocked];
    }
  }

  /* is somebody waiting for this peripheral? */
  for (XCSBleSerialPort *port in [ports copy])
    if (port.peripheral == nil &&
        [port matchesDiscoveredPeripheral:peripheral name:name])
      [port attachDiscoveredPeripheral:peripheral];

  if (name == nil && features == 0)
    /* anonymous device without a service XCSoar knows: not worth
       listing */
    return;

  if (features != 0 && (features & DetectDeviceListener::FEATURE_BLE_SERIAL) == 0)
    /* a sensor-only device (heart rate, Sensbox); BLE sensors are not
       supported on Apple platforms yet, so do not offer it */
    return;

  if (features == 0)
    /* a named device which does not advertise any service UUID: it
       may still be a serial bridge (many adapters put only the name
       in the advertisement), so offer it as a BLE port; connecting
       will fail cleanly if it has no serial service */
    features = DetectDeviceListener::FEATURE_BLE_SERIAL;

  const std::lock_guard lock{mutex};

  const uint64_t key = features | (name != nil ? REPORTED_WITH_NAME : 0);
  NSNumber *previous = reported[identifier];
  if (previous != nil && previous.unsignedLongLongValue == key)
    return;
  reported[identifier] = @(key);

  for (DetectDeviceListener *listener : listeners)
    listener->OnDeviceDetected(DetectDeviceListener::Type::BLUETOOTH_LE,
                               identifier.UTF8String,
                               name != nil ? name.UTF8String : nullptr,
                               features);
}

- (void)centralManager:(CBCentralManager *)central
  didConnectPeripheral:(CBPeripheral *)peripheral
{
  [[self portForPeripheral:peripheral] didConnect];
}

- (void)centralManager:(CBCentralManager *)central
didFailToConnectPeripheral:(CBPeripheral *)peripheral
                 error:(nullable NSError *)error
{
  [[self portForPeripheral:peripheral] didFailToConnect:error];
}

- (void)centralManager:(CBCentralManager *)central
didDisconnectPeripheral:(CBPeripheral *)peripheral
                 error:(nullable NSError *)error
{
  [[self portForPeripheral:peripheral] didDisconnect:error];
}

@end

/* C++ facade */

BluetoothHelper::BluetoothHelper() noexcept
  :manager([[XCSBluetoothManager alloc] init])
{
}

BluetoothHelper::~BluetoothHelper() noexcept
{
  manager = nil;
}

bool
BluetoothHelper::HasBluetoothSupport() const noexcept
{
  return ![manager isUnsupported];
}

bool
BluetoothHelper::IsEnabled() const noexcept
{
  return [manager isPoweredOn];
}

const char *
BluetoothHelper::GetNameFromAddress(const char *address) const noexcept
{
  if (address == nullptr || *address == 0)
    return nullptr;

  NSString *name = [manager nameForIdentifier:@(address)];
  if (name == nil)
    return nullptr;

  static thread_local char buffer[256];
  strncpy(buffer, name.UTF8String, sizeof(buffer) - 1);
  buffer[sizeof(buffer) - 1] = 0;
  return buffer;
}

void
BluetoothHelper::AddDetectDeviceListener(DetectDeviceListener &l) noexcept
{
  [manager addDetectDeviceListener:&l];
}

void
BluetoothHelper::RemoveDetectDeviceListener(DetectDeviceListener &l) noexcept
{
  [manager removeDetectDeviceListener:&l];
}

PortBridge *
BluetoothHelper::connectBleSerial(const char *address)
{
  if (address == nullptr || *address == 0)
    throw std::runtime_error{"No Bluetooth address configured"};

  if ([manager isUnsupported])
    throw std::runtime_error{"Bluetooth LE is not supported on this device"};

  XCSBleSerialPort *port = [manager openBleSerialPort:@(address)];
  return new PortBridge(port);
}
