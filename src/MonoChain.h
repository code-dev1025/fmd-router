#pragma once

// FmdDsp.hpp reaches for four Rack symbols -- clamp(), random::uniform(),
// engine::Input and M_PI. The VCV project already ships stand-ins for exactly
// those in tests/fmd_test_shim.hpp so the filter can be built without Rack, and
// this app uses that same file rather than keeping a second copy in sync. The
// shim's RNG is a plain LCG with no allocation and no lock, which is what the
// real-time thread needs anyway.
#define FMD_DSP_TEST_SHIM
#include "FmdDsp.hpp"

#include <atomic>
#include <cmath>

namespace fmdr {

enum Module {
	ModFlowerChild = 0,
	ModShapedResonator = 1,
	ModSuperLove = 2,
	NumModules = 3,
};


/** A snapshot of the controls, taken once per render block. Plain values: the
    audio thread must never read the atomics twice and see two different worlds
    within one block. */
struct ChainSettings {
	int module = ModFlowerChild;
	int mode = fmd::FilterCore::MODE_LP12;
	bool aggressive = false;

	float freqOct = 6.f;   // octaves above 20 Hz, 0..10 spans 20 Hz..20.5 kHz
	float res = 0.f;
	float drive = 0.f;
	float spread = 0.f;
	float grit = 0.f;
	float clip = 0.f;

	float mix = 1.f;       // 0 = mono sum only, 1 = filtered only
	float outGain = 1.f;   // linear
};


/** The controls as the GUI thread owns them. Every field is written from the
    window thread and read from the audio thread, so every field is atomic and
    nothing is ever locked. Relaxed ordering throughout: these are independent
    knobs, not a protocol, and a block that catches FREQ one tick before RES is
    indistinguishable from a block that caught both. */
struct ChainParams {
	std::atomic<int> module{ModFlowerChild};
	std::atomic<int> mode{fmd::FilterCore::MODE_LP12};
	std::atomic<int> aggressive{0};

	std::atomic<float> freqOct{6.f};
	std::atomic<float> res{0.f};
	std::atomic<float> drive{0.f};
	std::atomic<float> spread{0.f};
	std::atomic<float> grit{0.f};
	std::atomic<float> clip{0.f};

	std::atomic<float> mix{1.f};
	std::atomic<float> outGain{1.f};

	ChainSettings snapshot() const {
		ChainSettings s;
		s.module = module.load(std::memory_order_relaxed);
		s.mode = mode.load(std::memory_order_relaxed);
		s.aggressive = aggressive.load(std::memory_order_relaxed) != 0;
		s.freqOct = freqOct.load(std::memory_order_relaxed);
		s.res = res.load(std::memory_order_relaxed);
		s.drive = drive.load(std::memory_order_relaxed);
		s.spread = spread.load(std::memory_order_relaxed);
		s.grit = grit.load(std::memory_order_relaxed);
		s.clip = clip.load(std::memory_order_relaxed);
		s.mix = mix.load(std::memory_order_relaxed);
		s.outGain = outGain.load(std::memory_order_relaxed);
		return s;
	}
};


/*  The stereo-to-mono FX chain.

        L ─┐
           ├─► M = (L+R)/2 ─┬─────────────────────────────► dry
        R ─┘                │
                            ├─► FilterCore ch0  (freq × 2^-spread) ─┐
                            └─► FilterCore ch1  (freq × 2^+spread) ─┴─► wet
                                                                      │
                              out = dry·(1-mix) + wet·mix, × outGain ◄─┘

    The one design decision worth stating, because it is not the obvious one:
    the mono sum is fed to *both* channels of FilterCore rather than collapsing
    after the filter.

    FilterCore's SPREAD detunes its two channels by up to ±1 octave. On a stereo
    module that widens the image. On a mono output an image is exactly what we
    do not have, so collapsing afterwards would leave SPREAD as a knob that does
    nothing -- and every knob doing something audible is the whole brief of this
    codebase. Feeding the same mono signal through both detuned voices and
    summing turns SPREAD into a second resonant peak instead: at zero the two
    voices coincide and it is one filter, and opening it walks them apart into a
    two-peak / comb character. Same knob, same range, meaningful in mono.

    Dry is the mono sum, not the original stereo, so MIX at 0 is already the
    downmix and the user never hears un-collapsed audio at any mix setting. */
class MonoChain {
public:
	void prepare(double sampleRate) {
		sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
		core_.setSampleRate(float(sampleRate_));
		// ~10 ms one-pole on every continuous control. Slow enough to kill
		// zipper noise on a dragged slider, fast enough to feel attached to it.
		smoothCoeff_ = 1.f - std::exp(-1.f / float(0.010 * sampleRate_));
		reset();
	}

	void reset() {
		core_.reset();
		primed_ = false;
	}

	/** Consumes one stereo input frame and returns one mono output sample. */
	float processFrame(float l, float r, const ChainSettings& s) {
		// Jump rather than glide the first time, so opening the app on a
		// non-default patch does not audibly sweep into position.
		if (!primed_) {
			cur_ = s;
			primed_ = true;
		}

		// Discrete controls switch instantly; the filter is reset on a module
		// change because the three voicings drive the resonance path
		// differently and carrying state across can bark.
		if (s.module != cur_.module) {
			core_.reset();
			cur_.module = s.module;
		}
		cur_.mode = s.mode;
		cur_.aggressive = s.aggressive;

		glide(cur_.freqOct, s.freqOct);
		glide(cur_.res, s.res);
		glide(cur_.drive, s.drive);
		glide(cur_.spread, s.spread);
		glide(cur_.grit, s.grit);
		glide(cur_.clip, s.clip);
		glide(cur_.mix, s.mix);
		glide(cur_.outGain, s.outGain);

		fmd::FilterParams p;
		p.freqHz = fmd::freqFromOctaves(cur_.freqOct, 0.f);
		p.res = cur_.res;
		p.drive = cur_.drive;
		p.spread = cur_.spread;
		p.grit = cur_.grit;
		p.clip = cur_.clip;

		// The three products differ only in how they use the shared core --
		// this mirrors the module .cpp files in fmd-vcv exactly.
		switch (cur_.module) {
			case ModShapedResonator:
				p.mode = cur_.mode;
				p.gritIsCrunch = true;   // CRNCH bends the resonant peak
				p.aggressive = false;
				break;
			case ModSuperLove:
				p.mode = cur_.mode;      // LP6 / LP12 / BP / HP slider
				p.gritIsCrunch = false;  // NOISE mixes into the filter input
				p.aggressive = false;
				break;
			case ModFlowerChild:
			default:
				p.mode = fmd::FilterCore::MODE_LP12;  // no mode control on this panel
				p.gritIsCrunch = false;
				p.aggressive = cur_.aggressive;       // AGGR switch
				break;
		}

		const float mono = (l + r) * 0.5f;
		const float in[2] = {mono, mono};
		float out[2];
		core_.process(in, out, p);

		const float wet = (out[0] + out[1]) * 0.5f;
		float y = mono + (wet - mono) * cur_.mix;
		y *= cur_.outGain;

		// The core already guards its own integrators, but a NaN arriving from
		// a misbehaving capture device must not be handed to the endpoint.
		if (!std::isfinite(y))
			y = 0.f;
		return y;
	}

private:
	void glide(float& state, float target) {
		state += smoothCoeff_ * (target - state);
	}

	fmd::FilterCore core_;
	ChainSettings cur_;
	double sampleRate_ = 48000.0;
	float smoothCoeff_ = 0.01f;
	bool primed_ = false;
};

} // namespace fmdr
