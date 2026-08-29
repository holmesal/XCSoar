// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "Device/Port/BleSerialWriteBuffer.hpp"
#include "Device/Error.hpp"
#include "TestUtil.hpp"

#include <chrono>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static constexpr auto SHORT = 50ms;
static constexpr auto LONG = 5s;

/**
 * A deterministic byte sequence so ordering and integrity can be
 * checked after the data went through the buffer in chunks.
 */
static std::vector<std::byte>
MakePattern(std::size_t n, std::uint8_t seed = 0)
{
  std::vector<std::byte> v(n);
  std::uint8_t x = seed;
  for (auto &b : v) {
    x = x * 31 + 7;
    b = std::byte{x};
  }
  return v;
}

static std::size_t
WriteAll(BleSerialWriteBuffer &b, std::span<const std::byte> src)
{
  std::size_t total = 0;
  while (!src.empty()) {
    const std::size_t n = b.Write(src, LONG);
    total += n;
    src = src.subspan(n);
  }
  return total;
}

/**
 * Take chunks until the buffer is empty, returning everything that
 * came out and the largest chunk seen.
 */
static std::vector<std::byte>
TakeAll(BleSerialWriteBuffer &b, bool with_response, std::size_t &max_chunk)
{
  std::vector<std::byte> out;
  std::array<std::byte, 600> chunk;
  max_chunk = 0;
  while (true) {
    const std::size_t n = b.TakeChunk(chunk, with_response);
    if (n == 0)
      break;
    max_chunk = std::max(max_chunk, n);
    out.insert(out.end(), chunk.begin(), chunk.begin() + n);
    if (with_response)
      b.ChunkCompleted(true);
  }
  return out;
}

static void
TestNotReady()
{
  BleSerialWriteBuffer b;
  ok1(!b.IsReady());
  ok1(b.IsEmpty());

  const auto data = MakePattern(10);
  bool threw = false;
  try {
    b.Write(data, SHORT);
  } catch (const std::runtime_error &) {
    threw = true;
  }
  ok1(threw);

  std::array<std::byte, 32> chunk;
  ok1(b.TakeChunk(chunk, false) == 0);

  /* nothing pending, so draining a not-ready buffer succeeds */
  ok1(b.Drain(SHORT));
}

static void
TestChunkingAndOrdering()
{
  BleSerialWriteBuffer b;
  b.SetReady(true);
  ok1(b.GetMtu() == BleSerialWriteBuffer::DEFAULT_MTU);

  const auto data = MakePattern(1000, 1);
  ok1(WriteAll(b, data) == data.size());
  ok1(b.GetPending() == data.size());

  std::size_t max_chunk;
  const auto out = TakeAll(b, false, max_chunk);
  ok1(out == data);
  ok1(max_chunk == BleSerialWriteBuffer::DEFAULT_MTU);
  ok1(b.IsEmpty());

  /* a larger MTU is honoured, and clamped to the BLE maximum */
  b.SetMtu(200);
  ok1(b.GetMtu() == 200);
  WriteAll(b, data);
  TakeAll(b, false, max_chunk);
  ok1(max_chunk == 200);

  b.SetMtu(100000);
  ok1(b.GetMtu() == BleSerialWriteBuffer::MAX_MTU);
  b.SetMtu(1);
  ok1(b.GetMtu() == BleSerialWriteBuffer::DEFAULT_MTU);

  /* the destination span limits the chunk too */
  b.SetMtu(512);
  WriteAll(b, data);
  std::array<std::byte, 7> small;
  ok1(b.TakeChunk(small, false) == 7);
  ok1(std::equal(small.begin(), small.end(), data.begin()));
}

static void
TestInterleavedWritesAndChunks()
{
  /* writes and chunk-taking alternate, with the buffer wrapping
     (compacting) several times; the output must be the exact
     concatenation of the input */
  BleSerialWriteBuffer b;
  b.SetReady(true);
  b.SetMtu(244);

  const auto data = MakePattern(5 * BleSerialWriteBuffer::CAPACITY, 2);
  std::vector<std::byte> out;
  std::array<std::byte, 512> chunk;

  std::span<const std::byte> src = data;
  std::size_t step = 1;
  while (!src.empty() || !b.IsEmpty()) {
    if (!src.empty()) {
      const std::size_t want = std::min<std::size_t>(src.size(), 37 * step);
      const std::size_t n = b.Write(src.first(want), LONG);
      src = src.subspan(n);
    }

    if (step % 3 != 0) {
      const std::size_t n = b.TakeChunk(chunk, false);
      out.insert(out.end(), chunk.begin(), chunk.begin() + n);
    }

    step = step % 11 + 1;
  }

  ok1(out.size() == data.size());
  ok1(out == data);
  ok1(b.IsEmpty());
}

