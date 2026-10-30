// SPDX-License-Identifier: AGPL-3.0-only
#include "remote_engine.h"

#include "engine_link.h"
#include "engine_link_platform.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

using namespace enginelink;
using Status = ProphecyEngine::InstanceStatus;

namespace {
std::atomic<std::uint64_t> g_regionSerial{0};

#if defined(_WIN32)
constexpr const char *kHelperName = "ProfligacyEngine.exe";
#else
constexpr const char *kHelperName = "ProfligacyEngine";
#endif
} // namespace

RemoteEngine::RemoteEngine() = default;

RemoteEngine::~RemoteEngine() { stop(); }

std::string RemoteEngine::helperPath()
{
	if (const char *env = std::getenv("PROFLIGACY_ENGINE_EXE"); env && *env) return env;
	const std::string dir = thisModuleDirectory();
	if (dir.empty()) return {};
#if defined(_WIN32)
	return dir + "\\" + kHelperName;
#else
	return dir + "/" + kHelperName;
#endif
}

bool RemoteEngine::start(const std::vector<std::string> &args, bool firmwareHandshake)
{
	if (m_status.load() != Status::NotStarted) return false;
	const std::string exe = helperPath();
	auto region = std::make_unique<SharedRegion>();
	const std::string name = makeRegionName(g_regionSerial.fetch_add(1) + 1);
	if (exe.empty() || !region->create(name, sizeof(SharedBlock)))
	{
		m_status.store(Status::Unavailable);
		return false;
	}
	auto *block = new (region->data()) SharedBlock();
	block->magic = kMagic;
	block->version = kVersion;
	block->blockSize = sizeof(SharedBlock);
	block->firmwareHandshake = firmwareHandshake ? 1u : 0u;
	std::size_t used = 0;
	for (const auto &a : args)
	{
		if (used + a.size() + 1 > kArgBytes) { m_status.store(Status::Unavailable); return false; }
		std::memcpy(block->args + used, a.c_str(), a.size() + 1);
		used += a.size() + 1;
		++block->argCount;
	}
	auto child = std::make_unique<ChildProcess>();
	if (!child->spawn(exe, {"--serve", name, "--host-pid", std::to_string(currentProcessId())}))
	{
		block->~SharedBlock();
		m_status.store(Status::Unavailable);
		return false;
	}
	m_region = std::move(region);
	m_block = block;
	m_child = std::move(child);
	m_status.store(Status::Active);
	return true;
}

void RemoteEngine::requestStop()
{
	m_stopRequested.store(true);
	if (m_block) m_block->stopRequested.store(1, std::memory_order_release);
}

void RemoteEngine::stop()
{
	if (!m_block) return;
	requestStop();
	// The helper calls ProphecyEngine::stop(), which lets MAME write this instance's
	// battery RAM (bounded at 3 s), then exits. Give it a little more than that.
	if (m_child && !m_child->waitForExit(6000)) m_child->kill();
	m_block->~SharedBlock();
	m_block = nullptr;
	m_region.reset();
	m_child.reset();
	m_status.store(Status::Stopped);
}

bool RemoteEngine::helperGone() const
{
	if (!m_block) return true;
	const auto s = m_block->state.load(std::memory_order_acquire);
	return s == kFailed || s == kExited || !m_child || !m_child->alive();
}

bool RemoteEngine::running() const
{
	return m_block && !helperGone() && m_block->engineFinished.load(std::memory_order_acquire) == 0;
}

bool RemoteEngine::finished() const
{
	return m_block && (m_block->engineFinished.load(std::memory_order_acquire) != 0 || helperGone());
}

ProphecyEngine::InstanceStatus RemoteEngine::instanceStatus() const { return m_status.load(); }

