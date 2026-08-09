#pragma once

#include "RealMono.h"

#include <atomic>
#include <cstdint>

namespace fmdr {

/*  ================================================================= Real Mono

    The client's DAW recipe, in one class.

        stereo in
          -> Stage 0  HQ input trim (-3 dB, off by default)
          -> Stage 1  Side high-pass          (lab; BYPASSED by default)
          -> Stage 2  M/S split               M = (L+R)/2,  S = (L-R)/2
          -> Stage 3  +90 deg rotation of S   (Hilbert, Mid delay-aligned)
          -> Stage 4  mono commit             mono = M + rot(S)
          -> Stage 5  look-ahead limiter      ceiling -0.3 dBFS
          -> dual mono out                    outL = outR

    Why the rotation, in one line: a pure difference signal (R = -L) has no Mid
    at all, so a plain (L+R)/2 downmix cancels it to silence. Rotating the Side
    bus by 90 degrees turns it into something that survives the sum, which is
    the entire product.

    Where the +/-90 goes. The brief describes a Side-only *stereo bus* with
    +90 on its left and -90 on its right. That bus is (S, -S), so after
    rotation it is (R+(S), -R-(S)) = (R+(S), R+(S)) -- because a -90 rotation
    is the negative of a +90 one. Both channels are identical, so its mono
    downmix is exactly R+(S), and the whole two-channel arrangement collapses
    to a single rotation of S. That is what is implemented; it is the same
    signal Voxengo MSED into PHA-979 produces, with one rotator instead of two.

        mono = M + R+(S) = 0.5*[(L+R) + R+(L-R)]

    Levels: M and S use the 0.5 convention, so a purely correlated input comes
    out at unity and a purely anti-phase input also comes out at unity. What
    does overshoot is programme material: R+(S) has the same magnitude spectrum
    as S but a different waveform, so its peaks land in different places and
    the sum can exceed max(|L|,|R|) by around 3 dB. That is the overshoot the
    client reported, and Stage 5 is what catches it.

    Latency: Stage 3 in its linear-phase modes has a group delay of half its
    tap count, and the Mid path is delayed to match; Stage 5 adds its
    look-ahead. latencySamples() reports the total, and the engine adds it to
    the round-trip figure on screen.                                          */

enum HilbertQuality {
	QualityHqLinear = 0,     // 1023-tap FIR, 511 samples, flat from ~120 Hz
	QualityShortLinear = 1,  //  255-tap FIR, 127 samples, flat from ~300 Hz
	QualityAllpass = 2,      // IIR quadrature pair, 1 sample, non-linear phase
	NumQualities = 3,
};

enum CommitMode {
	CommitMidPlusRotatedSide = 0,  // the product
	CommitSum = 1,                 // 0.5*(L+R), no gains -- the reference downmix
	CommitMidOnly = 2,             // Mid path with its gain, Side discarded
	CommitSideEnergyFold = 3,      // lab: sign(M) * sqrt(M^2 + S^2)
	CommitPolarityMatrix = 4,      // lab: whichever of Mid / Side has the energy
	NumCommitModes = 5,
};

enum HpfMode {
	HpfSide = HighpassChain::ModeSideHpf,
	HpfCrossoverMono = HighpassChain::ModeCrossoverMono,
};


/** A snapshot of every control, taken once per render block. Plain values: the
    audio thread must never read the atomics twice within one block and see two
    different worlds. */
struct RealMonoSettings {
	// Global. Off is a stereo pass-through, which is the A/B the client will
	// spend most of their time on.
	bool enabled = true;

	// Stage 0 -- highest quality mode: 3 dB of headroom taken at the input so
	// the Mid + rotated Side sum does not have to be won back by the limiter.
	bool hqMode = false;

	// Stage 1 -- lab only. The client does not use a high-pass; this ships
	// bypassed and the UI says so.
	bool hpfEnabled = false;
	float hpfHz = 120.f;
	int hpfSlope = 12;  // 6 | 12 | 24 dB/oct
	int hpfMode = HpfSide;

	// Stage 2
	bool msEnabled = true;
	float midGainDb = 0.f;
	float sideGainDb = 0.f;

	// Stage 3
	bool rotateEnabled = true;
	int quality = QualityHqLinear;

