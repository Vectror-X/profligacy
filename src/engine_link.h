// SPDX-License-Identifier: AGPL-3.0-only
//
// engine_link.h - shared-memory contract between the plug-in and one
// ProfligacyEngine helper process (Vectror-X fork, multi-instance plan).
//
// MAME allows one machine per process. The first plug-in instance in a host
// process keeps running the engine in-process exactly as before; every further
// instance starts its own helper process, which owns a ProphecyEngine and talks
// to the plug-in only through the block below. No MAME and no JUCE types here.
//
// Threads: plug-in audio thread -> audioRing, plug-in message thread -> uiRing,
// helper -> txRing / audio / snapshots. Every ring is single-producer,
// single-consumer. Realtime plug-in calls never wait on the helper.
//
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace enginelink {

constexpr std::uint32_t kMagic   = 0x4B4C5250; // "PRLK"
constexpr std::uint32_t kVersion = 1;

static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "shared-memory atomics must be lock-free");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "shared-memory atomics must be lock-free");
static_assert(std::atomic<std::uint8_t>::is_always_lock_free, "shared-memory atomics must be lock-free");

// Bounded SPSC byte ring with monotonic indices. Records are all-or-nothing.
template <std::size_t Cap>
struct ByteRing
{
	static_assert((Cap & (Cap - 1)) == 0, "capacity must be a power of two");

	struct Header
	{
		std::uint32_t length = 0; // payload bytes
		std::uint8_t  kind = 0;
		std::uint8_t  pad[3] = {};
		std::uint64_t frame = 0;
		std::int32_t  a = 0, b = 0, c = 0, d = 0;
	};
	static constexpr std::size_t kHeader = sizeof(Header);
	static constexpr std::size_t kMaxPayload = Cap / 2;

	std::atomic<std::uint64_t> head{0}; // consumer position
	std::atomic<std::uint64_t> tail{0}; // producer position
	std::uint8_t data[Cap];

	std::size_t freeSpace() const
	{
		return Cap - static_cast<std::size_t>(tail.load(std::memory_order_relaxed)
			- head.load(std::memory_order_acquire));
	}
	std::size_t used() const
	{
		return static_cast<std::size_t>(tail.load(std::memory_order_acquire)
			- head.load(std::memory_order_relaxed));
	}

	void copyIn(std::uint64_t at, const void *src, std::size_t n)
	{
		const auto *p = static_cast<const std::uint8_t *>(src);
		for (std::size_t i = 0; i < n; ++i) data[(at + i) & (Cap - 1)] = p[i];
	}
	void copyOut(std::uint64_t at, void *dst, std::size_t n) const
	{
		auto *p = static_cast<std::uint8_t *>(dst);
		for (std::size_t i = 0; i < n; ++i) p[i] = data[(at + i) & (Cap - 1)];
	}

	// Producer: one complete record or nothing.
	bool pushRecord(const Header &h, const std::uint8_t *payload)
	{
		if (h.length > kMaxPayload) return false;
		const std::size_t need = kHeader + h.length;
		if (freeSpace() < need) return false;
		const auto t = tail.load(std::memory_order_relaxed);
		copyIn(t, &h, kHeader);
		if (h.length) copyIn(t + kHeader, payload, h.length);
		tail.store(t + need, std::memory_order_release);
		return true;
	}

	// Consumer: look at the next record without consuming it. `payload` must hold
	// kMaxPayload bytes. Returns false when the ring is empty.
	bool peekRecord(Header &h, std::uint8_t *payload) const
	{
		const auto hd = head.load(std::memory_order_relaxed);
		const auto t = tail.load(std::memory_order_acquire);
		if (t - hd < kHeader) return false;
		copyOut(hd, &h, kHeader);
		if (h.length > kMaxPayload || t - hd < kHeader + h.length) return false;
		if (h.length) copyOut(hd + kHeader, payload, h.length);
		return true;
	}
	void dropRecord(const Header &h)
	{
		head.store(head.load(std::memory_order_relaxed) + kHeader + h.length, std::memory_order_release);
	}

	// Plain byte stream (MIDI OUT): producer writes what fits, consumer drains.
	std::size_t pushBytes(const std::uint8_t *p, std::size_t n)
	{
		const std::size_t count = n < freeSpace() ? n : freeSpace();
		const auto t = tail.load(std::memory_order_relaxed);
		copyIn(t, p, count);
		tail.store(t + count, std::memory_order_release);
		return count;
	}
	std::size_t popBytes(std::uint8_t *out, std::size_t cap)
	{
		const auto hd = head.load(std::memory_order_relaxed);
		const auto t = tail.load(std::memory_order_acquire);
		std::size_t n = static_cast<std::size_t>(t - hd);
		if (n > cap) n = cap;
		copyOut(hd, out, n);
		head.store(hd + n, std::memory_order_release);
		return n;
	}
};