static void
TestPartialWrite()
{
  BleSerialWriteBuffer b;
  b.SetReady(true);

  /* fill up to one byte short of the capacity */
  const auto data = MakePattern(BleSerialWriteBuffer::CAPACITY - 1, 3);
  ok1(b.Write(data, SHORT) == data.size());

  /* the next write only fits partially: Port::FullWrite() handles
     that by calling again */
  const auto more = MakePattern(100, 4);
  ok1(b.Write(more, SHORT) == 1);
  ok1(b.GetPending() == BleSerialWriteBuffer::CAPACITY);
}

static void
TestBackpressure()
{
  BleSerialWriteBuffer b;
  b.SetReady(true);
  b.SetMtu(512);

  const auto data = MakePattern(BleSerialWriteBuffer::CAPACITY, 5);
  ok1(b.Write(data, SHORT) == data.size());

  /* full: a write with a short timeout must time out */
  const auto more = MakePattern(16, 6);
  bool timed_out = false;
  try {
    b.Write(more, SHORT);
  } catch (const DeviceTimeout &) {
    timed_out = true;
  }
  ok1(timed_out);

  /* a consumer on another thread frees room; the blocked writer
     must wake up and succeed */
  std::thread consumer([&b]{
    std::this_thread::sleep_for(100ms);
    std::array<std::byte, 512> chunk;
    b.TakeChunk(chunk, false);
  });

  const auto t0 = std::chrono::steady_clock::now();
  std::size_t n = 0;
  bool threw = false;
  try {
    n = b.Write(more, LONG);
  } catch (...) {
    threw = true;
  }
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  consumer.join();

  ok1(!threw);
  ok1(n == more.size());
  ok1(elapsed >= 50ms && elapsed < LONG);
}

static void
TestWriteWithResponse()
{
  BleSerialWriteBuffer b;
  b.SetReady(true);
  b.SetMtu(20);

  const auto data = MakePattern(50, 7);
  WriteAll(b, data);

  std::array<std::byte, 64> chunk;
  ok1(b.TakeChunk(chunk, true) == 20);
  ok1(b.HasInFlight());

  /* no second chunk until the first one is acknowledged */
  ok1(b.TakeChunk(chunk, true) == 0);
  ok1(b.GetPending() == 30);

  /* and Drain() must wait for the acknowledgement */
  ok1(!b.Drain(SHORT));

  b.ChunkCompleted(true);
  ok1(!b.HasInFlight());
  ok1(b.TakeChunk(chunk, true) == 20);
  b.ChunkCompleted(true);
  ok1(b.TakeChunk(chunk, true) == 10);
  ok1(b.TakeChunk(chunk, true) == 0);
  ok1(b.HasInFlight());
  b.ChunkCompleted(true);
  ok1(b.Drain(SHORT));
  ok1(b.IsEmpty());
}

static void
TestWriteFailure()
{
  BleSerialWriteBuffer b;
  b.SetReady(true);

  const auto data = MakePattern(100, 8);
  WriteAll(b, data);

  std::array<std::byte, 64> chunk;
  ok1(b.TakeChunk(chunk, true) == 20);

  /* the peripheral rejected the write: pending data is dropped and
     the error is reported exactly once, by Drain() ... */
  b.ChunkCompleted(false);
  ok1(b.IsEmpty());
  ok1(!b.HasInFlight());
  ok1(!b.Drain(SHORT));
  ok1(b.Drain(SHORT));

  /* ... or by Write() */
  b.SetError();
  bool threw = false;
  try {
    b.Write(data, SHORT);
  } catch (const std::runtime_error &e) {
    /* DeviceTimeout is a runtime_error too; make sure it is the
       failure, not a timeout */
    threw = dynamic_cast<const DeviceTimeout *>(&e) == nullptr;
  }
  ok1(threw);

  /* the error is cleared by being reported; the buffer is usable
     again */
  ok1(b.Write(data, SHORT) == data.size());
}

static void
TestDrain()
{
  BleSerialWriteBuffer b;
  b.SetReady(true);
  b.SetMtu(512);

  ok1(b.Drain(SHORT));

  const auto data = MakePattern(1000, 9);
  WriteAll(b, data);

  /* nobody consumes: times out */
  ok1(!b.Drain(SHORT));

  /* a consumer on another thread empties it: Drain() returns once
     the last chunk is gone */
  std::thread consumer([&b]{
    std::this_thread::sleep_for(100ms);
    std::size_t max_chunk;
    TakeAll(b, false, max_chunk);
  });

  const auto t0 = std::chrono::steady_clock::now();
  const bool drained = b.Drain(LONG);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  consumer.join();

  ok1(drained);
  ok1(elapsed >= 50ms && elapsed < LONG);
  ok1(b.IsEmpty());
}