	// Stage 4
	bool commitEnabled = true;
	int commitMode = CommitMidPlusRotatedSide;
	float sideInjectDb = 0.f;
	float outputGainDb = 0.f;

	// Stage 5
	bool limiterEnabled = true;
	float ceilingDb = -0.3f;
	float lookaheadMs = 5.f;
};


enum Preset {
	PresetRealMonoDefault = 0,
	PresetHighestQuality = 1,
	PresetLabSafeMidOnly = 2,
	PresetLabWithMonoLF = 3,
	NumPresets = 4,
};

/** The four presets the brief names. Everything not mentioned stays at the
    shipping default, so a preset is a complete state and not a patch. */
inline RealMonoSettings presetSettings(int preset) {
	RealMonoSettings s;  // = RealMono_Default
	switch (preset) {
		case PresetHighestQuality:
			// Same chain, 3 dB of input headroom so the sum keeps its
			// dynamics. The limiter stays on for what is left.
			s.hqMode = true;
			break;

		case PresetLabSafeMidOnly:
			// Classic mono: the reference to A/B Real Mono against.
			s.rotateEnabled = false;
			s.commitMode = CommitMidOnly;
			break;

		case PresetLabWithMonoLF:
			s.hpfEnabled = true;
			s.hpfHz = 120.f;
			break;

		case PresetRealMonoDefault:
		default:
			break;
	}
	return s;
}

inline const wchar_t* presetName(int preset) {
	switch (preset) {
		case PresetHighestQuality: return L"RealMono_HighestQuality";
		case PresetLabSafeMidOnly: return L"Lab_SafeMidOnly";
		case PresetLabWithMonoLF:  return L"Lab_WithMonoLF";
		default:                   return L"RealMono_Default";
	}
}


/** The controls as the GUI thread owns them: every field atomic, nothing ever
    locked. Relaxed ordering throughout -- these are independent knobs, not a
    protocol, and a block that catches the ceiling one tick before the
    look-ahead is indistinguishable from one that caught both. */
struct RealMonoParams {
	std::atomic<int> enabled{1};
	std::atomic<int> hqMode{0};

	std::atomic<int> hpfEnabled{0};
	std::atomic<float> hpfHz{120.f};
	std::atomic<int> hpfSlope{12};
	std::atomic<int> hpfMode{HpfSide};

	std::atomic<int> msEnabled{1};
	std::atomic<float> midGainDb{0.f};
	std::atomic<float> sideGainDb{0.f};

	std::atomic<int> rotateEnabled{1};
	std::atomic<int> quality{QualityHqLinear};

	std::atomic<int> commitEnabled{1};
	std::atomic<int> commitMode{CommitMidPlusRotatedSide};
	std::atomic<float> sideInjectDb{0.f};
	std::atomic<float> outputGainDb{0.f};

	std::atomic<int> limiterEnabled{1};
	std::atomic<float> ceilingDb{-0.3f};
	std::atomic<float> lookaheadMs{5.f};

	RealMonoSettings snapshot() const {
		RealMonoSettings s;
		s.enabled = enabled.load(std::memory_order_relaxed) != 0;
		s.hqMode = hqMode.load(std::memory_order_relaxed) != 0;

		s.hpfEnabled = hpfEnabled.load(std::memory_order_relaxed) != 0;
		s.hpfHz = hpfHz.load(std::memory_order_relaxed);
		s.hpfSlope = hpfSlope.load(std::memory_order_relaxed);
		s.hpfMode = hpfMode.load(std::memory_order_relaxed);

		s.msEnabled = msEnabled.load(std::memory_order_relaxed) != 0;
		s.midGainDb = midGainDb.load(std::memory_order_relaxed);
		s.sideGainDb = sideGainDb.load(std::memory_order_relaxed);

		s.rotateEnabled = rotateEnabled.load(std::memory_order_relaxed) != 0;
		s.quality = quality.load(std::memory_order_relaxed);

		s.commitEnabled = commitEnabled.load(std::memory_order_relaxed) != 0;
		s.commitMode = commitMode.load(std::memory_order_relaxed);
		s.sideInjectDb = sideInjectDb.load(std::memory_order_relaxed);
		s.outputGainDb = outputGainDb.load(std::memory_order_relaxed);

		s.limiterEnabled = limiterEnabled.load(std::memory_order_relaxed) != 0;
		s.ceilingDb = ceilingDb.load(std::memory_order_relaxed);
		s.lookaheadMs = lookaheadMs.load(std::memory_order_relaxed);
		return s;
	}

