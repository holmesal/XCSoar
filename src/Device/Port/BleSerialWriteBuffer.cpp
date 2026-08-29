// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "BleSerialWriteBuffer.hpp"
#include "Device/Error.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>

void
BleSerialWriteBuffer::SetMtu(std::size_t _mtu) noexcept
{
  const std::lock_guard lock{mutex};
  mtu = std::clamp(_mtu, DEFAULT_MTU, MAX_MTU);
}

std::size_t
BleSerialWriteBuffer::GetMtu() const noexcept
{
  const std::lock_guard lock{mutex};
  return mtu;
}

void
BleSerialWriteBuffer::ClearLocked() noexcept
{
  head = tail = 0;
  in_flight = false;
  cond.notify_all();
}

void
BleSerialWriteBuffer::SetReady(bool _ready) noexcept
{
  const std::lock_guard lock{mutex};
  ready = _ready;
  if (!ready) {
    /* the device will never receive what is pending (or confirm a
       failed write): report that once via Drain(), but do not fail
       the first Write() after the next connection */
    if (head != tail || in_flight || error)
      discarded = true;
    error = false;
    ClearLocked();
  } else
    cond.notify_all();
}

bool
BleSerialWriteBuffer::IsReady() const noexcept
{
  const std::lock_guard lock{mutex};
  return ready;
}

void
BleSerialWriteBuffer::SetError() noexcept
{
  const std::lock_guard lock{mutex};
  error = true;
  ClearLocked();
}

std::size_t
BleSerialWriteBuffer::Write(std::span<const std::byte> src,
                            std::chrono::steady_clock::duration timeout)
{
  if (src.empty())
    return 0;

  std::unique_lock lock{mutex};

  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (true) {
    if (error) {
      error = false;
      throw std::runtime_error{"BLE write failed"};
    }

    if (!ready)
      throw std::runtime_error{"BLE port not connected"};

    if (head > 0 && tail == buffer.size()) {
      /* make room at the end */
      std::copy(buffer.begin() + head, buffer.begin() + tail,
                buffer.begin());
      tail -= head;
      head = 0;
    }

    if (tail < buffer.size())
      break;

    if (cond.wait_until(lock, deadline) == std::cv_status::timeout)
      throw DeviceTimeout{"BLE write timeout"};
  }

  const std::size_t nbytes = std::min(src.size(), buffer.size() - tail);
  std::copy_n(src.begin(), nbytes, buffer.begin() + tail);
  tail += nbytes;

  /* the caller has moved on to new data; an old loss report would
     only confuse its Drain() */
  discarded = false;
  return nbytes;
}

std::size_t
BleSerialWriteBuffer::TakeChunk(std::span<std::byte> dest,
                                bool with_response) noexcept
{
  const std::lock_guard lock{mutex};

  if (!ready || head == tail)
    return 0;

  if (with_response && in_flight)
    return 0;

  const std::size_t nbytes = std::min({tail - head, mtu, dest.size()});
  std::copy_n(buffer.begin() + head, nbytes, dest.begin());
  head += nbytes;
  if (head == tail)
    head = tail = 0;

  if (with_response)
    in_flight = true;

  /* wake up Write() and Drain() */
  cond.notify_all();
  return nbytes;
}

void
BleSerialWriteBuffer::ChunkCompleted(bool success) noexcept
{
  const std::lock_guard lock{mutex};
  in_flight = false;
  if (!success) {
    error = true;
    head = tail = 0;
  }
  cond.notify_all();
}

bool
BleSerialWriteBuffer::Drain(std::chrono::steady_clock::duration timeout) noexcept
{
  std::unique_lock lock{mutex};

  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (head != tail || in_flight) {
    if (error || !ready)
      break;

    if (cond.wait_until(lock, deadline) == std::cv_status::timeout)
      return false;
  }

  if (error) {
    error = false;
    return false;
  }

  if (discarded) {
    discarded = false;
    return false;
  }

  return head == tail && !in_flight;
}

bool
BleSerialWriteBuffer::IsEmpty() const noexcept
{
  const std::lock_guard lock{mutex};
  return head == tail;
}

std::size_t
BleSerialWriteBuffer::GetPending() const noexcept
{
  const std::lock_guard lock{mutex};
  return tail - head;
}

bool
BleSerialWriteBuffer::HasInFlight() const noexcept
{
  const std::lock_guard lock{mutex};
  return in_flight;
}
