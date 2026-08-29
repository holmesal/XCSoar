// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#import "BluetoothManager.h"
#include "PortBridge.hpp"
#include "BluetoothUuids.hpp"
#include "Device/Port/Listener.hpp"
#include "Device/Error.hpp"
#include "io/DataHandler.hpp"
#include "LogFile.hpp"
#include "thread/Mutex.hxx"
#include "thread/Cond.hxx"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>

#include <string.h>

/**
 * Size of the transmit buffer.  Port::FullWrite() blocks while it
 * is full, which gives us natural backpressure towards the device
 * drivers (e.g. during FLARM IGC downloads, which are
 * request/response driven).
 */
static constexpr std::size_t WRITE_BUFFER_SIZE = 4096;

/**
 * How long write() waits for room in the transmit buffer, and how
 * long drain() waits for the buffer to empty.
 */
static constexpr auto WRITE_TIMEOUT = std::chrono::seconds(5);

/**
 * The ATT default MTU (23) minus the 3 byte ATT header.
 */
static constexpr std::size_t DEFAULT_MTU = 20;

/**
 * CoreBluetooth caps writes at 512 bytes.
 */
static constexpr std::size_t MAX_MTU = 512;

/**
 * Delay before retrying after a failed connection attempt.
 */
static constexpr int64_t RECONNECT_DELAY_NS = 1 * NSEC_PER_SEC;

static void *const kQueueSpecificKey = (void *)&kQueueSpecificKey;

/**
 * Describes one of the supported BLE serial profiles.
 */
struct BleSerialProfile {
  const char *name;
  CBUUID *service;
  CBUUID *write_characteristic;
  CBUUID *notify_characteristic;
};

static const BleSerialProfile *
GetProfiles() noexcept
{
  /* in order of preference; Nordic UART Service is the de-facto
     standard for new devices */
  static const BleSerialProfile profiles[] = {
    {
      "Nordic UART Service",
      [CBUUID UUIDWithString:@(BluetoothUuids::NORDIC_UART_SERVICE)],
      [CBUUID UUIDWithString:@(BluetoothUuids::NORDIC_UART_RX_CHARACTERISTIC)],
      [CBUUID UUIDWithString:@(BluetoothUuids::NORDIC_UART_TX_CHARACTERISTIC)],
    },
    {
      "Microchip/ISSC UART",
      [CBUUID UUIDWithString:@(BluetoothUuids::ISSC_UART_SERVICE)],
      [CBUUID UUIDWithString:@(BluetoothUuids::ISSC_UART_RX_CHARACTERISTIC)],
      [CBUUID UUIDWithString:@(BluetoothUuids::ISSC_UART_TX_CHARACTERISTIC)],
    },
    {
      "HM-10",
      [CBUUID UUIDWithString:@(BluetoothUuids::HM10_SERVICE)],
      [CBUUID UUIDWithString:@(BluetoothUuids::HM10_RX_TX_CHARACTERISTIC)],
      [CBUUID UUIDWithString:@(BluetoothUuids::HM10_RX_TX_CHARACTERISTIC)],
    },
    { nullptr, nil, nil, nil },
  };

  return profiles;
}

static NSArray<CBUUID *> *
GetServiceUuids() noexcept
{
  NSMutableArray<CBUUID *> *uuids = [NSMutableArray array];
  for (const auto *p = GetProfiles(); p->name != nullptr; ++p)
    [uuids addObject:p->service];
  return uuids;
}

@implementation XCSBleSerialPort {
  XCSBluetoothManager *_manager;
  dispatch_queue_t _queue;

  const BleSerialProfile *_profile;
  CBCharacteristic *_writeCharacteristic;
  CBCharacteristic *_notifyCharacteristic;
  CBCharacteristicWriteType _writeType;
  std::size_t _mtu;

  std::atomic<PortState> _state;
  std::atomic<PortListener *> _listener;
  std::atomic<DataHandler *> _handler;

  /** protects the transmit buffer and the flags below */
  Mutex _mutex;
  Cond _cond;
  std::array<std::byte, WRITE_BUFFER_SIZE> _buffer;
  std::size_t _head, _tail;

  /** a write-with-response is pending */
  bool _writeInFlight;

  /** the last asynchronous write failed */
  bool _writeError;

  /** a pump block has been queued */
  bool _pumpScheduled;

  /* the following are only accessed on the queue */
  BOOL _closed;
  BOOL _connecting;
}

