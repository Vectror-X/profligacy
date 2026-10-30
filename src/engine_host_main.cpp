// SPDX-License-Identifier: AGPL-3.0-only
//
// engine_host_main.cpp - ProfligacyEngine.exe (Vectror-X fork).
//
// First piece of the multi-instance plan: one MAME machine per helper process,
// because MAME allows only one machine per process. In this build the helper is
// started by the plug-in for its second, third... instance (--serve), and talks
// to it only through shared memory (engine_link.h).
//
// Run on its own it performs a self-test: find the ROM set exactly like the
// plug-in does, boot the Prophecy against a TEMPORARY copy of the battery RAM (the
// real sysram is never written), pull audio at a real-time 48 kHz pace like a DAW
// callback would, and print a short report.
//
//   ProfligacyEngine.exe              self-test, 25 seconds, waits for Enter
//   ProfligacyEngine.exe --seconds 40 longer self-test
//   ProfligacyEngine.exe --no-pause   do not wait for Enter at the end
//   ProfligacyEngine.exe --serve <shared memory name> --host-pid <pid>   (plug-in only)
//
#include "engine_link.h"
#include "engine_link_platform.h"
#include "prophecy_engine.h"
#include "rom_locator.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

void setEnv(const char *name, const char *value, bool overwrite)
{
	if (!overwrite && std::getenv(name) != nullptr) return;
#if defined(_WIN32)
	_putenv_s(name, value);
#else
	setenv(name, value, overwrite ? 1 : 0);
#endif
}

juce::String g_log;

void say(const juce::String &line)
{
	std::printf("%s\n", line.toRawUTF8());
	std::fflush(stdout);
	g_log << line << "\n";
}

int finish(int code, bool pause)
{
	const juce::File report = romloc::appSupportDir().getChildFile("engine-selftest.txt");
	report.getParentDirectory().createDirectory();
	(void) report.replaceWithText(g_log);
	std::printf("\nRelatorio salvo em: %s\n", report.getFullPathName().toRawUTF8());
	if (pause)
	{
		std::printf("Pressione Enter para fechar...");
		std::fflush(stdout);
		(void) std::getchar();
	}
	return code;
}

} // namespace