	void store(const RealMonoSettings& s) {
		enabled.store(s.enabled ? 1 : 0, std::memory_order_relaxed);
		hqMode.store(s.hqMode ? 1 : 0, std::memory_order_relaxed);

		hpfEnabled.store(s.hpfEnabled ? 1 : 0, std::memory_order_relaxed);
		hpfHz.store(s.hpfHz, std::memory_order_relaxed);
		hpfSlope.store(s.hpfSlope, std::memory_order_relaxed);
		hpfMode.store(s.hpfMode, std::memory_order_relaxed);

		msEnabled.store(s.msEnabled ? 1 : 0, std::memory_order_relaxed);
		midGainDb.store(s.midGainDb, std::memory_order_relaxed);
		sideGainDb.store(s.sideGainDb, std::memory_order_relaxed);

		rotateEnabled.store(s.rotateEnabled ? 1 : 0, std::memory_order_relaxed);
		quality.store(s.quality, std::memory_order_relaxed);

		commitEnabled.store(s.commitEnabled ? 1 : 0, std::memory_order_relaxed);
		commitMode.store(s.commitMode, std::memory_order_relaxed);
		sideInjectDb.store(s.sideInjectDb, std::memory_order_relaxed);
		outputGainDb.store(s.outputGainDb, std::memory_order_relaxed);

		limiterEnabled.store(s.limiterEnabled ? 1 : 0, std::memory_order_relaxed);
		ceilingDb.store(s.ceilingDb, std::memory_order_relaxed);
		lookaheadMs.store(s.lookaheadMs, std::memory_order_relaxed);
	}
};


class RealMonoChain {
public:
	/** The HQ trim the brief specifies, as a linear gain. */
	static constexpr float kHqTrimDb = -3.f;
	static constexpr double kMaxLookaheadMs = 12.0;

	void prepare(double sampleRate) {
		sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;

		fir_.prepare();
		netMid_.prepare();
		netSide_.prepare();
		midDelay_.prepare(size_t(HilbertFir::kHqHalf) + 2);
		sideDelay_.prepare(size_t(HilbertFir::kHqHalf) + 2);
		hpf_.prepare(sampleRate_);
		limiter_.prepare(sampleRate_, kMaxLookaheadMs);

		// ~10 ms on every continuous control: slow enough to kill zipper noise
		// on a dragged slider, fast enough to feel attached to it.
		smoothCoeff_ = 1.f - std::exp(-1.f / float(0.010 * sampleRate_));
		// A structural change (a bypass, a quality switch) cannot be smoothed,
		// so the output is faded through zero around it instead. 3 ms each way
		// is short enough to read as a click-free switch rather than a gap.
		fadeStep_ = 1.f / float(std::max(1.0, 0.003 * sampleRate_));

		// Envelope followers for the lab polarity matrix.
		envAttack_ = 1.f - std::exp(-1.f / float(0.005 * sampleRate_));
		envRelease_ = 1.f - std::exp(-1.f / float(0.200 * sampleRate_));
		selCoeff_ = 1.f - std::exp(-1.f / float(0.050 * sampleRate_));

		reset();
	}

	void reset() {
		fir_.clear();
		netMid_.clear();
		netSide_.clear();
		midDelay_.clear();
		sideDelay_.clear();
		hpf_.clear();
		limiter_.clear();
		primed_ = false;
		fade_ = FadeNone;
		fadeGain_ = 1.f;
		holdLeft_ = 0;
		envMid_ = envSide_ = 0.f;
		selector_ = 0.f;
	}

	/** Total added delay, in samples, of the configuration currently in force.
	    Reported rather than assumed: it is what the engine adds to the
	    round-trip figure, and what a host would have to compensate. */
	int latencySamples() const {
		if (!applied_.enabled)
			return 0;
		int n = rotationDelay(applied_.quality);
		if (applied_.limiterEnabled)
			n += int(limiter_.latency());
		return n;
	}