- (instancetype)initWithManager:(XCSBluetoothManager *)manager
                     identifier:(nullable NSUUID *)identifier
                     targetName:(nullable NSString *)targetName
{
  self = [super init];
  if (self) {
    _manager = manager;
    _queue = manager.queue;
    dispatch_queue_set_specific(_queue, kQueueSpecificKey,
                                kQueueSpecificKey, nullptr);
    _identifier = identifier;
    _targetName = targetName;
    _state = PortState::LIMBO;
    _mtu = DEFAULT_MTU;
    _head = _tail = 0;
    _writeInFlight = _writeError = _pumpScheduled = false;
  }
  return self;
}

- (const char *)debugName
{
  NSString *name = _peripheral.name;
  if (name.length == 0)
    name = _targetName;
  if (name.length == 0)
    name = _identifier.UUIDString;
  return name != nil ? name.UTF8String : "?";
}

/* public thread-safe API */

- (void)setListener:(nullable PortListener *)listener
{
  _listener = listener;
}

- (void)setInputListener:(nullable DataHandler *)handler
{
  _handler = handler;
}

- (PortState)state
{
  return _state;
}

- (std::size_t)write:(std::span<const std::byte>)src
{
  if (src.empty())
    return 0;

  std::unique_lock lock{_mutex};

  const auto deadline = std::chrono::steady_clock::now() + WRITE_TIMEOUT;

  while (true) {
    if (_writeError) {
      _writeError = false;
      throw std::runtime_error{"BLE write failed"};
    }

    if (_state != PortState::READY)
      throw std::runtime_error{"BLE port not connected"};

    if (_head > 0 && _tail == _buffer.size()) {
      /* make room at the end */
      std::copy(_buffer.begin() + _head, _buffer.begin() + _tail,
                _buffer.begin());
      _tail -= _head;
      _head = 0;
    }

    if (_tail < _buffer.size())
      break;

    if (_cond.wait_until(lock, deadline) == std::cv_status::timeout)
      throw DeviceTimeout{"BLE write timeout"};
  }

  const std::size_t nbytes = std::min(src.size(), _buffer.size() - _tail);
  std::copy_n(src.begin(), nbytes, _buffer.begin() + _tail);
  _tail += nbytes;

  if (!_pumpScheduled) {
    _pumpScheduled = true;
    dispatch_async(_queue, ^{ [self pump]; });
  }

  return nbytes;
}

- (BOOL)drain
{
  std::unique_lock lock{_mutex};

  const auto deadline = std::chrono::steady_clock::now() + WRITE_TIMEOUT;

  while (_head != _tail || _writeInFlight) {
    if (_writeError || _state != PortState::READY)
      break;

    if (_cond.wait_until(lock, deadline) == std::cv_status::timeout)
      return NO;
  }

  if (_writeError) {
    _writeError = false;
    return NO;
  }

  return _head == _tail && !_writeInFlight;
}

- (void)close
{
  dispatch_block_t block = ^{
    _closed = YES;
    _listener = nullptr;
    _handler = nullptr;

    if (_peripheral != nil) {
      _peripheral.delegate = nil;
      if ([_manager isPoweredOn])
        [_manager.central cancelPeripheralConnection:_peripheral];
    }

    [self resetConnection];
    _state = PortState::FAILED;
    [_manager portClosed:self];
  };

  if (dispatch_get_specific(kQueueSpecificKey) == kQueueSpecificKey)
    block();
  else
    dispatch_sync(_queue, block);
}

/* internal helpers (queue only) */

- (void)stateChanged
{
  if (PortListener *listener = _listener.load())
    listener->PortStateChanged();
}

- (void)fail:(const char *)msg
{
  LogFmt("BLE: {}: {}", [self debugName], msg);

  [self resetConnection];
  _state = PortState::FAILED;

  if (PortListener *listener = _listener.load())
    listener->PortError(msg);

  [self stateChanged];
}

/**
 * Forget the GATT state and discard pending writes.
 */
- (void)resetConnection
{
  {
    const std::lock_guard lock{_mutex};
    _head = _tail = 0;
    _writeInFlight = false;
    _cond.notify_all();
  }

  _profile = nullptr;
  _writeCharacteristic = nil;
  _notifyCharacteristic = nil;
  _mtu = DEFAULT_MTU;
}

- (BOOL)canSendWriteWithoutResponse
{
  if (@available(iOS 11.0, macOS 10.13, *))
    return _peripheral.canSendWriteWithoutResponse;
  return YES;
}

/**
 * Transmit as much of the buffer as CoreBluetooth accepts right
 * now.  Continued from the "ready" delegate callbacks.
 */
