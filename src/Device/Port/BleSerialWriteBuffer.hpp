// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "thread/Mutex.hxx"
#include "thread/Cond.hxx"

#include <array>
#include <chrono>
#include <cstddef>
#include <span>

/**
 * Transmit buffer for a Bluetooth LE serial connection (Nordic UART
 * Service, HM-10, ...).
 *
 * The producer (the device thread calling Port::Write()) appends
 * data and blocks while the buffer is full; the consumer (the
 * platform's BLE stack callback) takes chunks of at most one MTU
 * whenever the link can accept another write.  This is the
 * platform-neutral half of the transmit path; the CoreBluetooth glue
 * (XCSBleSerialPort) only decides *when* the next chunk may be sent.
 *
 * All methods are thread-safe.
 */
class BleSerialWriteBuffer {
public:
  /**
   * Size of the buffer.  Keeping it small gives natural backpressure
   * towards the device drivers, which are request/response driven
   * (e.g. FLARM IGC downloads).
   */
  static constexpr std::size_t CAPACITY = 4096;

  /**
   * The ATT default MTU (23) minus the 3 byte ATT header.
   */
  static constexpr std::size_t DEFAULT_MTU = 20;

  /**
   * Longest characteristic value Bluetooth allows.
   */
  static constexpr std::size_t MAX_MTU = 512;

private:
  mutable Mutex mutex;
  Cond cond;

  std::array<std::byte, CAPACITY> buffer;
  std::size_t head = 0, tail = 0;

  std::size_t mtu = DEFAULT_MTU;

  /** the link accepts writes (port state READY) */
  bool ready = false;

  /** a chunk was handed out with "with_response" and has not been
      confirmed yet */
  bool in_flight = false;

  /** the last asynchronous write failed; reported by the next
      Write() or Drain() call */
  bool error = false;

  /** pending data was discarded because the link went away;
      reported by the next Drain() call, unless new data gets
      written first */
  bool discarded = false;

public:
  /**
   * Set the maximum chunk size; clamped to [DEFAULT_MTU, MAX_MTU].
   */
  void SetMtu(std::size_t _mtu) noexcept;

  [[gnu::pure]]
  std::size_t GetMtu() const noexcept;

  /**
   * Declare the link ready (or not).  Becoming "not ready" (the
   * peripheral disconnected) discards all pending data and the
   * in-flight state, because the device will never receive them;
   * the next Drain() reports that loss.  A pending write error is
   * folded into that report instead of failing the first Write()
   * on the next connection.
   */
  void SetReady(bool _ready) noexcept;

  [[gnu::pure]]
  bool IsReady() const noexcept;

  /**
   * An asynchronous write failed.  Pending data is discarded and the
   * error is reported by the next Write() or Drain() call.
   */
  void SetError() noexcept;

  /**
   * Append data, blocking while the buffer is full.
   *
   * Throws std::runtime_error if the link is not ready or the last
   * asynchronous write failed, DeviceTimeout if no room became
   * available within the timeout.
   *
   * @return the number of bytes accepted (always > 0)
   */
  std::size_t Write(std::span<const std::byte> src,
                    std::chrono::steady_clock::duration timeout);

  /**
   * Take the next chunk to be transmitted.
   *
   * @param with_response the chunk will be sent as
   * write-with-response; no further chunk is handed out until
   * ChunkCompleted() is called
   * @return the number of bytes copied to dest (at most GetMtu()),
   * or 0 if there is nothing to send right now
   */
  std::size_t TakeChunk(std::span<std::byte> dest, bool with_response) noexcept;

  /**
   * A chunk taken with "with_response" has been acknowledged (or has
   * failed) by the peripheral.
   */
  void ChunkCompleted(bool success) noexcept;

  /**
   * Wait until everything has been handed out (and, for
   * write-with-response, acknowledged).
   *
   * @return false on error or timeout
   */
  bool Drain(std::chrono::steady_clock::duration timeout) noexcept;

  [[gnu::pure]]
  bool IsEmpty() const noexcept;

  [[gnu::pure]]
  std::size_t GetPending() const noexcept;

  [[gnu::pure]]
  bool HasInFlight() const noexcept;

private:
  /** caller must hold the mutex */
  void ClearLocked() noexcept;
};