// ---------------------------------------------------------------------------
// --serve: one engine for one plug-in instance.
// ---------------------------------------------------------------------------
namespace serve {

using namespace enginelink;

// Copies a record payload; keeps it while the engine queue is full so order holds.
template <typename Ring>
struct Pending
{
	typename Ring::Header header{};
	std::vector<std::uint8_t> payload = std::vector<std::uint8_t>(Ring::kMaxPayload);
	bool has = false;
	bool load(Ring &ring)
	{
		if (!has) has = ring.peekRecord(header, payload.data());
		return has;
	}
	void consumed(Ring &ring) { ring.dropRecord(header); has = false; }
};

void publishSnapshots(ProphecyEngine &engine, SharedBlock &b, std::uint32_t &lastLcd,
	std::uint32_t &lastProgram, std::uint32_t &lastArp, std::vector<std::uint8_t> &scratch)
{
	std::uint8_t leds[12], visual[12];
	const auto ledVersion = engine.ledSnapshot(leds);
	(void) engine.ledVisualSnapshot(visual);
	for (int i = 0; i < 12; ++i)
	{
		b.led[i].store(leds[i], std::memory_order_relaxed);
		if (visual[i]) b.ledLatch[i].fetch_or(visual[i], std::memory_order_acq_rel);
	}
	b.ledVersion.store(ledVersion, std::memory_order_release);

	std::uint8_t r1[40], r2[40], cg[64];
	const auto lcdVersion = engine.lcdRawSnapshot(r1, r2, cg);
	if (lcdVersion != lastLcd)
	{
		lastLcd = lcdVersion;
		char t1[48] = {}, t2[48] = {};
		const bool has = engine.latestLcd(t1, t2, 41);
		b.lcd.guard.beginWrite();
		std::memcpy(b.lcd.row1, r1, 40);
		std::memcpy(b.lcd.row2, r2, 40);
		std::memcpy(b.lcd.cgram, cg, 64);
		b.lcd.rawVersion = lcdVersion;
		b.lcd.hasText = has ? 1u : 0u;
		std::memcpy(b.lcd.text1, t1, sizeof(t1));
		std::memcpy(b.lcd.text2, t2, sizeof(t2));
		b.lcd.guard.endWrite();
	}

	std::uint32_t version = 0;
	std::size_t n = engine.latestProgramData(scratch.data(), scratch.size(), &version);
	if (version != lastProgram)
	{
		lastProgram = version;
		n = std::min(n, sizeof(b.program.data));
		b.program.guard.beginWrite();
		std::memcpy(b.program.data, scratch.data(), n);
		b.program.length = (std::uint32_t) n;
		b.program.dataVersion = version;
		b.program.guard.endWrite();
	}
	int pattern = -1;
	n = engine.latestArpeggioPatternData(scratch.data(), scratch.size(), &version, &pattern);
	if (version != lastArp)
	{
		lastArp = version;
		n = std::min(n, sizeof(b.arpeggio.data));
		b.arpeggio.guard.beginWrite();
		std::memcpy(b.arpeggio.data, scratch.data(), n);
		b.arpeggio.length = (std::uint32_t) n;
		b.arpeggio.dataVersion = version;
		b.arpeggio.pattern = pattern;
		b.arpeggio.guard.endWrite();
	}

	b.producedFrames.store(engine.producedFrames(), std::memory_order_release);
	b.requestedFrames.store(engine.requestedFrames(), std::memory_order_release);
	b.engineRunning.store(engine.running() ? 1u : 0u, std::memory_order_release);
	b.engineFinished.store(engine.finished() ? 1u : 0u, std::memory_order_release);
	b.droppedImmediate.store(engine.droppedImmediateMidiBytes(), std::memory_order_relaxed);
	b.droppedScheduled.store(engine.droppedScheduledMidiBytes(), std::memory_order_relaxed);
	b.droppedUiAdin.store(engine.droppedUiAdinEvents(), std::memory_order_relaxed);
	b.droppedAudioAdin.store(engine.droppedAudioAdinEvents(), std::memory_order_relaxed);
	b.droppedScheduledAdin.store(engine.droppedScheduledAdinEvents(), std::memory_order_relaxed);
	b.droppedScheduledPanel.store(engine.droppedScheduledPanelEvents(), std::memory_order_relaxed);
}

int run(const std::string &regionName, std::uint64_t hostPid)
{
	SharedRegion region;
	if (!region.open(regionName, sizeof(SharedBlock))) return 10;
	auto &b = *static_cast<SharedBlock *>(region.data());
	if (b.magic != kMagic || b.version != kVersion || b.blockSize != sizeof(SharedBlock)) return 11;

	helperProcessSetup();
	setEnvironment("SDL_VIDEODRIVER", "dummy", true);
	setEnvironment("SDL_AUDIODRIVER", "dummy", true);
	setEnvironment("KPROP_LIE_BATTERY_OK", "1", false);

	std::vector<std::string> args;
	for (std::uint32_t i = 0, at = 0; i < b.argCount && at < kArgBytes; ++i)
	{
		const char *a = b.args + at;
		const std::size_t len = strnlen(a, kArgBytes - at);
		args.emplace_back(a, len);
		at += (std::uint32_t) len + 1;
	}

	ProphecyEngine engine;
	(void) engine.enableHostTimeline();
	if (!engine.start(args))
	{
		b.state.store(kFailed, std::memory_order_release);
		return 12;
	}
	b.state.store(kStarted, std::memory_order_release);

	std::atomic<int> init{0}; // 0 running, 1 ready, 2 failed
	const bool firmware = b.firmwareHandshake != 0;
	std::thread initThread([&] { init.store(engine.initializePlayback(firmware) ? 1 : 2); });

	using Ui = decltype(b.uiRing);
	using Rt = decltype(b.audioRing);
	Pending<Ui> ui;
	Pending<Rt> rt;
	std::deque<std::uint8_t> txPending;
	std::vector<std::uint8_t> scratch(8192);
	std::uint8_t txBuf[4096];
	constexpr std::size_t kChunk = 1024;
	std::vector<float> left(kChunk), right(kChunk);
	std::uint32_t lastLcd = 0xffffffffu, lastProgram = 0xffffffffu, lastArp = 0xffffffffu;
	std::uint32_t lastBlockFrames = 0;
	std::uint64_t audioPos = 0, forwarded = 0;
	bool ready = false;
	auto lastSnapshot = std::chrono::steady_clock::now() - std::chrono::seconds(1);
	auto lastHostCheck = lastSnapshot;
	int exitCode = 0;

	while (true)
	{
		const auto now = std::chrono::steady_clock::now();
		if (b.stopRequested.load(std::memory_order_acquire)) break;
		if (now - lastHostCheck > std::chrono::milliseconds(200))
		{
			lastHostCheck = now;
			if (hostPid != 0 && !processAlive(hostPid)) { exitCode = 20; break; } // host is gone
		}
		if (engine.finished()) { exitCode = 21; break; }

		bool busy = false;
		if (!ready)
		{
			const int s = init.load();
			if (s == 1)
			{
				ready = true;
				audioPos = engine.playbackOrigin();
				forwarded = engine.requestedFrames();
				b.playbackOrigin.store(audioPos, std::memory_order_release);
				b.audioEnd.store(audioPos, std::memory_order_release);
				b.readFirst.store(audioPos, std::memory_order_release);
				publishSnapshots(engine, b, lastLcd, lastProgram, lastArp, scratch);
				b.state.store(kReady, std::memory_order_release);
			}
			else if (s == 2)
			{
				b.state.store(kFailed, std::memory_order_release);
				exitCode = 13;
				break;
			}
		}

		if (ready)
		{
			const auto blockFrames = b.hostBlockFrames.load(std::memory_order_acquire);
			if (blockFrames != 0 && blockFrames != lastBlockFrames)
			{
				lastBlockFrames = blockFrames;
				engine.setHostBlockFrames(blockFrames);
			}
			const auto wanted = b.requestThrough.load(std::memory_order_acquire);
			if (wanted > forwarded)
			{
				forwarded = wanted;
				engine.requestThroughFrame(wanted);
			}

			// Audio: copy what the engine has finished, never lapping the plug-in reader.
			const auto readFirst = b.readFirst.load(std::memory_order_acquire);
			const auto limit = readFirst + kAudioCapacity - kAudioGuard;
			if (audioPos < limit)
			{
				const std::size_t want = (std::size_t) std::min<std::uint64_t>(kChunk, limit - audioPos);
				const std::size_t got = engine.readAtFrame(audioPos, left.data(), right.data(), want, false);
				for (std::size_t i = 0; i < got; ++i)
				{
					const auto slot = (std::size_t) ((audioPos + i) % kAudioCapacity);
					b.audioL[slot] = left[i];
					b.audioR[slot] = right[i];
				}
				if (got > 0)
				{
					audioPos += got;
					b.audioEnd.store(audioPos, std::memory_order_release);
					busy = true;
				}
			}

			// Plug-in message thread -> engine (immediate MIDI, knobs, panel buttons).
			for (int i = 0; i < 64 && ui.load(b.uiRing); ++i)
			{
				const auto &h = ui.header;
				bool accepted = true;
				if (h.kind == kMidi) accepted = engine.pushMidi(ui.payload.data(), h.length);
				else if (h.kind == kAdin) accepted = engine.pushAdin(h.a, h.b);
				else if (h.kind == kPanel) engine.pushPanelPulse(h.a, h.b, h.c);
				if (!accepted && h.kind == kAdin) accepted = true; // a full knob queue drops, like in-process
				if (!accepted) break; // engine queue full: retry the same record later
				ui.consumed(b.uiRing);
				busy = true;
			}
			// Plug-in audio thread -> engine (sample-timed MIDI and knobs).
			for (int i = 0; i < 256 && rt.load(b.audioRing); ++i)
			{
				const auto &h = rt.header;
				bool accepted = true;
				if (h.kind == kMidiAtFrame) accepted = engine.pushMidiAtFrame(rt.payload.data(), h.length, h.frame);
				else if (h.kind == kAdinAtFrame) accepted = engine.pushAdinAtFrame(h.a, h.b, h.frame);
				if (!accepted) break;
				rt.consumed(b.audioRing);
				busy = true;
			}
		}

		// Firmware MIDI OUT -> plug-in (dumps, parameter echoes). Nothing is dropped:
		// bytes wait here while the plug-in has not drained its side.
		for (std::size_t n; (n = engine.popMidiTx(txBuf, sizeof(txBuf))) > 0; busy = true)
			txPending.insert(txPending.end(), txBuf, txBuf + n);
		while (!txPending.empty())
		{
			const std::size_t n = std::min<std::size_t>(txPending.size(), sizeof(txBuf));
			std::copy(txPending.begin(), txPending.begin() + (std::ptrdiff_t) n, txBuf);
			const std::size_t wrote = b.txRing.pushBytes(txBuf, n);
			txPending.erase(txPending.begin(), txPending.begin() + (std::ptrdiff_t) wrote);
			if (wrote < n) break;
		}

		if (now - lastSnapshot > std::chrono::milliseconds(5))
		{
			lastSnapshot = now;
			publishSnapshots(engine, b, lastLcd, lastProgram, lastArp, scratch);
			b.heartbeat.fetch_add(1, std::memory_order_relaxed);
		}
		if (!busy) sleepMilliseconds(1);
	}

	engine.requestStop();
	if (initThread.joinable()) initThread.join();
	engine.stop(); // MAME writes this instance's battery RAM here
	b.engineFinished.store(1, std::memory_order_release);
	b.state.store(kExited, std::memory_order_release);
	return exitCode;
}

} // namespace serve