static void
TestDisconnectDiscards()
{
  BleSerialWriteBuffer b;
  b.SetReady(true);

  const auto data = MakePattern(100, 10);
  WriteAll(b, data);

  std::array<std::byte, 64> chunk;
  ok1(b.TakeChunk(chunk, true) == 20);
  ok1(b.HasInFlight());

  /* the peripheral went away: everything pending is discarded, a
     blocked Drain() gives up, and Write() fails until the link is
     ready again */
  std::thread disconnector([&b]{
    std::this_thread::sleep_for(100ms);
    b.SetReady(false);
  });
  ok1(!b.Drain(LONG));
  disconnector.join();

  ok1(b.IsEmpty());
  ok1(!b.HasInFlight());
  ok1(!b.IsReady());

  bool threw = false;
  try {
    b.Write(data, SHORT);
  } catch (const std::runtime_error &) {
    threw = true;
  }
  ok1(threw);

  /* reconnected: a fresh start, no stale error */
  b.SetReady(true);
  ok1(b.Write(data, SHORT) == data.size());
  ok1(b.TakeChunk(chunk, false) == 20);
  ok1(std::equal(chunk.begin(), chunk.begin() + 20, data.begin()));
}

static void
TestErrorAcrossReconnect()
{
  /* review finding: a write error right before a disconnect must not
     make the first Write() after the reconnect fail spuriously; the
     loss is reported by Drain() instead */
  BleSerialWriteBuffer b;
  b.SetReady(true);

  const auto data = MakePattern(40, 12);
  WriteAll(b, data);
  std::array<std::byte, 64> chunk;
  ok1(b.TakeChunk(chunk, true) == 20);
  b.ChunkCompleted(false);

  b.SetReady(false);
  b.SetReady(true);

  bool threw = false;
  try {
    ok1(b.Write(data, SHORT) == data.size());
  } catch (...) {
    threw = true;
  }
  ok1(!threw);

  /* new data was written, so there is no stale loss report either:
     Drain() fails only because of the pending data */
  ok1(!b.Drain(SHORT));
  std::size_t max_chunk;
  TakeAll(b, false, max_chunk);
  ok1(b.Drain(SHORT));
}

static void
TestDrainAfterDisconnectReportsLoss()
{
  /* review finding: Drain() must not claim success when the data it
     was waiting for was discarded by a disconnect, even if the link
     is already back */
  BleSerialWriteBuffer b;
  b.SetReady(true);

  const auto data = MakePattern(40, 13);
  WriteAll(b, data);

  b.SetReady(false);
  b.SetReady(true);
  ok1(b.IsEmpty());
  ok1(!b.Drain(SHORT));

  /* reported once */
  ok1(b.Drain(SHORT));

  /* a disconnect with nothing pending is not a loss */
  b.SetReady(false);
  b.SetReady(true);
  ok1(b.Drain(SHORT));
}

static void
TestConcurrentProducerConsumer()
{
  /* a producer pushing much more than the capacity while a consumer
     takes chunks at the same time: no loss, no reordering, no
     duplication */
  BleSerialWriteBuffer b;
  b.SetReady(true);
  b.SetMtu(185);

  const auto data = MakePattern(64 * 1024, 11);
  std::vector<std::byte> out;

  std::thread consumer([&]{
    std::array<std::byte, 512> chunk;
    while (out.size() < data.size()) {
      const std::size_t n = b.TakeChunk(chunk, false);
      if (n == 0) {
        std::this_thread::yield();
        continue;
      }
      out.insert(out.end(), chunk.begin(), chunk.begin() + n);
    }
  });

  const std::size_t written = WriteAll(b, data);
  ok1(b.Drain(LONG));
  consumer.join();

  ok1(written == data.size());
  ok1(out == data);
}

int
main()
{
  plan_tests(5 + 12 + 3 + 3 + 5 + 12 + 7 + 5 + 10 + 5 + 4 + 3);

  TestNotReady();
  TestChunkingAndOrdering();
  TestInterleavedWritesAndChunks();
  TestPartialWrite();
  TestBackpressure();
  TestWriteWithResponse();
  TestWriteFailure();
  TestDrain();
  TestDisconnectDiscards();
  TestErrorAcrossReconnect();
  TestDrainAfterDisconnectReportsLoss();
  TestConcurrentProducerConsumer();

  return exit_status();
}