// Record kinds.
enum Kind : std::uint8_t
{
	kMidi = 1,        // uiRing: immediate MIDI (payload = bytes)
	kAdin = 2,        // uiRing: a = source, b = value
	kPanel = 3,       // uiRing: a = row, b = bit, c = length ms
	kMidiAtFrame = 4, // audioRing: payload = bytes, frame
	kAdinAtFrame = 5, // audioRing: a = source, b = value, frame
};

// Helper life cycle as seen by the plug-in.
enum State : std::uint32_t
{
	kLaunching = 0,   // process created, engine not started yet
	kStarted = 1,     // engine running, firmware booting
	kReady = 2,       // initializePlayback() succeeded
	kFailed = 3,      // engine did not start or firmware did not finish booting
	kExited = 4,      // helper left its loop (stop request, host gone, or engine ended)
};

// Single-writer snapshot guarded by a sequence counter (odd = being written).
struct SeqGuard
{
	std::atomic<std::uint32_t> seq{0};
	void beginWrite() { seq.fetch_add(1, std::memory_order_acq_rel); std::atomic_thread_fence(std::memory_order_release); }
	void endWrite()   { std::atomic_thread_fence(std::memory_order_release); seq.fetch_add(1, std::memory_order_acq_rel); }
	std::uint32_t beginRead() const { return seq.load(std::memory_order_acquire); }
	bool readOk(std::uint32_t start) const
	{
		std::atomic_thread_fence(std::memory_order_acquire);
		return (start & 1u) == 0 && seq.load(std::memory_order_acquire) == start;
	}
};

constexpr std::size_t kAudioCapacity = 32768; // frames, ~0.68 s at 48 kHz
constexpr std::size_t kArgBytes = 16384;

struct SharedBlock
{
	std::uint32_t magic = 0;
	std::uint32_t version = 0;
	std::uint64_t blockSize = 0;

	// ---- plug-in -> helper ----------------------------------------------------
	std::uint32_t argCount = 0;
	std::uint32_t firmwareHandshake = 1;
	char args[kArgBytes] = {};                       // NUL-separated MAME command line
	std::atomic<std::uint32_t> stopRequested{0};
	std::atomic<std::uint64_t> requestThrough{0};    // like ProphecyEngine::requestThroughFrame
	std::atomic<std::uint32_t> hostBlockFrames{0};   // like setHostBlockFrames
	std::atomic<std::uint64_t> readFirst{0};         // plug-in's current audio read position

	// ---- helper -> plug-in ----------------------------------------------------
	std::atomic<std::uint32_t> state{kLaunching};
	std::atomic<std::uint32_t> engineRunning{0};
	std::atomic<std::uint32_t> engineFinished{0};
	std::atomic<std::uint64_t> playbackOrigin{0};
	std::atomic<std::uint64_t> producedFrames{0};
	std::atomic<std::uint64_t> requestedFrames{0};
	std::atomic<std::uint64_t> droppedImmediate{0};
	std::atomic<std::uint64_t> droppedScheduled{0};
	std::atomic<std::uint64_t> droppedUiAdin{0};
	std::atomic<std::uint64_t> droppedAudioAdin{0};
	std::atomic<std::uint64_t> droppedScheduledAdin{0};
	std::atomic<std::uint64_t> droppedScheduledPanel{0};
	std::atomic<std::uint64_t> heartbeat{0};

	// Audio indexed by absolute native 48 kHz frame: slot = frame % kAudioCapacity.
	// Frames below audioEnd are published; the helper never writes further than
	// readFirst + kAudioCapacity - kAudioGuard, so a slot being read is never rewritten.
	std::atomic<std::uint64_t> audioEnd{0};
	float audioL[kAudioCapacity] = {};
	float audioR[kAudioCapacity] = {};

	// Front panel LEDs (12 banks of 8). `ledLatch` accumulates rising edges for
	// the editor's visual snapshot and is cleared by the plug-in when read.
	std::atomic<std::uint32_t> ledVersion{0};
	std::atomic<std::uint8_t>  led[12];
	std::atomic<std::uint8_t>  ledLatch[12];

	struct Lcd
	{
		SeqGuard guard;
		std::uint32_t rawVersion = 0;
		std::uint8_t row1[40] = {}, row2[40] = {}, cgram[64] = {};
		std::uint32_t hasText = 0;
		char text1[48] = {}, text2[48] = {};
	} lcd;

	struct Blob
	{
		SeqGuard guard;
		std::uint32_t dataVersion = 0;
		std::int32_t  pattern = -1;
		std::uint32_t length = 0;
		std::uint8_t  data[4096] = {};
	} program, arpeggio;

	ByteRing<16384>  uiRing;    // plug-in message thread -> helper
	ByteRing<65536>  audioRing; // plug-in audio thread -> helper
	ByteRing<262144> txRing;    // helper -> plug-in (firmware MIDI OUT)
};

constexpr std::uint64_t kAudioGuard = 4096;

} // namespace enginelink
