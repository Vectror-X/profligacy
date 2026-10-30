// SPDX-License-Identifier: AGPL-3.0-only
//
// engine_facade.h - what the plug-in holds instead of a bare ProphecyEngine
// (Vectror-X fork, multi-instance plan).
//
// The first instance in a host process reserves the in-process engine and behaves
// exactly as before. Every further instance runs its engine in a ProfligacyEngine
// helper process (RemoteEngine). Both sides expose the same calls.
//
#pragma once

#include "prophecy_engine.h"
#include "remote_engine.h"

#include <atomic>
#include <memory>

class EngineFacade
{
public:
	using InstanceStatus = ProphecyEngine::InstanceStatus;
	using MidiTxByteEvent = ProphecyEngine::MidiTxByteEvent;

	EngineFacade() = default;
	~EngineFacade();
	EngineFacade(const EngineFacade &) = delete;
	EngineFacade &operator=(const EngineFacade &) = delete;

	// Decide, before start(), whether this instance gets the in-process engine.
	// Returns true for the in-process engine, false for a helper process. The
	// answer is fixed after the first call.
	bool reserveInProcess();
	bool isRemote() const { return m_useRemote; }

	bool start(const std::vector<std::string> &args);
	bool enableMidiTxByteCapture(bool enabled = true) { return m_local.enableMidiTxByteCapture(enabled); }
	bool enableHostTimeline() { m_hostTimeline = true; return m_local.enableHostTimeline(); }
	void requestStop();
	void stop();

	bool running() const        { return m_useRemote ? m_remote->running() : m_local.running(); }
	bool finished() const       { return m_useRemote ? m_remote->finished() : m_local.finished(); }
	InstanceStatus instanceStatus() const
	{ return m_useRemote ? m_remote->instanceStatus() : m_local.instanceStatus(); }

	bool initializePlayback(bool firmwareHandshake = true)
	{ return m_useRemote ? m_remote->initializePlayback(firmwareHandshake) : m_local.initializePlayback(firmwareHandshake); }
	bool readyForPlayback() const { return m_useRemote ? m_remote->readyForPlayback() : m_local.readyForPlayback(); }
	std::uint64_t playbackOrigin() const { return m_useRemote ? m_remote->playbackOrigin() : m_local.playbackOrigin(); }
	void setHostBlockFrames(std::uint32_t nativeFrames)
	{ if (m_useRemote) m_remote->setHostBlockFrames(nativeFrames); else m_local.setHostBlockFrames(nativeFrames); }
	void requestThroughFrame(std::uint64_t exclusiveEnd)
	{ if (m_useRemote) m_remote->requestThroughFrame(exclusiveEnd); else m_local.requestThroughFrame(exclusiveEnd); }
	std::uint64_t requestedFrames() const { return m_useRemote ? m_remote->requestedFrames() : m_local.requestedFrames(); }
	std::size_t readAtFrame(std::uint64_t first, float *left, float *right, std::size_t frames, bool offline)
	{
		return m_useRemote ? m_remote->readAtFrame(first, left, right, frames, offline)
			: m_local.readAtFrame(first, left, right, frames, offline);
	}

	bool pushMidi(const std::uint8_t *bytes, std::size_t n)
	{ return m_useRemote ? m_remote->pushMidi(bytes, n) : m_local.pushMidi(bytes, n); }
	bool pushMidiAtFrame(const std::uint8_t *bytes, std::size_t n, std::uint64_t frame)
	{ return m_useRemote ? m_remote->pushMidiAtFrame(bytes, n, frame) : m_local.pushMidiAtFrame(bytes, n, frame); }
	void pushPanelPulse(int row, int bit, int lenMs = 75)
	{ if (m_useRemote) m_remote->pushPanelPulse(row, bit, lenMs); else m_local.pushPanelPulse(row, bit, lenMs); }
	bool pushAdin(int source, int value)
	{ return m_useRemote ? m_remote->pushAdin(source, value) : m_local.pushAdin(source, value); }
	bool pushAdinAtFrame(int source, int value, std::uint64_t frame)
	{ return m_useRemote ? m_remote->pushAdinAtFrame(source, value, frame) : m_local.pushAdinAtFrame(source, value, frame); }

