#pragma once

#include "Ring.h"

#include <cmath>

namespace fmdr {

/*  Fractional-rate reader for the capture ring.

    Two jobs, and they are the same job. The capture and render endpoints are
    different devices with different crystals, so even when both claim 48 kHz
    they are not the same 48 kHz -- a hundred parts per million of drift fills
    or empties the ring over a few minutes and you get a click. And the two may
    genuinely disagree on rate (VB-CABLE at 44.1 kHz into a 48 kHz card).

    Both are handled by reading the ring at a ratio that is nudged by how full
    the ring is: base ratio for the nominal rate difference, and a slow
    correction of at most +/-0.5% -- about 8 cents, well under what anyone
    hears -- to hold the fill at its target.

    Interpolation is the 4-point 3rd-order Hermite from Laurent de Soras'
    resampling paper. It needs one sample behind and two ahead, which is why
    the phase starts at 4: the first call pops four frames to load the history
    before it interpolates anything. */
class Resampler {
public:
	void reset() {
		phase_ = 4.0;
		for (int i = 0; i < 4; i++)
			hl_[i] = hr_[i] = 0.f;
	}

	void setRatio(double ratio) {
		ratio_ = ratio;
	}

	double ratio() const {
		return ratio_;
	}

	/** Produces one output frame. Returns false if the ring ran dry, in which
	    case nothing is written and the phase is left where it stopped, so the
	    next call resumes mid-interpolation rather than glitching the history. */
	bool next(StereoRing& ring, float& outL, float& outR) {
		while (phase_ >= 1.0) {
			float l, r;
			if (!ring.popFrame(l, r))
				return false;
			hl_[0] = hl_[1]; hl_[1] = hl_[2]; hl_[2] = hl_[3]; hl_[3] = l;
			hr_[0] = hr_[1]; hr_[1] = hr_[2]; hr_[2] = hr_[3]; hr_[3] = r;
			phase_ -= 1.0;
		}
		const float t = float(phase_);
		outL = hermite(hl_[0], hl_[1], hl_[2], hl_[3], t);
		outR = hermite(hr_[0], hr_[1], hr_[2], hr_[3], t);
		phase_ += ratio_;
		return true;
	}

private:
	/** 4-point, 3rd-order Hermite. `t` is the position in [0,1) between x0
	    and x1; xm1 and x2 only supply the tangents. */
	static float hermite(float xm1, float x0, float x1, float x2, float t) {
		const float c = (x1 - xm1) * 0.5f;
		const float v = x0 - x1;
		const float w = c + v;
		const float a = w + v + (x2 - x0) * 0.5f;
		const float b = w + a;
		return ((a * t - b) * t + c) * t + x0;
	}

	double phase_ = 4.0;
	double ratio_ = 1.0;
	float hl_[4] = {0.f, 0.f, 0.f, 0.f};
	float hr_[4] = {0.f, 0.f, 0.f, 0.f};
};


/*  Proportional controller that holds the ring at `target` frames by trimming
    the resampler ratio.

    The fill measurement is smoothed hard before it is used. Raw fill jumps by
    a whole packet every time the capture thread wakes, and reacting to that
    would modulate pitch at the packet rate, which is very audible. One pole at
    roughly 1 Hz turns it into the slow trend we actually want to correct.

    The clamp matters as much as the gain: it bounds the worst case to an
    inaudible pitch offset, so a controller that is wrong -- during a device
    glitch, say -- degrades to slightly-off-speed rather than to a chirp. */
class DriftController {
public:
	void reset(double baseRatio, double targetFrames, double blockRateHz) {
		base_ = baseRatio;
		target_ = targetFrames;
		smoothed_ = targetFrames;
		// One-pole coefficient for a ~1 Hz corner at the rate this is ticked.
		const double fc = 1.0;
		coeff_ = 1.0 - std::exp(-2.0 * 3.14159265358979323846 * fc / (blockRateHz > 1.0 ? blockRateHz : 1.0));
	}

	/** Tick once per render block with the current fill; returns the ratio. */
	double update(double fillFrames) {
		smoothed_ += coeff_ * (fillFrames - smoothed_);
		const double err = (smoothed_ - target_) / (target_ > 1.0 ? target_ : 1.0);
		double trim = kP_ * err;
		if (trim > kMaxTrim) trim = kMaxTrim;
		if (trim < -kMaxTrim) trim = -kMaxTrim;
		return base_ * (1.0 + trim);
	}

	double smoothedFill() const {
		return smoothed_;
	}

private:
	static constexpr double kP_ = 0.02;
	static constexpr double kMaxTrim = 0.005;  // +/-0.5%, about 8 cents

	double base_ = 1.0;
	double target_ = 0.0;
	double smoothed_ = 0.0;
	double coeff_ = 0.01;
};

} // namespace fmdr