- (void)pump
{
  std::unique_lock lock{_mutex};
  _pumpScheduled = false;

  while (true) {
    if (_closed || _state != PortState::READY ||
        _writeCharacteristic == nil || _peripheral == nil)
      break;

    if (_head == _tail)
      break;

    if (_writeType == CBCharacteristicWriteWithoutResponse) {
      if (![self canSendWriteWithoutResponse])
        /* peripheralIsReadyToSendWriteWithoutResponse: will call
           pump again */
        break;
    } else if (_writeInFlight)
      /* didWriteValueForCharacteristic: will call pump again */
      break;

    const std::size_t nbytes = std::min(_tail - _head, _mtu);
    NSData *chunk = [NSData dataWithBytes:&_buffer[_head] length:nbytes];
    _head += nbytes;
    if (_head == _tail)
      _head = _tail = 0;

    if (_writeType == CBCharacteristicWriteWithResponse)
      _writeInFlight = true;

    /* wake up write() and drain() */
    _cond.notify_all();

    CBPeripheral *peripheral = _peripheral;
    CBCharacteristic *characteristic = _writeCharacteristic;
    const CBCharacteristicWriteType type = _writeType;

    lock.unlock();
    [peripheral writeValue:chunk forCharacteristic:characteristic type:type];
    lock.lock();
  }
}

/* connection management (queue only) */

- (void)start
{
  if (_closed || ![_manager isPoweredOn])
    /* managerPoweredOn will call us again */
    return;

  if (_peripheral == nil && _identifier != nil) {
    NSArray<CBPeripheral *> *known =
      [_manager.central retrievePeripheralsWithIdentifiers:@[_identifier]];
    if (known.count > 0)
      _peripheral = known.firstObject;
  }

  if (_peripheral == nil) {
    LogFmt("BLE: {}: waiting for the device to be discovered",
           [self debugName]);
    return;
  }

  _peripheral.delegate = self;

  if (!_connecting) {
    _connecting = YES;
    LogFmt("BLE: {}: connecting", [self debugName]);
    /* note: CoreBluetooth connection requests never time out; this
       request stays pending until the peripheral shows up, which
       gives us automatic reconnection for free */
    [_manager.central connectPeripheral:_peripheral options:nil];
  }
}

- (void)managerPoweredOn
{
  _connecting = NO;
  [self start];
}

- (void)managerPoweredOff
{
  _connecting = NO;
  [self resetConnection];

  if (_state != PortState::FAILED) {
    _state = PortState::LIMBO;
    [self stateChanged];
  }
}

- (BOOL)needsScan
{
  return !_closed && _peripheral == nil;
}

- (BOOL)matchesDiscoveredPeripheral:(CBPeripheral *)peripheral
                               name:(nullable NSString *)name
{
  if (_identifier != nil)
    return [peripheral.identifier isEqual:_identifier];

  return _targetName != nil && name != nil &&
    [name isEqualToString:_targetName];
}

- (void)attachDiscoveredPeripheral:(CBPeripheral *)peripheral
{
  _peripheral = peripheral;
  _identifier = peripheral.identifier;
  LogFmt("BLE: {}: discovered as {}", [self debugName],
         _identifier.UUIDString.UTF8String);
  [self start];
}

- (void)didConnect
{
  _connecting = NO;
  LogFmt("BLE: {}: connected, discovering services", [self debugName]);
  [_peripheral discoverServices:GetServiceUuids()];
}

- (void)didFailToConnect:(nullable NSError *)error
{
  _connecting = NO;
  LogFmt("BLE: {}: connection failed: {}", [self debugName],
         error != nil ? error.localizedDescription.UTF8String : "?");

  if (_closed)
    return;

  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, RECONNECT_DELAY_NS),
                 _queue, ^{ [self start]; });
}

- (void)didDisconnect:(nullable NSError *)error
{
  _connecting = NO;
  LogFmt("BLE: {}: disconnected{}{}", [self debugName],
         error != nil ? ": " : "",
         error != nil ? error.localizedDescription.UTF8String : "");

  [self resetConnection];

  if (_closed)
    return;

  if (_state != PortState::LIMBO) {
    _state = PortState::LIMBO;
    [self stateChanged];
  }

  /* reconnect */
  [self start];
}

/* CBPeripheralDelegate (queue) */

- (void)peripheral:(CBPeripheral *)peripheral
didDiscoverServices:(nullable NSError *)error
{
  if (_closed)
    return;

  if (error != nil) {
    [self fail:"Discovering GATT services failed"];
    return;
  }

  for (const auto *p = GetProfiles(); p->name != nullptr; ++p) {
    for (CBService *service in peripheral.services) {
      if ([service.UUID isEqual:p->service]) {
        _profile = p;
        LogFmt("BLE: {}: using {}", [self debugName], p->name);
        [peripheral discoverCharacteristics:nil forService:service];
        return;
      }
    }
  }

  [self fail:"BLE serial service not found"];
}