int main(int argc, char **argv)
{
	for (int i = 1; i + 1 < argc; ++i)
		if (std::strcmp(argv[i], "--serve") == 0)
		{
			std::uint64_t hostPid = 0;
			for (int j = 1; j + 1 < argc; ++j)
				if (std::strcmp(argv[j], "--host-pid") == 0) hostPid = std::strtoull(argv[j + 1], nullptr, 10);
			return serve::run(argv[i + 1], hostPid);
		}

	juce::ScopedJuceInitialiser_GUI juceInit; // PropertiesFile (saved ROM path) needs JUCE initialised
	double seconds = 25.0;
	bool pause = true;
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
			seconds = juce::jlimit(12.0, 600.0, std::atof(argv[++i]));
		else if (std::strcmp(argv[i], "--no-pause") == 0)
			pause = false;
	}

	say("ProfligacyEngine - teste do motor auxiliar (o mesmo que o plugin usa da 2a instancia em diante)");
	say("--------------------------------------------------------------------------");

	const juce::File romDir = romloc::locateRomDir();
	if (romDir == juce::File())
	{
		say("FALHA: nao achei as ROMs do Prophecy. Abra o plugin uma vez e escolha a pasta das ROMs.");
		return finish(2, pause);
	}
	say("ROMs: " + romDir.getFullPathName());

	// Temporary battery RAM: copy the plug-in's sysram so the test boots your real
	// banks, but MAME's exit-time write lands only in the temporary folder.
	const juce::File realNvram = romloc::nvramDirFor(romDir);
	const juce::File temp = juce::File::getSpecialLocation(juce::File::tempDirectory)
		.getChildFile("ProfligacyEngineSelftest");
	(void) temp.deleteRecursively();
	const juce::File tempSys = temp.getChildFile("nvram").getChildFile("korgprop").getChildFile("sysram");
	tempSys.getParentDirectory().createDirectory();
	const juce::File realSys = realNvram.getChildFile("korgprop").getChildFile("sysram");
	if (realSys.existsAsFile())
	{
		(void) realSys.copyFileTo(tempSys);
		say("Memoria (copia temporaria, a original nao e tocada): " + realSys.getFullPathName());
	}
	else
		say("Memoria: nenhum sysram encontrado, o firmware vai iniciar com a memoria vazia");

	setEnv("SDL_VIDEODRIVER", "dummy", true);
	setEnv("SDL_AUDIODRIVER", "dummy", true);
	setEnv("KPROP_LIE_BATTERY_OK", "1", false);

	const std::vector<std::string> args = {
		"prophecy", "korgprop",
		"-rompath", romDir.getFullPathName().toStdString(),
		"-nvram_directory", temp.getChildFile("nvram").getFullPathName().toStdString(),
		"-video", "none", "-sound", "none", "-nothrottle", "-skip_gameinfo",
		"-debugger", "none", "-midiprovider", "none", "-networkprovider", "none",
		"-keyboardprovider", "none", "-mouseprovider", "none",
		"-lightgunprovider", "none", "-joystickprovider", "none", "-output", "none",
		"-noplugins",
	};

	using clock = std::chrono::steady_clock;
	const auto t0 = clock::now();
	auto elapsed = [&] { return std::chrono::duration<double>(clock::now() - t0).count(); };

	ProphecyEngine engine;
	if (!engine.start(args))
	{
		say("FALHA: o motor do MAME nao iniciou neste processo.");
		(void) temp.deleteRecursively();
		return finish(3, pause);
	}
	say("Motor iniciado. Rodando " + juce::String(seconds, 0) + " segundos em tempo real...");

