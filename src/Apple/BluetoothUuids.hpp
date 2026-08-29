// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

/**
 * Bluetooth LE service/characteristic UUIDs known to XCSoar.
 *
 * Keep in sync with android/src/BluetoothUuids.java.
 */
namespace BluetoothUuids {

/**
 * Nordic UART Service (NUS) - a BLE serial bridge with separate RX
 * (write) and TX (notify) characteristics.  Used by e.g. Naviter
 * dongles, SoftRF, ESP32-based adapters.
 */
constexpr const char *NORDIC_UART_SERVICE =
  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";

/** NUS RX characteristic: XCSoar writes data to it */
constexpr const char *NORDIC_UART_RX_CHARACTERISTIC =
  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";

/** NUS TX characteristic: XCSoar receives notifications from it */
constexpr const char *NORDIC_UART_TX_CHARACTERISTIC =
  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

/**
 * Microchip/ISSC transparent UART service (e.g. BlueFly Vario BLE).
 */
constexpr const char *ISSC_UART_SERVICE =
  "49535343-FE7D-4AE5-8FA9-9FAFD205E455";

/** ISSC RX characteristic: XCSoar writes data to it */
constexpr const char *ISSC_UART_RX_CHARACTERISTIC =
  "49535343-8841-43F4-A8D4-ECBE34729BB3";

/** ISSC TX characteristic: XCSoar receives notifications from it */
constexpr const char *ISSC_UART_TX_CHARACTERISTIC =
  "49535343-1E4D-4BD9-BA61-23C647249616";

/**
 * HM-10 and compatible modules: one characteristic for both
 * directions.
 */
constexpr const char *HM10_SERVICE = "FFE0";
constexpr const char *HM10_RX_TX_CHARACTERISTIC = "FFE1";

constexpr const char *HEART_RATE_SERVICE = "180D";

/** Flytec Sensbox */
constexpr const char *FLYTEC_SENSBOX_SERVICE =
  "ABA27100-143B-4B81-A444-EDCD0000F020";

} // namespace BluetoothUuids