	/** Limiter gain, linear, 1 when Stage 5 is bypassed. Read per sample by the
	    engine's meter, which is why it does not take a logarithm. */
	float limiterGain() const {
		// The limiter is fed even when it is switched out, so that its delay
		// line is never stale -- but a meter must report what is being heard,
		// so a bypassed stage reads as no reduction.
		return (applied_.enabled && applied_.limiterEnabled) ? limiter_.gain() : 1.f;
	}

	/** Limiter gain reduction, dB, never positive. 0 when Stage 5 is bypassed. */
	float gainReductionDb() const {
		return gainToDb(limiterGain());
	}

	static int rotationDelay(int quality) {
		if (quality == QualityAllpass)
			return 1;  // the I branch's alignment sample
		return HilbertFir::halfLength(quality == QualityShortLinear);
	}

	/** One stereo frame in, one dual-mono frame out. */
	void process(float l, float r, const RealMonoSettings& s, float& outL, float& outR) {
		if (!primed_) {
			applied_ = s;
			structure_ = signature(s);
			primeSmoothing(s);
			primed_ = true;
		}

		// Structural changes -- anything that moves the latency or steps the
		// signal -- are applied at the bottom of a fade rather than mid-sample.
		const uint64_t sig = signature(s);
		if (fade_ == FadeNone && sig != structure_)
			fade_ = FadeOut;

		const RealMonoSettings& a = applied_;

		// Continuous controls follow the live settings; the flags they belong
		// to come from `applied_`, so a bypass never takes effect early.
		// HQ is a 3 dB gain change and nothing else, so it glides like any
		// other gain instead of being faded through zero -- it follows the
		// live setting rather than the applied one.
		const float trim = glide(inTrim_, s.hqMode ? hqTrim_.of(kHqTrimDb) : 1.f);
		const float midGain = glide(midGain_, midGainDb_.of(s.midGainDb));
		const float sideGain = glide(sideGain_, sideGainDb_.of(s.sideGainDb));
		const float inject = glide(inject_, injectDb_.of(s.sideInjectDb));
		const float outGain = glide(outGain_, outGainDb_.of(s.outputGainDb));
		const float ceiling = glide(ceiling_, ceilingDb_.of(s.ceilingDb));

		// ------------------------------------------------- Stages 0, 1 and 2
		const float li = l * trim;
		const float ri = r * trim;

		const float rawMid = 0.5f * (li + ri);
		float side = a.msEnabled ? (0.5f * (li - ri)) : 0.f;

		if (a.hpfEnabled) {
			hpf_.configure(s.hpfHz, a.hpfSlope, a.hpfMode);
			side = hpf_.process(side);
		}

		// ------------------------------------------------------------ Stage 3
		// Every path is fed every sample, whatever is selected, so switching a
		// bypass or a quality mode never plays out of stale history -- and so
		// that toggling the global enable, which is the comparison the client
		// lives in, is instant.
		float midAligned = 0.f;
		float sideAligned = 0.f;
		{
			const bool shortTaps = (a.quality == QualityShortLinear);
			const size_t firDelay = size_t(HilbertFir::halfLength(shortTaps));

			const float rotFir = fir_.rotatePlus90(side, shortTaps);
			const float dryFir = sideDelay_.process(side, firDelay);
			const float midFir = midDelay_.process(rawMid, firDelay);

			float midI = 0.f, midQ = 0.f, sideI = 0.f, sideQ = 0.f;
			netMid_.process(rawMid, midI, midQ);
			netSide_.process(side, sideI, sideQ);
			(void)midQ;

			if (a.quality == QualityAllpass) {
				// Both branches carry the same allpass phase, so Mid from I and
				// Side from Q are exactly 90 degrees apart without either being
				// linear phase -- and one sample of latency instead of 511.
				midAligned = midI;
				sideAligned = a.rotateEnabled ? sideQ : sideI;
			}
			else {
				midAligned = midFir;
				sideAligned = a.rotateEnabled ? rotFir : dryFir;
			}
		}

		const float mid = midAligned * midGain;
		const float sideOut = sideAligned * sideGain * inject;

		// ------------------------------------------------------------ Stage 4
		float commitL = 0.f, commitR = 0.f;
		if (!a.commitEnabled) {
			// Bypassing the commit leaves the graph in its M/S domain: the
			// rotated Side is put back as a stereo pair rather than summed, so
			// Stages 1-3 can be auditioned without the collapse.
			commitL = mid + sideOut;
			commitR = mid - sideOut;
		}
		else {
			float mono = 0.f;
			switch (a.commitMode) {
				case CommitSum:
					mono = midAligned;  // the reference downmix, no gains applied
					break;

				case CommitMidOnly:
					mono = mid;
					break;

				case CommitSideEnergyFold:
					// Lab: keep the total energy of both components in one
					// channel instead of letting them interfere.
					mono = std::sqrt(mid * mid + sideOut * sideOut);
					if (mid < 0.f)
						mono = -mono;
					break;

				case CommitPolarityMatrix: {
					// Lab: the discrete "(L+R) or (L-R), whichever has the
					// energy" chooser. Crossfaded rather than switched, because
					// a hard swap on a level comparison chatters.
					envMid_ = follow(envMid_, std::abs(mid));
					envSide_ = follow(envSide_, std::abs(sideOut));
					const float want = (envSide_ > envMid_) ? 1.f : 0.f;
					selector_ += selCoeff_ * (want - selector_);
					mono = mid + (sideOut - mid) * selector_;
					break;
				}

				case CommitMidPlusRotatedSide:
				default:
					mono = mid + sideOut;
					break;
			}
			commitL = commitR = mono;
		}

		commitL *= outGain;
		commitR *= outGain;

		// ------------------------------------------------------------ Stage 5
		// Fed every sample so its delay line is never stale, but only listened
		// to when it is switched in.
		float limitedL = 0.f, limitedR = 0.f;
		limiter_.process(commitL, commitR, ceiling, limitedL, limitedR);

		float yL = a.limiterEnabled ? limitedL : commitL;
		float yR = a.limiterEnabled ? limitedR : commitR;

		if (!a.enabled) {
			// Global bypass is a stereo pass-through, undelayed.
			yL = l;
			yR = r;
		}

		// ------------------------------------------------- fade and hand over
		if (fade_ != FadeNone) {
			if (fade_ == FadeOut) {
				fadeGain_ -= fadeStep_;
				if (fadeGain_ <= 0.f) {
					fadeGain_ = 0.f;
					applyStructure(s);  // also sets holdLeft_
					fade_ = FadeHold;
				}
			}
			else if (fade_ == FadeHold) {
				if (holdLeft_ == 0)
					fade_ = FadeIn;
				else
					holdLeft_--;
			}
			else {
				fadeGain_ += fadeStep_;
				if (fadeGain_ >= 1.f) {
					fadeGain_ = 1.f;
					fade_ = FadeNone;
				}
			}
			yL *= fadeGain_;
			yR *= fadeGain_;
		}

		// The stages guard their own state, but a NaN arriving from a
		// misbehaving capture device must not be handed to the endpoint.
		if (!std::isfinite(yL))
			yL = 0.f;
		if (!std::isfinite(yR))
			yR = 0.f;

		outL = yL;
		outR = yR;
	}

private:
	enum FadeState { FadeNone, FadeOut, FadeHold, FadeIn };

