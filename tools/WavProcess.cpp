/*  realmono-wav -- run a WAV file through the Real Mono chain offline.

    The app can only be judged by ear through a live capture path, which needs
    a virtual cable and two endpoints. This does the same job without either:
    it feeds a file through the *same* RealMonoChain the audio thread runs, so
    what comes out is what the app would have played.

    It exists for the validation the brief asks for -- A/B against the client's
    before/after references and their test material -- and for hearing the
    difference on a machine with nothing installed.

        realmono-wav --demo demo.wav              write a test signal
        realmono-wav demo.wav out.wav             the product
        realmono-wav demo.wav ref.wav --preset midonly    classic mono, to A/B

    The output is delay-compensated by default, so `out.wav` lines up with the
    input sample for sample and the two can be dropped onto adjacent tracks in
    a DAW without nudging anything.

    No Windows API here: this is portable C++ and builds anywhere the chain
    does.                                                                     */

#include "RealMonoChain.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------ WAV I/O

struct Audio {
	uint32_t sampleRate = 48000;
	int bits = 24;
	bool isFloat = false;
	std::vector<float> l;
	std::vector<float> r;

	size_t frames() const { return l.size(); }
};

uint32_t readU32(const uint8_t* p) {
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint16_t readU16(const uint8_t* p) {
	return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

void writeU32(std::ofstream& out, uint32_t v) {
	const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
	out.write(reinterpret_cast<const char*>(b), 4);
}

void writeU16(std::ofstream& out, uint16_t v) {
	const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
	out.write(reinterpret_cast<const char*>(b), 2);
}

/** One interleaved sample -> float, matching the engine's own scaling. */
float decode(const uint8_t* p, int bits, bool isFloat) {
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

bool readWav(const std::string& path, Audio& audio, std::string& err) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		err = "could not open " + path;
		return false;
	}
	std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)),
	                          std::istreambuf_iterator<char>());
	if (file.size() < 44 || std::memcmp(file.data(), "RIFF", 4) != 0
	    || std::memcmp(file.data() + 8, "WAVE", 4) != 0) {
		err = path + " is not a RIFF/WAVE file";
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
		const uint32_t size = readU32(file.data() + pos + 4);
		const size_t body = pos + 8;
		if (body + size > file.size() && std::memcmp(id, "data", 4) != 0)
			break;

		if (std::memcmp(id, "fmt ", 4) == 0 && size >= 16) {
			tag = readU16(file.data() + body);
			channels = readU16(file.data() + body + 2);
			audio.sampleRate = readU32(file.data() + body + 4);
			audio.bits = int(readU16(file.data() + body + 14));
			if (tag == 0xFFFE && size >= 40) {
				// WAVE_FORMAT_EXTENSIBLE: the real format is the first two
				// bytes of the sub-format GUID.
				tag = readU16(file.data() + body + 24);
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
		err = path + " has no fmt or data chunk";
		return false;
	}
	audio.isFloat = (tag == 3);
	if (tag != 1 && tag != 3) {
		err = path + ": only PCM and IEEE float WAV are supported (this one is "
		    + std::to_string(tag) + "). Convert it first, e.g. "
		      "ffmpeg -i in.mp3 -c:a pcm_s24le out.wav";
		return false;
	}
	if (audio.isFloat && audio.bits != 32) {
		err = path + ": only 32-bit float is supported";
		return false;
	}
	if (!audio.isFloat && audio.bits != 8 && audio.bits != 16 && audio.bits != 24
	    && audio.bits != 32) {
		err = path + ": unsupported bit depth " + std::to_string(audio.bits);
		return false;
	}
	if (channels < 1) {
		err = path + ": no channels";
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
		audio.l[i] = decode(frame, audio.bits, audio.isFloat);
		audio.r[i] = decode(frame + rightIndex * sampleBytes, audio.bits, audio.isFloat);
	}
	return true;
}

void encode(std::ofstream& out, float v, int bits, bool isFloat) {
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

bool writeWav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r,
              bool monoOut, uint32_t sampleRate, int bits, bool isFloat, std::string& err) {
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
	writeU32(out, 4 + (8 + fmtSize) + factSize + 8 + dataBytes);
	out.write("WAVE", 4);

	out.write("fmt ", 4);
	writeU32(out, fmtSize);
	writeU16(out, isFloat ? 3 : 1);
	writeU16(out, channels);
	writeU32(out, sampleRate);
	writeU32(out, sampleRate * blockAlign);
	writeU16(out, blockAlign);
	writeU16(out, uint16_t(bits));
	if (isFloat)
		writeU16(out, 0);  // cbSize

	if (isFloat) {
		out.write("fact", 4);
		writeU32(out, 4);
		writeU32(out, uint32_t(l.size()));
	}

	out.write("data", 4);
	writeU32(out, dataBytes);
	for (size_t i = 0; i < l.size(); i++) {
		if (monoOut) {
			encode(out, 0.5f * (l[i] + r[i]), bits, isFloat);
		}
		else {
			encode(out, l[i], bits, isFloat);
			encode(out, r[i], bits, isFloat);
		}
	}
	return out.good();
}


// ------------------------------------------------------------- demo signal

/** A file that makes the difference obvious: four sections, each three
    seconds, with the case that matters in the middle. */
void makeDemo(Audio& audio) {
	audio.sampleRate = 48000;
	audio.bits = 24;
	audio.isFloat = false;

	const size_t rate = audio.sampleRate;
	const size_t section = rate * 3;
	const size_t frames = section * 4;
	audio.l.assign(frames, 0.f);
	audio.r.assign(frames, 0.f);

	auto tone = [rate](double hz, size_t i) {
		return std::sin(2.0 * kPi * hz * double(i) / double(rate));
	};

	for (size_t i = 0; i < frames; i++) {
		const size_t part = i / section;
		const size_t within = i % section;

		// 30 ms of fade at each end of a section, so the transitions are not
		// clicks that get blamed on the chain.
		const double fadeLen = 0.03 * double(rate);
		double env = 1.0;
		if (double(within) < fadeLen)
			env = double(within) / fadeLen;
		else if (double(section - within) < fadeLen)
			env = double(section - within) / fadeLen;
		env *= 0.3;

		double left = 0.0, right = 0.0;
		switch (part) {
			case 0:
				// Correlated: identical in both channels. Real Mono and a plain
				// downmix must sound the same here.
				left = right = tone(220.0, i) + 0.5 * tone(440.0, i);
				break;
			case 1: {
				// Pure Side: R = -L. A plain downmix cancels this to silence.
				const double v = tone(330.0, i) + 0.5 * tone(660.0, i);
				left = v;
				right = -v;
				break;
			}
			case 2:
				// Hard panned left: half Mid, half Side.
				left = tone(523.25, i);
				right = 0.0;
				break;
			default: {
				// A widened mix: mono bass with an anti-phase top, which is
				// what actually goes missing on real programme material.
				const double bass = tone(110.0, i);
				const double top = 0.6 * tone(1500.0, i) + 0.4 * tone(2500.0, i);
				left = bass + top;
				right = bass - top;
				break;
			}
		}
		audio.l[i] = float(left * env);
		audio.r[i] = float(right * env);
	}
}


// ------------------------------------------------------------------ driver

double toDb(double linear) {
	return (linear <= 1e-12) ? -240.0 : 20.0 * std::log10(linear);
}

void usage() {
	std::printf(
	    "realmono-wav -- run a WAV through the Real Mono chain\n\n"
	    "  realmono-wav --demo <out.wav>          write a 12 s test signal\n"
	    "  realmono-wav <in.wav> <out.wav> [options]\n\n"
	    "Options\n"
	    "  --preset <name>     default | hq | midonly | monolf\n"
	    "  --quality <name>    hq | short | allpass          (Stage 3)\n"
	    "  --hq                highest quality mode, -3 dB input trim\n"
	    "  --ceiling <dB>      limiter ceiling, default -0.3\n"
	    "  --lookahead <ms>    5..10, default 5\n"
	    "  --hpf <Hz>          enable Stage 1 at this corner\n"
	    "  --hpf-slope <n>     6 | 12 | 24\n"
	    "  --mid <dB> --side <dB> --inject <dB> --gain <dB>\n"
	    "  --no-rotate         bypass Stage 3\n"
	    "  --no-ms             bypass Stage 2\n"
	    "  --no-commit         bypass Stage 4, leaving M/S in stereo\n"
	    "  --no-limiter        bypass Stage 5\n"
	    "  --bypass            global bypass: the input, unchanged\n"
	    "  --mono-out          write one channel instead of dual mono\n"
	    "  --float             write 32-bit float instead of the input's format\n"
	    "  --no-align          do not compensate the chain's latency\n\n"
	    "The output is latency-compensated by default, so it lines up with the\n"
	    "input sample for sample for A/B on adjacent DAW tracks.\n\n"
	    "MP3 in, WAV out: convert first, e.g.\n"
	    "  ffmpeg -i \"stereo flute and mono guitar.mp3\" -c:a pcm_s24le test.wav\n");
}

bool parseFloatArg(int argc, char** argv, int& i, float& out) {
	if (i + 1 >= argc)
		return false;
	out = float(std::atof(argv[++i]));
	return true;
}

} // namespace


