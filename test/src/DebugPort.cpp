// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "DebugPort.hpp"
#include "system/Args.hpp"
#include "Device/Config.hpp"
#include "Device/Port/Port.hpp"
#include "Device/Port/ConfiguredPort.hpp"

#ifdef __APPLE__
#include "Apple/BluetoothHelper.hpp"
#endif

#include <stdexcept>

DeviceConfig
ParsePortArgs(Args &args)
{
  DeviceConfig config;
  config.Clear();

  config.port_type = DeviceConfig::PortType::SERIAL;
  config.path = args.ExpectNextT().c_str();

#ifndef NDEBUG
  if (config.path.equals("dump")) {
    config = ParsePortArgs(args);
    config.dump_port = true;
    return config;
  }
#endif

  if (config.path.equals("k6bt")) {
    config = ParsePortArgs(args);
    config.k6bt = true;
    return config;
  }

#ifdef __APPLE__
  if (config.path.equals("ble")) {
    /* Bluetooth LE serial bridge (NUS/ISSC/HM-10); the argument is
       the CoreBluetooth peripheral identifier or, for convenience,
       the advertised device name */
    config.port_type = DeviceConfig::PortType::BLE_SERIAL;
    config.bluetooth_mac = args.ExpectNext();

    if (bluetooth_helper == nullptr)
      bluetooth_helper = new BluetoothHelper();

    return config;
  }
#endif

  if (config.path.equals("pty")) {
    config.port_type = DeviceConfig::PortType::PTY;
    config.path = args.ExpectNextT().c_str();
    return config;
  }

  if (config.path.equals("tcp")) {
    config.port_type = DeviceConfig::PortType::TCP_LISTENER;
    config.tcp_port = atoi(args.ExpectNext());
    return config;
  }

  if (config.path.equals("tcp_client")) {
    config.port_type = DeviceConfig::PortType::TCP_CLIENT;
    config.ip_address = args.ExpectNextT().c_str();
    config.tcp_port = atoi(args.ExpectNext());
    return config;
  }

  if (config.path.equals("udp")) {
    config.port_type = DeviceConfig::PortType::UDP_LISTENER;
    config.tcp_port = atoi(args.ExpectNext());
    return config;
  }

  if (config.UsesSpeed()) {
    char *endptr;
    config.baud_rate = strtoul(args.ExpectNext(), &endptr, 10);

    if (*endptr == ':')
      config.bulk_baud_rate = atoi(endptr + 1);
  }

  return config;
}

std::unique_ptr<Port>
DebugPort::Open(EventLoop &event_loop, Cares::Channel &cares,
                DataHandler &handler)
{
  auto port = OpenPort(event_loop, cares,
#ifdef __APPLE__
                       bluetooth_helper,
#endif
                       config, this, handler);
  if (port == nullptr)
    throw std::runtime_error("Failed to open port");

  return port;
}

void
DebugPort::PortStateChanged() noexcept
{
  if (listener != nullptr)
    listener->PortStateChanged();
}

void
DebugPort::PortError(const char *msg) noexcept
{
  fprintf(stderr, "Port error: %s\n", msg);

  if (listener != nullptr)
    listener->PortError(msg);
}