#if defined(_WIN32)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif

	constexpr std::size_t kBlock = 512; // ~10.7 ms at 48 kHz, a typical host buffer
	std::vector<float> left(kBlock), right(kBlock);
	const auto period = std::chrono::duration<double>(double(kBlock) / ProphecyEngine::kSampleRate);

	double firstAudioAt = -1.0, lcdAt = -1.0;
	std::uint64_t blocksAfterBoot = 0, shortBlocksAfterBoot = 0;
	char line1[64] = {}, line2[64] = {};
	juce::String lastLcd;

	// Wait (up to 30 s) for the first audio before pacing, like the console host.
	while (engine.available() < kBlock && !engine.finished() && elapsed() < 30.0)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	if (engine.available() >= kBlock) firstAudioAt = elapsed();

	auto next = clock::now();
	const auto end = clock::now() + std::chrono::duration_cast<clock::duration>(
		std::chrono::duration<double>(seconds));
	while (firstAudioAt >= 0.0 && clock::now() < end && !engine.finished())
	{
		next += std::chrono::duration_cast<clock::duration>(period);
		std::this_thread::sleep_until(next);
		const std::size_t got = engine.pull(left.data(), right.data(), kBlock);
		if (lcdAt >= 0.0)
		{
			++blocksAfterBoot;
			if (got < kBlock) ++shortBlocksAfterBoot;
		}
		if (engine.latestLcd(line1, line2, sizeof(line1)))
		{
			const juce::String lcd = juce::String(line1).trimEnd() + " | " + juce::String(line2).trimEnd();
			if (lcd != lastLcd)
			{
				lastLcd = lcd;
				if (lcdAt < 0.0) lcdAt = elapsed();
				say("  LCD [" + juce::String(elapsed(), 1) + " s]: " + lcd);
			}
		}
	}

	const std::uint64_t produced = engine.producedFrames();
	engine.stop();
	(void) temp.deleteRecursively();

	say("--------------------------------------------------------------------------");
	say("Primeiro audio: " + (firstAudioAt >= 0.0 ? juce::String(firstAudioAt, 1) + " s" : juce::String("nunca")));
	say("Firmware desenhou o LCD: " + (lcdAt >= 0.0 ? juce::String(lcdAt, 1) + " s" : juce::String("nunca")));
	say("Audio produzido: " + juce::String(double(produced) / ProphecyEngine::kSampleRate, 1) + " s");
	say("Blocos depois do boot: " + juce::String((juce::int64) blocksAfterBoot)
		+ ", blocos incompletos (engasgos): " + juce::String((juce::int64) shortBlocksAfterBoot));

	const bool ok = firstAudioAt >= 0.0 && lcdAt >= 0.0 && blocksAfterBoot > 0
		&& shortBlocksAfterBoot * 100 <= blocksAfterBoot; // at most 1 % short blocks
	say(ok ? "RESULTADO: OK - o motor auxiliar roda sozinho neste PC."
	       : "RESULTADO: FALHA - mande este relatorio no chat.");
	return finish(ok ? 0 : 1, pause);
}
