// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "Device/Port/State.hpp"

#include <cstddef>
#include <span>

#ifdef __OBJC__
@class XCSBleSerialPort;
#else
typedef struct objc_object XCSBleSerialPort;
#endif

class PortListener;
class DataHandler;

/**
 * C++ facade for a CoreBluetooth BLE serial connection.  Mirrors the
 * API of the Android #PortBridge so #ApplePort can stay a thin
 * wrapper (like #AndroidPort).
 *
 * All methods may be called from any thread.
 */
class PortBridge final {
  XCSBleSerialPort *port;

public:
  explicit PortBridge(XCSBleSerialPort *_port) noexcept;

  /**
   * Closes the connection.
   */
  ~PortBridge() noexcept;

  PortBridge(const PortBridge &) = delete;
  PortBridge &operator=(const PortBridge &) = delete;

  void setListener(PortListener *listener) noexcept;
  void setInputListener(DataHandler *handler) noexcept;

  [[gnu::pure]]
  PortState getState() const noexcept;

  /**
   * Wait until all buffered data has been sent.
   *
   * @return false on error or timeout
   */
  bool drain();

  constexpr unsigned getBaudRate() const noexcept {
    return 0;
  }

  constexpr bool setBaudRate([[maybe_unused]] unsigned baud_rate) noexcept {
    /* BLE has no baud rate; accept silently like the Android BLE
       serial port does */
    return true;
  }

  /**
   * Queue data for transmission.  Blocks while the transmit buffer
   * is full.
   *
   * Throws on error.
   *
   * @return the number of bytes accepted (always > 0)
   */
  std::size_t write(std::span<const std::byte> src);
};
