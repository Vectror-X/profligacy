#pragma once
// Patch/bank librarian helpers (Vectror-X fork). Pure file/byte utilities: reading
// .syx and Standard MIDI Files, recognising Prophecy SysEx, and writing files back.
// Nothing here touches the engine; PluginProcessor decides what to send and when.

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace librarian
{
using Bytes = std::vector<std::uint8_t>;

// Korg Prophecy exclusive header: F0 42 3n 41 <function> ...
inline bool isProphecySysex(const Bytes &m)
{
	return m.size() >= 6 && m.front() == 0xF0 && m.back() == 0xF7 && m[1] == 0x42
		&& (m[2] & 0xF0) == 0x30 && m[3] == 0x41;
}

inline int functionCode(const Bytes &m) { return isProphecySysex(m) ? m[4] : -1; }

// Function codes seen in real files / the firmware: 0x40 current program (edit buffer),
// 0x50 All Data (globals + 128 programs + arpeggio patterns).
constexpr int kCurrentProgramDump = 0x40;
constexpr int kAllDataDump        = 0x50;

inline juce::String describeFunction(int fn)
{
	if (fn == kCurrentProgramDump) return "Individual Patch";
	if (fn == kAllDataDump)        return "All Data";
	return "type 0x" + juce::String::toHexString(fn).paddedLeft('0', 2).toUpperCase();
}

// Korg 7-in-8 unpack: a high-bit byte followed by up to seven low-7-bit bytes.
inline Bytes unpack7in8(const std::uint8_t *p, std::size_t len)
{
	Bytes out;
	out.reserve(len);
	for (std::size_t pos = 0; pos < len;)
	{
		const std::uint8_t high = p[pos++];
		for (int i = 0; i < 7 && pos < len; ++i)
			out.push_back(std::uint8_t((p[pos++] & 0x7f) | (((high >> i) & 1) << 7)));
	}
	return out;
}

// Program name of a current-program dump: F0 42 3n 41 40 00 <packed> F7, name = first 16 bytes.
inline juce::String programName(const Bytes &m)
{
	if (functionCode(m) != kCurrentProgramDump || m.size() < 8) return {};
	const Bytes raw = unpack7in8(m.data() + 6, m.size() - 7);
	juce::String name;
	for (std::size_t i = 0; i < 16 && i < raw.size(); ++i)
		name += (raw[i] >= 32 && raw[i] < 127) ? (juce::juce_wchar) raw[i] : (juce::juce_wchar) ' ';
	return name.trim();
}

namespace detail
{
inline bool readVlq(const std::uint8_t *d, std::size_t size, std::size_t &pos, std::uint32_t &value)
{
	value = 0;
	for (int i = 0; i < 4; ++i)
	{
		if (pos >= size) return false;
		const std::uint8_t b = d[pos++];
		value = (value << 7) | (b & 0x7f);
		if ((b & 0x80) == 0) return true;
	}
	return false;
}

inline void writeVlq(Bytes &out, std::uint32_t value)
{
	std::uint8_t tmp[5];
	int n = 0;
	tmp[n++] = std::uint8_t(value & 0x7f);
	while ((value >>= 7) != 0) tmp[n++] = std::uint8_t(0x80 | (value & 0x7f));
	while (n > 0) out.push_back(tmp[--n]);
}

inline std::uint32_t be32(const std::uint8_t *p)
{
	return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

// Raw .syx (or any byte stream): collect every complete F0..F7 message.
inline std::vector<Bytes> scanRaw(const std::uint8_t *d, std::size_t size)
{
	std::vector<Bytes> out;
	for (std::size_t i = 0; i < size; ++i)
	{
		if (d[i] != 0xF0) continue;
		std::size_t j = i + 1;
		while (j < size && d[j] < 0x80) ++j; // SysEx body is 7-bit
		if (j < size && d[j] == 0xF7) out.emplace_back(d + i, d + j + 1);
		i = j;
	}
	return out;
}

// Standard MIDI File: collect SysEx events (F0 packets, joined with F7 continuations).
inline std::vector<Bytes> scanSmf(const std::uint8_t *d, std::size_t size)
{
	std::vector<Bytes> out;
	std::size_t chunk = 0;
	while (chunk + 8 <= size)
	{
		const std::uint32_t len = be32(d + chunk + 4);
		const std::size_t body = chunk + 8;
		const std::size_t end = std::min<std::size_t>(size, body + len);
		if (std::memcmp(d + chunk, "MTrk", 4) == 0)
		{
			std::size_t pos = body;
			std::uint8_t status = 0;
			Bytes pending;
			while (pos < end)
			{
				std::uint32_t delta = 0;
				if (!readVlq(d, end, pos, delta) || pos >= end) break;
				std::uint8_t b = d[pos];
				if (b == 0xF0 || b == 0xF7)
				{
					++pos;
					std::uint32_t n = 0;
					if (!readVlq(d, end, pos, n) || pos + n > end) break;
					// F0 starts a packet; F7 continues one (or escapes a complete F0..F7 message).
					if (b == 0xF0) pending.assign(1, 0xF0);
					pending.insert(pending.end(), d + pos, d + pos + n);
					pos += n;
					if (!pending.empty() && pending.front() == 0xF0 && pending.back() == 0xF7)
					{
						for (auto &m : scanRaw(pending.data(), pending.size())) out.push_back(std::move(m));
						pending.clear();
					}
					status = 0;
					continue;
				}
				if (b == 0xFF)
				{
					pos += 2;
					std::uint32_t n = 0;
					if (pos > end || !readVlq(d, end, pos, n)) break;
					pos += n;
					status = 0;
					continue;
				}
				if (b & 0x80) { status = b; ++pos; }
				if (status == 0) break; // malformed running status
				const int hi = status & 0xF0;
				pos += (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
			}
		}
		chunk = body + len;
	}
	return out;
}
} // namespace detail

// Every complete SysEx message in a .syx or .mid file (MIDI files are detected by header).
inline std::vector<Bytes> readSysexMessages(const juce::MemoryBlock &file)
{
	const auto *d = static_cast<const std::uint8_t *>(file.getData());
	const std::size_t size = file.getSize();
	if (size >= 14 && std::memcmp(d, "MThd", 4) == 0)
		return detail::scanSmf(d, size);
	return detail::scanRaw(d, size);
}

// Write SysEx messages as raw .syx, or as a format-0 Standard MIDI File when the
// destination ends in .mid/.midi (one SysEx event per message, all at time zero).
inline bool writeSysexFile(const juce::File &dest, const std::vector<Bytes> &messages)
{
	Bytes out;
	const auto ext = dest.getFileExtension().toLowerCase();
	if (ext == ".mid" || ext == ".midi")
	{
		Bytes track;
		for (const auto &m : messages)
		{
			if (m.size() < 2) continue;
			track.push_back(0x00);                    // delta time
			track.push_back(0xF0);
			detail::writeVlq(track, (std::uint32_t) (m.size() - 1));
			track.insert(track.end(), m.begin() + 1, m.end());
		}
		track.insert(track.end(), {0x00, 0xFF, 0x2F, 0x00}); // end of track
		out.insert(out.end(), {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96});
		out.insert(out.end(), {'M', 'T', 'r', 'k'});
		const auto n = (std::uint32_t) track.size();
		out.insert(out.end(), {std::uint8_t(n >> 24), std::uint8_t(n >> 16), std::uint8_t(n >> 8), std::uint8_t(n)});
		out.insert(out.end(), track.begin(), track.end());
	}
	else
	{
		for (const auto &m : messages) out.insert(out.end(), m.begin(), m.end());
	}
	return dest.replaceWithData(out.data(), out.size());
}
} // namespace librarian
