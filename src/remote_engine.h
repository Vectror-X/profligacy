// SPDX-License-Identifier: AGPL-3.0-only
//
// remote_engine.h - plug-in side of one ProfligacyEngine helper process.
// Same calls as ProphecyEngine (the subset the plug-in uses); the machine itself
// runs in the helper. Realtime calls only touch shared memory and never wait.
//
#pragma once

#include "prophecy_engine.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace enginelink { struct SharedBlock; class SharedRegion; class ChildProcess; }

class RemoteEngine
{
public:
	RemoteEngine();
	~RemoteEngine();
	RemoteEngine(const RemoteEngine &) = delete;
	RemoteEngine &operator=(const RemoteEngine &) = delete;

	// Path of the helper next to this plug-in binary (PROFLIGACY_ENGINE_EXE overrides).
	static std::string helperPath();

	bool start(const std::vector<std::string> &args, bool firmwareHandshake);
	void requestStop();
	void stop();

	bool running() const;
	bool finished() const;
	ProphecyEngine::InstanceStatus instanceStatus() const;

	bool initializePlayback(bool firmwareHandshake);
	bool readyForPlayback() const;
	std::uint64_t playbackOrigin() const;
	void setHostBlockFrames(std::uint32_t nativeFrames);
	void requestThroughFrame(std::uint64_t exclusiveEnd);
	std::uint64_t requestedFrames() const;
	std::size_t readAtFrame(std::uint64_t first, float *left, float *right, std::size_t frames, bool offline);

	bool pushMidi(const std::uint8_t *bytes, std::size_t n);
	bool pushMidiAtFrame(const std::uint8_t *bytes, std::size_t n, std::uint64_t frame);
	void pushPanelPulse(int row, int bit, int lenMs);
	bool pushAdin(int source, int value);
	bool pushAdinAtFrame(int source, int value, std::uint64_t frame);

	std::uint64_t droppedImmediateMidiBytes() const;
	std::uint64_t droppedScheduledMidiBytes() const;
	std::uint64_t droppedUiAdinEvents() const;
	std::uint64_t droppedAudioAdinEvents() const;
	std::uint64_t droppedScheduledPanelEvents() const;
	std::uint64_t droppedScheduledAdinEvents() const;

	std::size_t popMidiTx(std::uint8_t *out, std::size_t cap);

	std::uint32_t ledSnapshot(std::uint8_t out[12]) const;
	std::uint32_t ledVisualSnapshot(std::uint8_t out[12]) const;
	std::uint32_t lcdRawSnapshot(std::uint8_t row1[40], std::uint8_t row2[40], std::uint8_t cgram[64]) const;
	bool latestLcd(char *line1, char *line2, std::size_t cap) const;
	std::size_t latestProgramData(std::uint8_t *out, std::size_t cap, std::uint32_t *version) const;
	std::size_t latestArpeggioPatternData(std::uint8_t *out, std::size_t cap, std::uint32_t *version, int *pattern) const;

	std::size_t available() const;
	std::uint64_t producedFrames() const;

private:
	bool helperGone() const;

	std::unique_ptr<enginelink::SharedRegion> m_region;
	std::unique_ptr<enginelink::ChildProcess> m_child;
	enginelink::SharedBlock *m_block = nullptr;
	std::atomic<ProphecyEngine::InstanceStatus> m_status{ProphecyEngine::InstanceStatus::NotStarted};
	std::atomic<bool> m_stopRequested{false};
	std::atomic_flag m_uiProducer = ATOMIC_FLAG_INIT;
	std::atomic<std::uint64_t> m_localDroppedImmediate{0}, m_localDroppedScheduled{0},
		m_localDroppedUiAdin{0}, m_localDroppedAudioAdin{0}, m_localDroppedPanel{0};
};
