// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include <memory>

class BluetoothHelper;
class Port;
class PortListener;
class DataHandler;

/**
 * Open a connection to a Bluetooth LE serial bridge (Nordic UART
 * Service, Microchip/ISSC transparent UART or HM-10) via
 * CoreBluetooth.
 *
 * Throws on error.
 *
 * @param address the CoreBluetooth peripheral identifier
 */
std::unique_ptr<Port>
OpenAppleBleSerialPort(BluetoothHelper &bluetooth_helper,
                       const char *address, PortListener *_listener,
                       DataHandler &_handler);