	/** The discrete state that cannot be crossfaded away sample by sample.
	    Anything in here triggers the fade; anything not in here is smoothed. */
	static uint64_t signature(const RealMonoSettings& s) {
		uint64_t v = 0;
		v = (v << 1) | (s.enabled ? 1u : 0u);
		v = (v << 1) | (s.hpfEnabled ? 1u : 0u);
		v = (v << 5) | uint64_t(s.hpfSlope & 31);
		v = (v << 2) | uint64_t(s.hpfMode & 3);
		v = (v << 1) | (s.msEnabled ? 1u : 0u);
		v = (v << 1) | (s.rotateEnabled ? 1u : 0u);
		v = (v << 3) | uint64_t(s.quality & 7);
		v = (v << 1) | (s.commitEnabled ? 1u : 0u);
		v = (v << 3) | uint64_t(s.commitMode & 7);
		v = (v << 1) | (s.limiterEnabled ? 1u : 0u);
		// Look-ahead moves the latency, so it is structural even though it is
		// spelled as a continuous control.
		v = (v << 8) | uint64_t(int(s.lookaheadMs * 10.f) & 255);
		return v;
	}

	/*  Applies a structural change at the bottom of the fade, and works out how
	    long the output has to stay down afterwards.

	    Both delay lines in the chain can be holding audio produced under the
	    settings being replaced, and that audio emerges one whole delay later --
	    long after a 3 ms fade has finished, as an audible click at exactly
	    fade + delay. Measured, not assumed: it was 385 samples for Stage 5
	    alone and 896 samples once Stage 1 or 2 was in the mix.

	      Stage 5's look-ahead holds committed audio, so any change needs it
	      flushed -- unless the new settings bypass the limiter or the whole
	      chain, in which case that buffer is not being listened to.

	      Stage 3's history holds *Side* audio, so only a change that alters
	      what feeds the Side path -- Stages 1 and 2 -- makes it stale. A
	      rotation, quality, commit or global-bypass change does not: every
	      path is fed every sample whatever is selected, so its history is
	      already current, and holding for it would mute the A/B toggle the
	      client uses most for four times longer than it needs. */
	void applyStructure(const RealMonoSettings& s) {
		const bool sideInputChanged = applied_.msEnabled != s.msEnabled
		                           || applied_.hpfEnabled != s.hpfEnabled
		                           || applied_.hpfSlope != s.hpfSlope
		                           || applied_.hpfMode != s.hpfMode;

		applied_ = s;
		structure_ = signature(s);
		limiter_.configure(size_t(double(s.lookaheadMs) * 0.001 * sampleRate_));
		if (s.hpfEnabled)
			hpf_.configure(s.hpfHz, s.hpfSlope, s.hpfMode);
		else
			hpf_.clear();

		holdLeft_ = 0;
		if (s.enabled) {
			if (sideInputChanged)
				holdLeft_ += size_t(rotationDelay(s.quality));
			if (s.limiterEnabled)
				holdLeft_ += limiter_.latency();
		}
	}

