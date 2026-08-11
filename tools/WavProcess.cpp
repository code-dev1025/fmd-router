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

#include "AudioFile.h"
#include "RealMonoChain.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

using fmdr::AudioBuffer;


// ------------------------------------------------------------- demo signal

/** A file that makes the difference obvious: four sections, each three
    seconds, with the case that matters in the middle. */
void makeDemo(AudioBuffer& audio) {
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
		AudioBuffer demo;
		makeDemo(demo);
		if (!fmdr::writeWav(argv[2], demo.l, demo.r, false, demo.sampleRate, demo.bits,
		                    demo.isFloat, err)) {
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

	AudioBuffer in;
	if (!fmdr::readWav(inPath, in, err)) {
		// The reader does not know which file it was handed, so the path is
		// added here rather than repeated inside every message it can produce.
		std::printf("error: %s: %s\n", inPath.c_str(), err.c_str());
		if (err.find("only PCM") != std::string::npos)
			std::printf("  convert it first, e.g. ffmpeg -i in.mp3 -c:a pcm_s24le out.wav\n");
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
	if (!fmdr::writeWav(outPath, outL, outR, monoOut, in.sampleRate, bits, isFloat, err)) {
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
