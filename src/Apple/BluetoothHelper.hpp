// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "DetectDeviceListener.hpp"

#ifdef __OBJC__
@class XCSBluetoothManager;
#else
typedef struct objc_object XCSBluetoothManager;
#endif

class PortBridge;

/**
 * Access to Bluetooth LE via CoreBluetooth (iOS and macOS).
 *
 * iOS has no public API for Bluetooth Classic (RFCOMM) connections
 * to non-MFi devices, so only BLE serial bridges (Nordic UART
 * Service, Microchip/ISSC transparent UART, HM-10) are supported.
 *
 * "Addresses" are CoreBluetooth peripheral identifier UUID strings;
 * they are stable for a given peripheral on a given host device but
 * differ between hosts.
 */
class BluetoothHelper final {
  XCSBluetoothManager *manager;

public:
  BluetoothHelper() noexcept;
  ~BluetoothHelper() noexcept;

  BluetoothHelper(const BluetoothHelper &) = delete;
  BluetoothHelper &operator=(const BluetoothHelper &) = delete;

  /**
   * Does this host have a Bluetooth LE radio at all?  Returns true
   * while the CoreBluetooth state is still unknown.
   */
  [[gnu::pure]]
  bool HasBluetoothSupport() const noexcept;

  /**
   * Is Bluetooth switched on (and authorised)?
   */
  [[gnu::pure]]
  bool IsEnabled() const noexcept;

  constexpr bool HasLe() const noexcept {
    return true;
  }

  /**
   * Look up the human-readable name of a peripheral.
   *
   * @return the name or nullptr if unknown; the returned pointer
   * points to thread-local storage and is invalidated by the next
   * call from the same thread
   */
  [[gnu::pure]]
  const char *GetNameFromAddress(const char *address) const noexcept;

  /**
   * Start scanning for BLE peripherals and report them to the given
   * listener.  The listener must stay valid until it is removed.
   */
  void AddDetectDeviceListener(DetectDeviceListener &l) noexcept;
  void RemoveDetectDeviceListener(DetectDeviceListener &l) noexcept;

  /**
   * Open a connection to a BLE serial bridge (NUS, ISSC or HM-10).
   * The connection is established asynchronously; the returned
   * #PortBridge starts in state LIMBO.
   *
   * Throws on error.
   *
   * @param address the peripheral identifier UUID, or (if it is not
   * a valid UUID) the advertised device name
   */
  PortBridge *connectBleSerial(const char *address);
};

/**
 * The global BluetoothHelper instance; nullptr if Bluetooth is not
 * available.
 */
extern BluetoothHelper *bluetooth_helper;