	void primeSmoothing(const RealMonoSettings& s) {
		// Jump rather than glide the first time, so opening the app on a
		// non-default preset does not audibly sweep into position.
		inTrim_ = s.hqMode ? dbToGain(kHqTrimDb) : 1.f;
		midGain_ = dbToGain(s.midGainDb);
		sideGain_ = dbToGain(s.sideGainDb);
		inject_ = dbToGain(s.sideInjectDb);
		outGain_ = dbToGain(s.outputGainDb);
		ceiling_ = dbToGain(s.ceilingDb);
		limiter_.configure(size_t(double(s.lookaheadMs) * 0.001 * sampleRate_));
		if (s.hpfEnabled)
			hpf_.configure(s.hpfHz, s.hpfSlope, s.hpfMode);
	}

	float glide(float& state, float target) {
		state += smoothCoeff_ * (target - state);
		return state;
	}

	float follow(float state, float x) const {
		const float c = (x > state) ? envAttack_ : envRelease_;
		return state + c * (x - state);
	}

	HilbertFir fir_;
	QuadratureNetwork netMid_;
	QuadratureNetwork netSide_;
	DelayLine midDelay_;
	DelayLine sideDelay_;
	HighpassChain hpf_;
	LookaheadLimiter limiter_;

	RealMonoSettings applied_;
	uint64_t structure_ = 0;
	bool primed_ = false;

	double sampleRate_ = 48000.0;
	float smoothCoeff_ = 0.01f;
	float fadeStep_ = 0.01f;
	FadeState fade_ = FadeNone;
	float fadeGain_ = 1.f;
	size_t holdLeft_ = 0;

	DbGain hqTrim_, midGainDb_, sideGainDb_, injectDb_, outGainDb_, ceilingDb_;
	float inTrim_ = 1.f, midGain_ = 1.f, sideGain_ = 1.f;
	float inject_ = 1.f, outGain_ = 1.f, ceiling_ = 1.f;

	float envAttack_ = 0.f, envRelease_ = 0.f, selCoeff_ = 0.f;
	float envMid_ = 0.f, envSide_ = 0.f, selector_ = 0.f;
};

} // namespace fmdr