	std::uint64_t droppedImmediateMidiBytes() const
	{ return m_useRemote ? m_remote->droppedImmediateMidiBytes() : m_local.droppedImmediateMidiBytes(); }
	std::uint64_t droppedScheduledMidiBytes() const
	{ return m_useRemote ? m_remote->droppedScheduledMidiBytes() : m_local.droppedScheduledMidiBytes(); }
	std::uint64_t droppedUiAdinEvents() const
	{ return m_useRemote ? m_remote->droppedUiAdinEvents() : m_local.droppedUiAdinEvents(); }
	std::uint64_t droppedAudioAdinEvents() const
	{ return m_useRemote ? m_remote->droppedAudioAdinEvents() : m_local.droppedAudioAdinEvents(); }
	std::uint64_t droppedScheduledPanelEvents() const
	{ return m_useRemote ? m_remote->droppedScheduledPanelEvents() : m_local.droppedScheduledPanelEvents(); }
	std::uint64_t droppedScheduledAdinEvents() const
	{ return m_useRemote ? m_remote->droppedScheduledAdinEvents() : m_local.droppedScheduledAdinEvents(); }

	std::size_t popMidiTx(std::uint8_t *out, std::size_t cap)
	{ return m_useRemote ? m_remote->popMidiTx(out, cap) : m_local.popMidiTx(out, cap); }
	// Raw MIDI OUT timing capture is a diagnostic of the in-process engine only.
	std::size_t popMidiTxByteEvents(MidiTxByteEvent *out, std::size_t cap)
	{ return m_useRemote ? 0 : m_local.popMidiTxByteEvents(out, cap); }
	std::uint64_t droppedMidiTxByteEvents() const { return m_useRemote ? 0 : m_local.droppedMidiTxByteEvents(); }

	std::uint32_t ledSnapshot(std::uint8_t out[12]) const
	{ return m_useRemote ? m_remote->ledSnapshot(out) : m_local.ledSnapshot(out); }
	std::uint32_t ledVisualSnapshot(std::uint8_t out[12]) const
	{ return m_useRemote ? m_remote->ledVisualSnapshot(out) : m_local.ledVisualSnapshot(out); }
	std::uint32_t lcdRawSnapshot(std::uint8_t row1[40], std::uint8_t row2[40], std::uint8_t cgram[64]) const
	{ return m_useRemote ? m_remote->lcdRawSnapshot(row1, row2, cgram) : m_local.lcdRawSnapshot(row1, row2, cgram); }
	bool latestLcd(char *line1, char *line2, std::size_t cap) const
	{ return m_useRemote ? m_remote->latestLcd(line1, line2, cap) : m_local.latestLcd(line1, line2, cap); }
	std::size_t latestProgramData(std::uint8_t *out, std::size_t cap, std::uint32_t *version) const
	{ return m_useRemote ? m_remote->latestProgramData(out, cap, version) : m_local.latestProgramData(out, cap, version); }
	std::size_t latestArpeggioPatternData(std::uint8_t *out, std::size_t cap, std::uint32_t *version, int *pattern) const
	{
		return m_useRemote ? m_remote->latestArpeggioPatternData(out, cap, version, pattern)
			: m_local.latestArpeggioPatternData(out, cap, version, pattern);
	}

	std::size_t available() const { return m_useRemote ? m_remote->available() : m_local.available(); }
	std::uint64_t producedFrames() const { return m_useRemote ? m_remote->producedFrames() : m_local.producedFrames(); }

private:
	void releaseReservation();

	ProphecyEngine m_local;
	std::unique_ptr<RemoteEngine> m_remote;
	bool m_decided = false;
	bool m_reservedInProcess = false;
	bool m_ownsInProcessSlot = false;
	bool m_useRemote = false;
	bool m_hostTimeline = false;
	static std::atomic<bool> s_inProcessTaken;
};
