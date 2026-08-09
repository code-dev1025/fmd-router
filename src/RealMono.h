#pragma once

/*  DSP building blocks for the Real Mono chain.

    The brief asks for RS-MET (rapt / rosic) for the Hilbert transformer, the
    FIR designer and the limiter. No RS-MET checkout is present in this
    workspace, so these are the noted fallback: published, small-footprint
    designs written to the same interfaces, so swapping in
    RAPT::HilbertFilter / QuadratureNetwork / rosic::Limiter later is a
    substitution inside RealMonoChain and nothing else.

      HilbertFir          <-> rapt FIR designer + convolver  (windowed type-III)
      QuadratureNetwork   <-> RAPT::QuadratureNetwork        (allpass pair)
      Biquad / HighpassChain <-> rosic cookbook biquads      (RBJ)
      LookaheadLimiter    <-> rosic::Limiter                 (windowed-max peak)

    Everything here is real-time safe once prepare() has run: no allocation, no
    locks, no logging, and no unbounded loops. */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fmdr {

constexpr double kPiD = 3.14159265358979323846;

inline float dbToGain(float db) {
	return std::pow(10.f, db * (1.f / 20.f));
}

inline float gainToDb(float gain) {
	return (gain <= 1e-9f) ? -180.f : 20.f * std::log10(gain);
}

/** Denormals cost hundreds of cycles on some CPUs and only ever arrive as the
    tail of a decaying filter state, so they are flushed where state is stored
    rather than being left to the FPU's mercy. */
inline float flushDenormal(float v) {
	return (std::abs(v) < 1e-25f) ? 0.f : v;
}


/** A dB value cached against its linear gain, so the pow() happens when the
    control moves rather than once per sample. */
struct DbGain {
	float db = 0.f;
	float gain = 1.f;

	float of(float newDb) {
		if (newDb != db) {
			db = newDb;
			gain = dbToGain(newDb);
		}
		return gain;
	}
};


/*  Power-of-two delay line with a caller-chosen tap.

    The tap is passed per call rather than stored because the Mid path's delay
    has to track whichever Hilbert length Stage 3 is using, and a delay line
    that can be re-tapped without reallocating is what makes switching quality
    modes a parameter change instead of a re-prepare. */
class DelayLine {
public:
	void prepare(size_t maxDelay) {
		size_t cap = 1;
		while (cap < maxDelay + 1)
			cap <<= 1;
		buf_.assign(cap, 0.f);
		mask_ = cap - 1;
		pos_ = 0;
	}

	void clear() {
		std::fill(buf_.begin(), buf_.end(), 0.f);
		pos_ = 0;
	}

	/** Writes `x`, returns the sample written `delay` calls ago. delay == 0
	    returns `x` itself. */
	float process(float x, size_t delay) {
		buf_[pos_] = x;
		const float y = buf_[(pos_ - delay) & mask_];
		pos_ = (pos_ + 1) & mask_;
		return y;
	}

private:
	std::vector<float> buf_;
	size_t mask_ = 0;
	size_t pos_ = 0;
};


/*  Linear-phase FIR Hilbert transformer -- the PHA-979 reference behaviour.

    Type III (odd length, antisymmetric): h[m] = 2/(pi*m) for odd m, zero for
    even m, windowed with Blackman. Every frequency is shifted by the same 90
    degrees, which is the property the client's process depends on; the price
    is a group delay of exactly half the tap count, and a roll-off at both ends
    of the spectrum whose width is set by the tap count.

    Two lengths are designed up front -- they are sample-rate independent, so
    this is a one-off cost at prepare() and switching between them at run time
    touches nothing but a pointer:

      HQ    1023 taps, 511 samples of delay, flat from roughly 300 Hz up
      short  255 taps, 127 samples of delay, flat from roughly 1.2 kHz up

    Half the taps are zero and the rest are antisymmetric, so a 1023-tap
    convolution costs 256 multiply-adds, not 1023.

    Sign: the antisymmetric pair is taken newer-minus-older, which is the
    negative of the textbook Hilbert transform -- so this is +90 degrees
    directly, the rotation the brief asks for on the Side bus, and no negation
    is applied downstream. tests/TestRealMono.cpp measures the shift rather
    than trusting the derivation, because getting this backwards would flip the
    polarity of every recovered Side signal and still sound plausible. */
class HilbertFir {
public:
	static constexpr int kHqHalf = 511;     // 1023 taps
	static constexpr int kShortHalf = 127;  // 255 taps

	void prepare() {
		design(hq_, kHqHalf);
		design(shortTaps_, kShortHalf);
		size_t cap = 1;
		while (cap < size_t(2 * kHqHalf + 1))
			cap <<= 1;
		buf_.assign(cap, 0.f);
		mask_ = cap - 1;
		pos_ = 0;
	}

	void clear() {
		std::fill(buf_.begin(), buf_.end(), 0.f);
		pos_ = 0;
	}

	static int halfLength(bool shortTaps) {
		return shortTaps ? kShortHalf : kHqHalf;
	}

	/** x rotated by +90 degrees, delayed by halfLength(shortTaps) samples. Must
	    be called once per sample whichever tap set is selected, or the history
	    has holes in it. */
	float rotatePlus90(float x, bool shortTaps) {
		buf_[pos_] = x;

		const int half = shortTaps ? kShortHalf : kHqHalf;
		const float* c = shortTaps ? shortTaps_.data() : hq_.data();

		// y = sum over odd m of h[m] * (x[centre + m] - x[centre - m]), where
		// the centre sits `half` samples back and x[centre + m] -- the newer
		// of the pair -- is at delay half - m.
		float y = 0.f;
		for (int m = 1; m <= half; m += 2) {
			const float newer = buf_[(pos_ - size_t(half - m)) & mask_];
			const float older = buf_[(pos_ - size_t(half + m)) & mask_];
			y += c[size_t(m)] * (newer - older);
		}

		pos_ = (pos_ + 1) & mask_;
		return y;
	}

private:
	static void design(std::vector<float>& taps, int half) {
		taps.assign(size_t(half) + 1, 0.f);
		for (int m = 1; m <= half; m += 2) {
			const double window = 0.42
			                    + 0.5 * std::cos(kPiD * m / half)
			                    + 0.08 * std::cos(2.0 * kPiD * m / half);
			taps[size_t(m)] = float(2.0 / (kPiD * m) * window);
		}
	}

	std::vector<float> hq_;
	std::vector<float> shortTaps_;
	std::vector<float> buf_;
	size_t mask_ = 0;
	size_t pos_ = 0;
};


/** Second-order allpass, H(z) = (c - z^-2) / (1 - c*z^-2). The building block
    of the quadrature network below. */
class AllpassSection {
public:
	void setCoeff(float c) {
		c_ = c;
	}

	void clear() {
		x1_ = x2_ = y1_ = y2_ = 0.f;
	}

	float process(float x) {
		const float y = c_ * (x + y2_) - x2_;
		x2_ = x1_;
		x1_ = x;
		y2_ = y1_;
		y1_ = flushDenormal(y);
		return y;
	}

private:
	float c_ = 0.f;
	float x1_ = 0.f, x2_ = 0.f, y1_ = 0.f, y2_ = 0.f;
};


/*  IIR quadrature network -- the low-latency alternative to the FIR.

    Two four-section allpass chains whose outputs stay 90 degrees apart across
    the audio band (Olli Niemitalo's coefficients, the design RS-MET's
    QuadratureNetwork also uses). One extra sample of delay on the I branch
    aligns the pair.

    The trick that makes this usable here: both outputs carry the *same*
    allpass phase distortion, so as long as the Mid path is taken from the I
    output of its own network and the Side path from the Q output of its own,
    the 90 degree relationship between them is exact even though neither is
    linear phase. Latency is one sample instead of 511.

    Nonlinear phase means transients smear slightly compared with the FIR,
    which is why this is offered as the low-latency option and not the
    default. */
class QuadratureNetwork {
public:
	void prepare() {
		// Published pole radii; the sections are written in terms of the
		// square, so they are squared here rather than in the table. Getting
		// this wrong still produces a plausible-looking 90-ish degrees -- it
		// costs 13 degrees at the band edges instead of 1.6 -- so the error is
		// measured in tests/TestRealMono.cpp rather than eyeballed.
		static const double kI[4] = {0.6923877778065, 0.9360654322959,
		                             0.9882295226860, 0.9987488452737};
		static const double kQ[4] = {0.4021921162426, 0.8561710882420,
		                             0.9722909545651, 0.9952884791278};
		for (int i = 0; i < 4; i++) {
			iChain_[i].setCoeff(float(kI[i] * kI[i]));
			qChain_[i].setCoeff(float(kQ[i] * kQ[i]));
		}
		clear();
	}

	void clear() {
		for (int i = 0; i < 4; i++) {
			iChain_[i].clear();
			qChain_[i].clear();
		}
		delayed_ = 0.f;
	}

	/** outI is the reference phase; outQ leads it by 90 degrees -- the same
	    direction of rotation HilbertFir::rotatePlus90 produces, so the two
	    quality modes agree on polarity. */
	void process(float x, float& outI, float& outQ) {
		float i = x;
		for (int k = 0; k < 4; k++)
			i = iChain_[k].process(i);

		float q = x;
		for (int k = 0; k < 4; k++)
			q = qChain_[k].process(q);

		// The I chain is one sample ahead of the Q chain by construction.
		outI = delayed_;
		delayed_ = i;
		outQ = q;
	}

private:
	AllpassSection iChain_[4];
	AllpassSection qChain_[4];
	float delayed_ = 0.f;
};


/** RBJ cookbook biquad, direct form I. Only the high-pass is needed here. */
class Biquad {
public:
	void setHighpass(double fc, double q, double sampleRate) {
		fc = std::max(1.0, std::min(fc, sampleRate * 0.45));
		const double w = 2.0 * kPiD * fc / sampleRate;
		const double cw = std::cos(w);
		const double sw = std::sin(w);
		const double alpha = sw / (2.0 * q);

		const double b0 = (1.0 + cw) * 0.5;
		const double b1 = -(1.0 + cw);
		const double b2 = (1.0 + cw) * 0.5;
		const double a0 = 1.0 + alpha;
		const double a1 = -2.0 * cw;
		const double a2 = 1.0 - alpha;

		b0_ = float(b0 / a0);
		b1_ = float(b1 / a0);
		b2_ = float(b2 / a0);
		a1_ = float(a1 / a0);
		a2_ = float(a2 / a0);
	}

	void clear() {
		x1_ = x2_ = y1_ = y2_ = 0.f;
	}

	float process(float x) {
		const float y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
		x2_ = x1_;
		x1_ = x;
		y2_ = y1_;
		y1_ = flushDenormal(y);
		return y;
	}

private:
	float b0_ = 1.f, b1_ = 0.f, b2_ = 0.f, a1_ = 0.f, a2_ = 0.f;
	float x1_ = 0.f, x2_ = 0.f, y1_ = 0.f, y2_ = 0.f;
};


/** One-pole high-pass, for the 6 dB/oct setting. */
class OnePoleHighpass {
public:
	void set(double fc, double sampleRate) {
		fc = std::max(1.0, std::min(fc, sampleRate * 0.45));
		coeff_ = float(std::exp(-2.0 * kPiD * fc / sampleRate));
	}

	void clear() {
		lp_ = 0.f;
	}

	float process(float x) {
		lp_ = flushDenormal(lp_ + (1.f - coeff_) * (x - lp_));
		return x - lp_;
	}

private:
	float coeff_ = 0.f;
	float lp_ = 0.f;
};


/*  Stage 1's high-pass: 6, 12 or 24 dB/oct, in two alignments.

    Both alignments high-pass the Side path and nothing else, because on a mono
    output that is the only choice that means anything -- "make the bass mono"
    and "high-pass the Side" are the same statement once the output has one
    channel. What the mode actually selects is the shape:

      side_hpf        Butterworth -- maximally flat, -3 dB at fc
      crossover_mono  Linkwitz-Riley -- -6 dB at fc, the half of a crossover
                      whose complement is the Mid-only band below it

    The client does not use this stage at all; it exists for the LF
    double-counting experiments the brief wants kept available. */
class HighpassChain {
public:
	enum Mode {
		ModeSideHpf = 0,
		ModeCrossoverMono = 1,
	};

	void prepare(double sampleRate) {
		sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
		fc_ = 0.f;  // forces a design on the first process()
		clear();
	}

	void clear() {
		one_.clear();
		one2_.clear();
		bq1_.clear();
		bq2_.clear();
	}

	void configure(float fcHz, int slope, int mode) {
		if (fcHz == fc_ && slope == slope_ && mode == mode_)
			return;
		fc_ = fcHz;
		slope_ = slope;
		mode_ = mode;

		const double fc = double(fcHz);
		const bool lr = (mode == ModeCrossoverMono);

		one_.set(fc, sampleRate_);
		one2_.set(fc, sampleRate_);

		if (slope_ >= 24) {
			if (lr) {
				// LR4 = two cascaded Butterworth-Q sections.
				bq1_.setHighpass(fc, 0.70710678, sampleRate_);
				bq2_.setHighpass(fc, 0.70710678, sampleRate_);
			}
			else {
				// 4th-order Butterworth.
				bq1_.setHighpass(fc, 0.54119610, sampleRate_);
				bq2_.setHighpass(fc, 1.30656296, sampleRate_);
			}
		}
		else if (slope_ >= 12 && !lr) {
			bq1_.setHighpass(fc, 0.70710678, sampleRate_);
		}
		// slope 12 in LR mode is two cascaded one-poles; slope 6 is one.
	}

	float process(float x) {
		if (slope_ >= 24)
			return bq2_.process(bq1_.process(x));
		if (slope_ >= 12)
			return (mode_ == ModeCrossoverMono) ? one2_.process(one_.process(x))
			                                    : bq1_.process(x);
		return one_.process(x);
	}

private:
	double sampleRate_ = 48000.0;
	float fc_ = 0.f;
	int slope_ = 12;
	int mode_ = ModeSideHpf;

	OnePoleHighpass one_;
	OnePoleHighpass one2_;
	Biquad bq1_;
	Biquad bq2_;
};


/*  Look-ahead brickwall limiter, stereo-linked.

    Mid plus a phase-rotated Side is not the same waveform as either input
    channel -- same magnitude spectrum, different crest factor -- so the sum
    overshoots the original peak, by about 3 dB on typical programme. That
    overshoot is what this exists to catch.

    The peak detector is an exact sliding maximum over the look-ahead window,
    kept by a monotonic deque, so the gain has started coming down before the
    peak it is reducing has arrived. Attack is a one-pole with a time constant
    of a fifth of the look-ahead, which converges to within 0.06 dB inside the
    window; a final clamp at the ceiling makes "brickwall" true rather than
    nearly true. Release is 100 ms, slow enough not to pump on bass.

    Latency is exactly the look-ahead. */
class LookaheadLimiter {
public:
	void prepare(double sampleRate, double maxLookaheadMs) {
		sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
		const size_t maxSamples = size_t(maxLookaheadMs * 0.001 * sampleRate_) + 2;

		size_t cap = 1;
		while (cap < maxSamples + 1)
			cap <<= 1;
		qVal_.assign(cap, 0.f);
		qIdx_.assign(cap, 0);
		qMask_ = cap - 1;

		delayL_.prepare(maxSamples);
		delayR_.prepare(maxSamples);

		releaseCoeff_ = float(1.0 - std::exp(-1.0 / (0.100 * sampleRate_)));
		configure(size_t(0.005 * sampleRate_));
		clear();
	}

	/** Look-ahead in samples. Anything longer than prepare() was told about is
	    clamped, so a bad parameter cannot walk off the buffers. */
	void configure(size_t lookaheadSamples) {
		const size_t maxSamples = qMask_;  // capacity - 1
		lookahead_ = std::min(lookaheadSamples, maxSamples);
		// A fifth of the window, so the gain is within 0.7% of target by the
		// time the peak reaches the output.
		const double tau = std::max(1.0, double(lookahead_) / 5.0);
		attackCoeff_ = float(1.0 - std::exp(-1.0 / tau));
	}

	void clear() {
		delayL_.clear();
		delayR_.clear();
		head_ = tail_ = count_ = 0;
		now_ = 0;
		gain_ = 1.f;
	}

	size_t latency() const {
		return lookahead_;
	}

	float gain() const {
		return gain_;
	}

	void process(float l, float r, float ceiling, float& outL, float& outR) {
		const float peak = pushPeak(std::max(std::abs(l), std::abs(r)));

		const float target = (peak > ceiling) ? (ceiling / peak) : 1.f;
		gain_ += ((target < gain_) ? attackCoeff_ : releaseCoeff_) * (target - gain_);

		const float dl = delayL_.process(l, lookahead_);
		const float dr = delayR_.process(r, lookahead_);

		outL = std::max(-ceiling, std::min(ceiling, dl * gain_));
		outR = std::max(-ceiling, std::min(ceiling, dr * gain_));
	}

private:
	/** Sliding maximum over the last lookahead + 1 samples. The deque holds
	    only samples that are still candidates for the maximum, so it is O(1)
	    amortised and never allocates. */
	float pushPeak(float v) {
		while (count_ > 0 && qVal_[(tail_ - 1) & qMask_] <= v) {
			tail_--;
			count_--;
		}
		qVal_[tail_ & qMask_] = v;
		qIdx_[tail_ & qMask_] = now_;
		tail_++;
		count_++;

		while (count_ > 0 && qIdx_[head_ & qMask_] + lookahead_ < now_) {
			head_++;
			count_--;
		}

		now_++;
		return qVal_[head_ & qMask_];
	}

	double sampleRate_ = 48000.0;
	size_t lookahead_ = 0;
	float attackCoeff_ = 0.01f;
	float releaseCoeff_ = 0.0002f;
	float gain_ = 1.f;

	DelayLine delayL_;
	DelayLine delayR_;

	std::vector<float> qVal_;
	std::vector<size_t> qIdx_;
	size_t qMask_ = 0;
	size_t head_ = 0, tail_ = 0, count_ = 0;
	size_t now_ = 0;
};

} // namespace fmdr