- (void)peripheral:(CBPeripheral *)peripheral
didDiscoverCharacteristicsForService:(CBService *)service
             error:(nullable NSError *)error
{
  if (_closed || _profile == nullptr)
    return;

  if (error != nil) {
    [self fail:"Discovering GATT characteristics failed"];
    return;
  }

  if (![service.UUID isEqual:_profile->service])
    return;

  CBCharacteristic *write = nil, *notify = nil;
  for (CBCharacteristic *c in service.characteristics) {
    if ([c.UUID isEqual:_profile->write_characteristic])
      write = c;
    if ([c.UUID isEqual:_profile->notify_characteristic])
      notify = c;
  }

  if (write == nil || notify == nil) {
    [self fail:"BLE serial characteristics not found"];
    return;
  }

  if ((notify.properties &
       (CBCharacteristicPropertyNotify|CBCharacteristicPropertyIndicate)) == 0) {
    [self fail:"BLE serial characteristic does not support notifications"];
    return;
  }

  _writeCharacteristic = write;
  _notifyCharacteristic = notify;

  /* prefer write-without-response: much higher throughput, and
     CoreBluetooth tells us when it can accept the next chunk */
  _writeType = (write.properties & CBCharacteristicPropertyWriteWithoutResponse) != 0
    ? CBCharacteristicWriteWithoutResponse
    : CBCharacteristicWriteWithResponse;

  [peripheral setNotifyValue:YES forCharacteristic:notify];
}

- (void)peripheral:(CBPeripheral *)peripheral
didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)characteristic
             error:(nullable NSError *)error
{
  if (_closed || _notifyCharacteristic == nil ||
      ![characteristic.UUID isEqual:_notifyCharacteristic.UUID])
    return;

  if (error != nil) {
    [self fail:"Could not enable GATT characteristic notification"];
    return;
  }

  if (!characteristic.isNotifying)
    /* this is the acknowledgement of our setNotifyValue:NO */
    return;

  std::size_t mtu = [peripheral maximumWriteValueLengthForType:_writeType];
  _mtu = std::clamp(mtu, DEFAULT_MTU, MAX_MTU);

  _state = PortState::READY;
  LogFmt("BLE: {}: ready ({}, {}, chunk size {})", [self debugName],
         _profile->name,
         _writeType == CBCharacteristicWriteWithoutResponse
         ? "write without response" : "write with response",
         _mtu);
  [self stateChanged];

  /* flush anything that was queued while connecting */
  [self pump];
}

- (void)peripheral:(CBPeripheral *)peripheral
didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic
             error:(nullable NSError *)error
{
  if (_closed || _notifyCharacteristic == nil)
    return;

  if (error != nil) {
    LogFmt("BLE: {}: notification error: {}", [self debugName],
           error.localizedDescription.UTF8String);
    return;
  }

  if (![characteristic.UUID isEqual:_notifyCharacteristic.UUID])
    return;

  NSData *value = characteristic.value;
  if (value.length == 0)
    return;

  if (DataHandler *handler = _handler.load())
    handler->DataReceived({(const std::byte *)value.bytes, value.length});
}

- (void)peripheral:(CBPeripheral *)peripheral
didWriteValueForCharacteristic:(CBCharacteristic *)characteristic
             error:(nullable NSError *)error
{
  {
    const std::lock_guard lock{_mutex};
    _writeInFlight = false;
    if (error != nil) {
      LogFmt("BLE: {}: write failed: {}", [self debugName],
             error.localizedDescription.UTF8String);
      _writeError = true;
      _head = _tail = 0;
    }
    _cond.notify_all();
  }

  [self pump];
}

- (void)peripheralIsReadyToSendWriteWithoutResponse:(CBPeripheral *)peripheral
{
  [self pump];
}

- (void)peripheral:(CBPeripheral *)peripheral
 didModifyServices:(NSArray<CBService *> *)invalidatedServices
{
  if (_closed)
    return;

  LogFmt("BLE: {}: services changed, rediscovering", [self debugName]);
  [self resetConnection];

  if (_state == PortState::READY) {
    _state = PortState::LIMBO;
    [self stateChanged];
  }

  [peripheral discoverServices:GetServiceUuids()];
}

@end

/* C++ facade */

PortBridge::PortBridge(XCSBleSerialPort *_port) noexcept
  :port(_port)
{
}

PortBridge::~PortBridge() noexcept
{
  [port close];
  port = nil;
}

void
PortBridge::setListener(PortListener *listener) noexcept
{
  [port setListener:listener];
}

void
PortBridge::setInputListener(DataHandler *handler) noexcept
{
  [port setInputListener:handler];
}

PortState
PortBridge::getState() const noexcept
{
  return [port state];
}

bool
PortBridge::drain()
{
  return [port drain];
}

std::size_t
PortBridge::write(std::span<const std::byte> src)
{
  return [port write:src];
}
