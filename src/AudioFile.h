#pragma once

/*  RIFF/WAVE, read and written.

    Two things need this and they are on opposite sides of the project: the
    offline tool reads a file, processes it and writes another, and the app's
    file player reads one to push through the live chain. There is no version
    of "the app grew a WAV reader" that ends well if it is a second, subtly
    different one -- a 24-bit scaling that disagrees by a bit or a chunk walk
    that gives up on a LIST chunk would show up as the app and the tool
    disagreeing about the same file, which is exactly the comparison the
    validation depends on.

    Deliberately portable: no Windows headers, so the offline tool still builds
    anywhere. Compressed formats are not this header's business -- see
    MediaFile.h, which is Windows-only and hands back the same AudioBuffer. */

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fmdr {

/** Decoded audio, split into channels. Sources with more than two channels are
    reduced to their front pair, which is what the engine does with a live
    endpoint too; a mono source is doubled up rather than left half silent. */
struct AudioBuffer {
	uint32_t sampleRate = 48000;
	int bits = 24;        // of the file it came from, so a tool can write it back
	bool isFloat = false;
	std::vector<float> l;
	std::vector<float> r;

	size_t frames() const {
		return l.size();
	}

	double seconds() const {
		return (sampleRate > 0) ? double(l.size()) / double(sampleRate) : 0.0;
	}
};


namespace wav {

inline uint32_t readU32(const uint8_t* p) {
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint16_t readU16(const uint8_t* p) {
	return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

inline void writeU32(std::ofstream& out, uint32_t v) {
	const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
	out.write(reinterpret_cast<const char*>(b), 4);
}

inline void writeU16(std::ofstream& out, uint16_t v) {
	const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
	out.write(reinterpret_cast<const char*>(b), 2);
}

/** One interleaved sample -> float, matching the engine's own scaling. */
inline float decode(const uint8_t* p, int bits, bool isFloat) {
	if (isFloat) {
		float v = 0.f;
		std::memcpy(&v, p, sizeof(v));
		return v;
	}
	switch (bits) {
		case 8:
			// 8-bit WAV is unsigned, unlike every other width.
			return (float(p[0]) - 128.f) * (1.f / 128.f);
		case 16:
			return float(int16_t(readU16(p))) * (1.f / 32768.f);
		case 24: {
			const int32_t v = int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16)
			                          | (uint32_t(p[2]) << 24));
			return float(v) * (1.f / 2147483648.f);
		}
		case 32:
			return float(int32_t(readU32(p))) * (1.f / 2147483648.f);
		default:
			return 0.f;
	}
}

inline void encode(std::ofstream& out, float v, int bits, bool isFloat) {
	if (isFloat) {
		out.write(reinterpret_cast<const char*>(&v), sizeof(v));
		return;
	}
	if (v > 1.f) v = 1.f;
	if (v < -1.f) v = -1.f;
	switch (bits) {
		case 16:
			writeU16(out, uint16_t(int16_t(v * 32767.f)));
			break;
		case 24: {
			const int32_t s = int32_t(v * 8388607.f);
			const uint8_t b[3] = {uint8_t(s & 0xff), uint8_t((s >> 8) & 0xff),
			                      uint8_t((s >> 16) & 0xff)};
			out.write(reinterpret_cast<const char*>(b), 3);
			break;
		}
		default:
			writeU32(out, uint32_t(int32_t(double(v) * 2147483647.0)));
			break;
	}
}

} // namespace wav


/** Parses a whole file already in memory. Split out from the path overloads
    because Windows needs a wide path and everything else needs a narrow one,
    and the parsing is the part worth having exactly once. `err` never names the
    file -- the caller knows which one it asked for and prefixes it. */
inline bool parseWav(const std::vector<uint8_t>& file, AudioBuffer& audio, std::string& err) {
	if (file.size() < 44 || std::memcmp(file.data(), "RIFF", 4) != 0
	    || std::memcmp(file.data() + 8, "WAVE", 4) != 0) {
		err = "not a RIFF/WAVE file";
		return false;
	}

	uint16_t channels = 0;
	uint16_t tag = 0;
	bool haveFmt = false;
	size_t dataAt = 0, dataSize = 0;

	// Walk the chunks rather than assuming the canonical 44-byte layout: real
	// files carry LIST, fact and JUNK chunks in any order.
	size_t pos = 12;
	while (pos + 8 <= file.size()) {
		const uint8_t* id = file.data() + pos;
		const uint32_t size = wav::readU32(file.data() + pos + 4);
		const size_t body = pos + 8;
		if (body + size > file.size() && std::memcmp(id, "data", 4) != 0)
			break;

		if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16) {
			tag = wav::readU16(file.data() + body);
			channels = wav::readU16(file.data() + body + 2);
			audio.sampleRate = wav::readU32(file.data() + body + 4);
			audio.bits = int(wav::readU16(file.data() + body + 14));
			if (tag == 0xFFFE && size >= 40) {
				// WAVE_FORMAT_EXTENSIBLE: the real format is the first two
				// bytes of the sub-format GUID.
				tag = wav::readU16(file.data() + body + 24);
			}
			haveFmt = true;
		}
		else if (std::memcmp(id, "data", 4) == 0) {
			dataAt = body;
			dataSize = size;
			// A truncated or streaming file can claim more than it has.
			if (dataAt + dataSize > file.size())
				dataSize = file.size() - dataAt;
		}

		pos = body + size + (size & 1);  // chunks are word-aligned
	}

	if (!haveFmt || dataAt == 0) {
		err = "no fmt or data chunk";
		return false;
	}
	audio.isFloat = (tag == 3);
	if (tag != 1 && tag != 3) {
		err = "only PCM and IEEE float WAV are supported (this one is format "
		    + std::to_string(tag) + ")";
		return false;
	}
	if (audio.isFloat && audio.bits != 32) {
		err = "only 32-bit float is supported";
		return false;
	}
	if (!audio.isFloat && audio.bits != 8 && audio.bits != 16 && audio.bits != 24
	    && audio.bits != 32) {
		err = "unsupported bit depth " + std::to_string(audio.bits);
		return false;
	}
	if (channels < 1) {
		err = "no channels";
		return false;
	}

	const size_t sampleBytes = size_t(audio.bits) / 8;
	const size_t frameBytes = sampleBytes * channels;
	const size_t frames = frameBytes ? (dataSize / frameBytes) : 0;
	audio.l.resize(frames);
	audio.r.resize(frames);

	// More than two channels: take the front pair, as the engine does.
	const size_t rightIndex = (channels > 1) ? 1 : 0;
	for (size_t i = 0; i < frames; i++) {
		const uint8_t* frame = file.data() + dataAt + i * frameBytes;
		audio.l[i] = wav::decode(frame, audio.bits, audio.isFloat);
		audio.r[i] = wav::decode(frame + rightIndex * sampleBytes, audio.bits, audio.isFloat);
	}
	return true;
}