bool RemoteEngine::initializePlayback(bool)
{
	// The helper runs initializePlayback() itself as soon as its engine starts.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (m_block && !m_stopRequested.load())
	{
		const auto s = m_block->state.load(std::memory_order_acquire);
		if (s == kReady) return running();
		if (s == kFailed || s == kExited || !m_child->alive()) return false;
		if (std::chrono::steady_clock::now() >= deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return false;
}

bool RemoteEngine::readyForPlayback() const
{
	return m_block && m_block->state.load(std::memory_order_acquire) == kReady && running();
}

std::uint64_t RemoteEngine::playbackOrigin() const
{
	return m_block ? m_block->playbackOrigin.load(std::memory_order_acquire) : 0;
}

void RemoteEngine::setHostBlockFrames(std::uint32_t nativeFrames)
{
	if (m_block) m_block->hostBlockFrames.store(nativeFrames, std::memory_order_release);
}

void RemoteEngine::requestThroughFrame(std::uint64_t exclusiveEnd)
{
	if (!m_block) return;
	// Monotonic, like the grants the plug-in makes to an in-process engine.
	auto current = m_block->requestThrough.load(std::memory_order_relaxed);
	while (exclusiveEnd > current
		&& !m_block->requestThrough.compare_exchange_weak(current, exclusiveEnd, std::memory_order_acq_rel)) {}
}

std::uint64_t RemoteEngine::requestedFrames() const
{
	if (!m_block) return 0;
	return std::max(m_block->requestedFrames.load(std::memory_order_acquire),
		m_block->requestThrough.load(std::memory_order_acquire));
}

std::size_t RemoteEngine::readAtFrame(std::uint64_t first, float *left, float *right,
	std::size_t frames, bool offline)
{
	if (!left || !right || frames == 0) return 0;
	if (!m_block || frames > kAudioCapacity - kAudioGuard)
	{
		std::fill(left, left + frames, 0.0f);
		std::fill(right, right + frames, 0.0f);
		return 0;
	}
	auto prev = m_block->readFirst.load(std::memory_order_relaxed);
	if (first > prev) m_block->readFirst.store(first, std::memory_order_release);

	if (offline)
	{
		// Rendering waits for the window, like the in-process engine (bounded).
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (m_block->audioEnd.load(std::memory_order_acquire) < first + frames && running()
			&& !m_stopRequested.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::microseconds(200));
	}

	const auto end = m_block->audioEnd.load(std::memory_order_acquire);
	const std::size_t count = end > first
		? static_cast<std::size_t>(std::min<std::uint64_t>(end - first, frames)) : 0;
	for (std::size_t i = 0; i < count; ++i)
	{
		const auto slot = static_cast<std::size_t>((first + i) % kAudioCapacity);
		left[i] = m_block->audioL[slot];
		right[i] = m_block->audioR[slot];
	}
	std::fill(left + count, left + frames, 0.0f);
	std::fill(right + count, right + frames, 0.0f);
	return count;
}

bool RemoteEngine::pushMidi(const std::uint8_t *bytes, std::size_t n)
{
	if (bytes == nullptr || n == 0) return true;
	if (!readyForPlayback() || n > decltype(m_block->uiRing)::kMaxPayload)
	{
		m_localDroppedImmediate.fetch_add(n, std::memory_order_relaxed);
		return false;
	}
	if (m_uiProducer.test_and_set(std::memory_order_acquire)) return false;
	decltype(m_block->uiRing)::Header h;
	h.kind = kMidi;
	h.length = (std::uint32_t) n;
	const bool ok = m_block->uiRing.pushRecord(h, bytes);
	m_uiProducer.clear(std::memory_order_release);
	if (!ok) m_localDroppedImmediate.fetch_add(n, std::memory_order_relaxed);
	return ok;
}

bool RemoteEngine::pushMidiAtFrame(const std::uint8_t *bytes, std::size_t n, std::uint64_t frame)
{
	if (bytes == nullptr || n == 0) return true;
	if (!readyForPlayback() || n > decltype(m_block->audioRing)::kMaxPayload)
	{
		m_localDroppedScheduled.fetch_add(n, std::memory_order_relaxed);
		return false;
	}
	decltype(m_block->audioRing)::Header h;
	h.kind = kMidiAtFrame;
	h.length = (std::uint32_t) n;
	h.frame = frame;
	const bool ok = m_block->audioRing.pushRecord(h, bytes);
	if (!ok) m_localDroppedScheduled.fetch_add(n, std::memory_order_relaxed);
	return ok;
}

void RemoteEngine::pushPanelPulse(int row, int bit, int lenMs)
{
	if (!m_block || helperGone()) { m_localDroppedPanel.fetch_add(1); return; }
	if (m_uiProducer.test_and_set(std::memory_order_acquire)) { m_localDroppedPanel.fetch_add(1); return; }
	decltype(m_block->uiRing)::Header h;
	h.kind = kPanel;
	h.a = row; h.b = bit; h.c = lenMs;
	if (!m_block->uiRing.pushRecord(h, nullptr)) m_localDroppedPanel.fetch_add(1);
	m_uiProducer.clear(std::memory_order_release);
}

bool RemoteEngine::pushAdin(int source, int value)
{
	if (source < 0 || source > 15 || value < 0 || value > 255) return false;
	if (!readyForPlayback()) { m_localDroppedUiAdin.fetch_add(1); return false; }
	if (m_uiProducer.test_and_set(std::memory_order_acquire)) { m_localDroppedUiAdin.fetch_add(1); return false; }
	decltype(m_block->uiRing)::Header h;
	h.kind = kAdin;
	h.a = source; h.b = value;
	const bool ok = m_block->uiRing.pushRecord(h, nullptr);
	m_uiProducer.clear(std::memory_order_release);
	if (!ok) m_localDroppedUiAdin.fetch_add(1);
	return ok;
}

bool RemoteEngine::pushAdinAtFrame(int source, int value, std::uint64_t frame)
{
	if (source < 0 || source > 15 || value < 0 || value > 255) return false;
	if (!readyForPlayback()) { m_localDroppedAudioAdin.fetch_add(1); return false; }
	decltype(m_block->audioRing)::Header h;
	h.kind = kAdinAtFrame;
	h.a = source; h.b = value; h.frame = frame;
	const bool ok = m_block->audioRing.pushRecord(h, nullptr);
	if (!ok) m_localDroppedAudioAdin.fetch_add(1);
	return ok;
}

std::uint64_t RemoteEngine::droppedImmediateMidiBytes() const
{
	return m_localDroppedImmediate.load() + (m_block ? m_block->droppedImmediate.load() : 0);
}
std::uint64_t RemoteEngine::droppedScheduledMidiBytes() const
{
	return m_localDroppedScheduled.load() + (m_block ? m_block->droppedScheduled.load() : 0);
}
std::uint64_t RemoteEngine::droppedUiAdinEvents() const
{
	return m_localDroppedUiAdin.load() + (m_block ? m_block->droppedUiAdin.load() : 0);
}
std::uint64_t RemoteEngine::droppedAudioAdinEvents() const
{
	return m_localDroppedAudioAdin.load() + (m_block ? m_block->droppedAudioAdin.load() : 0);
}
std::uint64_t RemoteEngine::droppedScheduledPanelEvents() const
{
	return m_localDroppedPanel.load() + (m_block ? m_block->droppedScheduledPanel.load() : 0);
}
std::uint64_t RemoteEngine::droppedScheduledAdinEvents() const
{
	return m_block ? m_block->droppedScheduledAdin.load() : 0;
}

std::size_t RemoteEngine::popMidiTx(std::uint8_t *out, std::size_t cap)
{
	return m_block ? m_block->txRing.popBytes(out, cap) : 0;
}

std::uint32_t RemoteEngine::ledSnapshot(std::uint8_t out[12]) const
{
	if (!m_block) { std::memset(out, 0, 12); return 0; }
	for (int i = 0; i < 12; ++i) out[i] = m_block->led[i].load(std::memory_order_relaxed);
	return m_block->ledVersion.load(std::memory_order_acquire);
}

std::uint32_t RemoteEngine::ledVisualSnapshot(std::uint8_t out[12]) const
{
	if (!m_block) { std::memset(out, 0, 12); return 0; }
	// Latched rising edges stay visible for one read, then fall back to the live state.
	for (int i = 0; i < 12; ++i)
		out[i] = m_block->ledLatch[i].exchange(m_block->led[i].load(std::memory_order_relaxed),
			std::memory_order_acq_rel);
	return m_block->ledVersion.load(std::memory_order_acquire);
}

std::uint32_t RemoteEngine::lcdRawSnapshot(std::uint8_t row1[40], std::uint8_t row2[40], std::uint8_t cgram[64]) const
{
	if (!m_block) return 0;
	auto &lcd = m_block->lcd;
	for (int attempt = 0; attempt < 64; ++attempt)
	{
		const auto s = lcd.guard.beginRead();
		std::memcpy(row1, lcd.row1, 40);
		std::memcpy(row2, lcd.row2, 40);
		std::memcpy(cgram, lcd.cgram, 64);
		const auto version = lcd.rawVersion;
		if (lcd.guard.readOk(s)) return version;
	}
	return 0;
}

bool RemoteEngine::latestLcd(char *line1, char *line2, std::size_t cap) const
{
	if (cap > 0) { line1[0] = 0; line2[0] = 0; }
	if (!m_block || cap == 0) return false;
	auto &lcd = m_block->lcd;
	for (int attempt = 0; attempt < 64; ++attempt)
	{
		const auto s = lcd.guard.beginRead();
		const bool has = lcd.hasText != 0;
		const std::size_t n = std::min<std::size_t>(cap - 1, sizeof(lcd.text1) - 1);
		std::memcpy(line1, lcd.text1, n); line1[n] = 0;
		std::memcpy(line2, lcd.text2, n); line2[n] = 0;
		if (lcd.guard.readOk(s))
		{
			if (!has) { line1[0] = 0; line2[0] = 0; }
			return has;
		}
	}
	line1[0] = 0; line2[0] = 0;
	return false;
}

namespace {
template <typename Blob>
std::size_t readBlob(const Blob &blob, std::uint8_t *out, std::size_t cap, std::uint32_t *version, int *pattern)
{
	for (int attempt = 0; attempt < 64; ++attempt)
	{
		const auto s = blob.guard.beginRead();
		const std::size_t n = std::min<std::size_t>(blob.length, cap);
		if (out && n) std::memcpy(out, blob.data, n);
		const auto v = blob.dataVersion;
		const int p = blob.pattern;
		if (blob.guard.readOk(s))
		{
			if (version) *version = v;
			if (pattern) *pattern = p;
			return out ? n : static_cast<std::size_t>(blob.length);
		}
	}
	return 0;
}
} // namespace

std::size_t RemoteEngine::latestProgramData(std::uint8_t *out, std::size_t cap, std::uint32_t *version) const
{
	if (!m_block) { if (version) *version = 0; return 0; }
	return readBlob(m_block->program, out, cap, version, nullptr);
}

std::size_t RemoteEngine::latestArpeggioPatternData(std::uint8_t *out, std::size_t cap,
	std::uint32_t *version, int *pattern) const
{
	if (!m_block) { if (version) *version = 0; if (pattern) *pattern = -1; return 0; }
	return readBlob(m_block->arpeggio, out, cap, version, pattern);
}

std::size_t RemoteEngine::available() const
{
	if (!m_block) return 0;
	const auto end = m_block->audioEnd.load(std::memory_order_acquire);
	const auto first = m_block->readFirst.load(std::memory_order_acquire);
	return end > first ? static_cast<std::size_t>(end - first) : 0;
}

std::uint64_t RemoteEngine::producedFrames() const
{
	return m_block ? m_block->producedFrames.load(std::memory_order_acquire) : 0;
}