int main(int argc, char** argv) {
	if (argc < 2) {
		usage();
		return 1;
	}

	std::string err;

	if (std::strcmp(argv[1], "--demo") == 0) {
		if (argc < 3) {
			std::printf("--demo needs an output path\n");
			return 1;
		}
		Audio demo;
		makeDemo(demo);
		if (!writeWav(argv[2], demo.l, demo.r, false, demo.sampleRate, demo.bits, demo.isFloat, err)) {
			std::printf("error: %s\n", err.c_str());
			return 1;
		}
		std::printf("wrote %s: 12 s, %u Hz, stereo\n", argv[2], demo.sampleRate);
		std::printf("  0-3 s   correlated      -- must sound the same either way\n");
		std::printf("  3-6 s   pure Side       -- silent in a plain downmix\n");
		std::printf("  6-9 s   hard panned     -- -6 dB downmixed, -3 dB here\n");
		std::printf("  9-12 s  widened mix     -- the top vanishes in a plain downmix\n");
		return 0;
	}

	if (argc < 3 || argv[1][0] == '-') {
		usage();
		return 1;
	}

	const std::string inPath = argv[1];
	const std::string outPath = argv[2];

	fmdr::RealMonoSettings s = fmdr::presetSettings(fmdr::PresetRealMonoDefault);
	bool monoOut = false;
	bool forceFloat = false;
	bool align = true;

	for (int i = 3; i < argc; i++) {
		const std::string arg = argv[i];
		float value = 0.f;

		if (arg == "--preset" && i + 1 < argc) {
			const std::string name = argv[++i];
			if (name == "default")      s = fmdr::presetSettings(fmdr::PresetRealMonoDefault);
			else if (name == "hq")      s = fmdr::presetSettings(fmdr::PresetHighestQuality);
			else if (name == "midonly") s = fmdr::presetSettings(fmdr::PresetLabSafeMidOnly);
			else if (name == "monolf")  s = fmdr::presetSettings(fmdr::PresetLabWithMonoLF);
			else { std::printf("unknown preset: %s\n", name.c_str()); return 1; }
		}
		else if (arg == "--quality" && i + 1 < argc) {
			const std::string name = argv[++i];
			if (name == "hq")           s.quality = fmdr::QualityHqLinear;
			else if (name == "short")   s.quality = fmdr::QualityShortLinear;
			else if (name == "allpass") s.quality = fmdr::QualityAllpass;
			else { std::printf("unknown quality: %s\n", name.c_str()); return 1; }
		}
		else if (arg == "--hq")          s.hqMode = true;
		else if (arg == "--no-rotate")   s.rotateEnabled = false;
		else if (arg == "--no-ms")       s.msEnabled = false;
		else if (arg == "--no-commit")   s.commitEnabled = false;
		else if (arg == "--no-limiter")  s.limiterEnabled = false;
		else if (arg == "--bypass")      s.enabled = false;
		else if (arg == "--mono-out")    monoOut = true;
		else if (arg == "--float")       forceFloat = true;
		else if (arg == "--no-align")    align = false;
		else if (arg == "--ceiling" && parseFloatArg(argc, argv, i, value))   s.ceilingDb = value;
		else if (arg == "--lookahead" && parseFloatArg(argc, argv, i, value)) s.lookaheadMs = value;
		else if (arg == "--mid" && parseFloatArg(argc, argv, i, value))       s.midGainDb = value;
		else if (arg == "--side" && parseFloatArg(argc, argv, i, value))      s.sideGainDb = value;
		else if (arg == "--inject" && parseFloatArg(argc, argv, i, value))    s.sideInjectDb = value;
		else if (arg == "--gain" && parseFloatArg(argc, argv, i, value))      s.outputGainDb = value;
		else if (arg == "--hpf" && parseFloatArg(argc, argv, i, value)) {
			s.hpfEnabled = true;
			s.hpfHz = value;
		}
		else if (arg == "--hpf-slope" && parseFloatArg(argc, argv, i, value)) {
			s.hpfSlope = int(value);
		}
		else {
			std::printf("unknown option: %s\n\n", arg.c_str());
			usage();
			return 1;
		}
	}

	Audio in;
	if (!readWav(inPath, in, err)) {
		std::printf("error: %s\n", err.c_str());
		return 1;
	}
	if (in.frames() == 0) {
		std::printf("error: %s has no audio\n", inPath.c_str());
		return 1;
	}

	fmdr::RealMonoChain chain;
	chain.prepare(double(in.sampleRate));

	// One priming frame so the chain has adopted the settings and can report
	// the latency it is actually going to add.
	float probeL = 0.f, probeR = 0.f;
	chain.process(0.f, 0.f, s, probeL, probeR);
	chain.reset();
	chain.prepare(double(in.sampleRate));
	chain.process(0.f, 0.f, s, probeL, probeR);
	const size_t latency = align ? size_t(chain.latencySamples()) : 0;

	// Run past the end by the latency so nothing is lost off the back when the
	// output is shifted forward to compensate.
	const size_t total = in.frames() + latency;
	std::vector<float> outL(total), outR(total);
	double inPeak = 0.0, outPeak = 0.0, minGain = 1.0;

	for (size_t i = 0; i < total; i++) {
		const float l = (i < in.frames()) ? in.l[i] : 0.f;
		const float r = (i < in.frames()) ? in.r[i] : 0.f;
		inPeak = std::max(inPeak, double(std::max(std::abs(l), std::abs(r))));
		chain.process(l, r, s, outL[i], outR[i]);
		minGain = std::min(minGain, double(chain.limiterGain()));
		outPeak = std::max(outPeak, double(std::max(std::abs(outL[i]), std::abs(outR[i]))));
	}

	if (latency > 0) {
		outL.erase(outL.begin(), outL.begin() + long(latency));
		outR.erase(outR.begin(), outR.begin() + long(latency));
	}
	else {
		outL.resize(in.frames());
		outR.resize(in.frames());
	}

	const int bits = forceFloat ? 32 : in.bits;
	const bool isFloat = forceFloat ? true : in.isFloat;
	if (!writeWav(outPath, outL, outR, monoOut, in.sampleRate, bits, isFloat, err)) {
		std::printf("error: %s\n", err.c_str());
		return 1;
	}

	std::printf("%s -> %s\n", inPath.c_str(), outPath.c_str());
	std::printf("  %zu frames, %u Hz, %d-bit %s\n", in.frames(), in.sampleRate, bits,
	            isFloat ? "float" : "PCM");
	std::printf("  chain latency %d samples (%.2f ms)%s\n",
	            chain.latencySamples(),
	            1000.0 * chain.latencySamples() / double(in.sampleRate),
	            align ? ", compensated" : ", NOT compensated");
	std::printf("  input peak  %.4f (%.2f dBFS)\n", inPeak, toDb(inPeak));
	std::printf("  output peak %.4f (%.2f dBFS)\n", outPeak, toDb(outPeak));
	std::printf("  limiter did %.2f dB at its hardest\n", toDb(minGain));
	return 0;
}