/** Slurps the whole file. These are test files, not streams, and the caller
    wants random access to all of it anyway. */
template <class Stream>
inline bool readWavStream(Stream& in, AudioBuffer& audio, std::string& err) {
	if (!in) {
		err = "could not open it";
		return false;
	}
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
	                                 std::istreambuf_iterator<char>());
	return parseWav(bytes, audio, err);
}

inline bool readWav(const std::string& path, AudioBuffer& audio, std::string& err) {
	std::ifstream in(path, std::ios::binary);
	return readWavStream(in, audio, err);
}

#ifdef _WIN32
/** Wide-path overload. MSVC's fstream takes wchar_t paths as an extension, and
    it is the only way to open a file whose name is not representable in the
    process code page -- which for a folder of client material is not exotic. */
inline bool readWav(const std::wstring& path, AudioBuffer& audio, std::string& err) {
	std::ifstream in(path.c_str(), std::ios::binary);
	return readWavStream(in, audio, err);
}
#endif


inline bool writeWav(const std::string& path, const std::vector<float>& l,
                     const std::vector<float>& r, bool monoOut, uint32_t sampleRate,
                     int bits, bool isFloat, std::string& err) {
	std::ofstream out(path, std::ios::binary);
	if (!out) {
		err = "could not write " + path;
		return false;
	}
	const uint16_t channels = monoOut ? 1 : 2;
	const uint16_t sampleBytes = uint16_t(bits / 8);
	const uint16_t blockAlign = uint16_t(sampleBytes * channels);
	const uint32_t dataBytes = uint32_t(l.size()) * blockAlign;
	// Float files want the 18-byte fmt chunk and a fact chunk; integer files
	// take the plain 16-byte one.
	const uint32_t fmtSize = isFloat ? 18u : 16u;
	const uint32_t factSize = isFloat ? 12u : 0u;

	out.write("RIFF", 4);
	wav::writeU32(out, 4 + (8 + fmtSize) + factSize + 8 + dataBytes);
	out.write("WAVE", 4);

	out.write("fmt ", 4);
	wav::writeU32(out, fmtSize);
	wav::writeU16(out, isFloat ? 3 : 1);
	wav::writeU16(out, channels);
	wav::writeU32(out, sampleRate);
	wav::writeU32(out, sampleRate * blockAlign);
	wav::writeU16(out, blockAlign);
	wav::writeU16(out, uint16_t(bits));
	if (isFloat)
		wav::writeU16(out, 0);  // cbSize

	if (isFloat) {
		out.write("fact", 4);
		wav::writeU32(out, 4);
		wav::writeU32(out, uint32_t(l.size()));
	}

	out.write("data", 4);
	wav::writeU32(out, dataBytes);
	for (size_t i = 0; i < l.size(); i++) {
		if (monoOut) {
			wav::encode(out, 0.5f * (l[i] + r[i]), bits, isFloat);
		}
		else {
			wav::encode(out, l[i], bits, isFloat);
			wav::encode(out, r[i], bits, isFloat);
		}
	}
	return out.good();
}

} // namespace fmdr
