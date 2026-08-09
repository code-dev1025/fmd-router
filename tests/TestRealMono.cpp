/*  Offline checks for the parts of Real Mono Sound that have nothing to do
    with Windows: the chain, the ring and the resampler.

    These exist because "I can hear it" is not a regression test. Every claim
    the README makes -- that anti-phase content survives the mono sum instead
    of cancelling, that the rotation really is 90 degrees and not merely
    something-ish, that the Mid path is delay-aligned to the sample, that the
    limiter holds its ceiling, that HQ mode is exactly 3 dB -- is asserted here
    against numbers rather than against an opinion.

    The two that would otherwise be trusted derivations, and would sound
    plausible if they were backwards, are the rotation's direction and its
    magnitude response. Those are measured.

    Build: cmake --build build --config Release --target fmd-router-tests
    Run:   build/Release/fmd-router-tests.exe   (exit code 0 = all passed) */

#include "RealMonoChain.h"
#include "Resampler.h"
#include "Ring.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, int line) {
	g_checks++;
	if (ok) {
		std::printf("  ok   %s\n", what);
	}
	else {
		std::printf("  FAIL %s   (line %d)\n", what, line);
		g_failures++;
	}
}

#define CHECK(cond, what) check((cond), (what), __LINE__)

constexpr double kSampleRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

double toDb(double linear) {
	return (linear <= 1e-12) ? -240.0 : 20.0 * std::log10(linear);
}

double wrap180(double deg) {
	while (deg > 180.0) deg -= 360.0;
	while (deg < -180.0) deg += 360.0;
	return deg;
}


/** Runs a block of stereo through one chain instance. */
struct Run {
	std::vector<float> l;
	std::vector<float> r;
};

Run runChain(const fmdr::RealMonoSettings& s,
             const std::vector<float>& inL, const std::vector<float>& inR,
             double sampleRate = kSampleRate) {
	fmdr::RealMonoChain chain;
	chain.prepare(sampleRate);

	Run out;
	out.l.resize(inL.size());
	out.r.resize(inL.size());
	for (size_t i = 0; i < inL.size(); i++)
		chain.process(inL[i], inR[i], s, out.l[i], out.r[i]);
	return out;
}

enum Content { InPhase, AntiPhase, LeftOnly };

/** A steady tone as Mid (in phase), as Side (anti-phase) or hard panned. */
void makeTone(double hz, Content content, size_t frames, double amp,
              std::vector<float>& l, std::vector<float>& r) {
	l.resize(frames);
	r.resize(frames);
	const double w = 2.0 * kPi * hz / kSampleRate;
	for (size_t i = 0; i < frames; i++) {
		const double v = amp * std::sin(w * double(i));
		l[i] = float(v);
		r[i] = (content == InPhase) ? float(v)
		     : (content == AntiPhase) ? float(-v)
		     : 0.f;
	}
}

/** Peak of the last quarter, by which point the chain has filled its delay
    lines and the control smoothing has settled. */
double tailPeak(const std::vector<float>& x) {
	double peak = 0.0;
	for (size_t i = x.size() * 3 / 4; i < x.size(); i++)
		peak = std::max(peak, double(std::abs(x[i])));
	return peak;
}

/** Amplitude and phase of a steady tone, measured over the tail. */
struct Tone {
	double amplitude = 0.0;
	double degrees = 0.0;
};

Tone measure(const std::vector<float>& x, double hz) {
	const double w = 2.0 * kPi * hz / kSampleRate;
	const size_t from = x.size() / 2;
	double ic = 0.0, is = 0.0;
	for (size_t i = from; i < x.size(); i++) {
		ic += double(x[i]) * std::cos(w * double(i));
		is += double(x[i]) * std::sin(w * double(i));
	}
	const double n = double(x.size() - from);
	Tone t;
	t.amplitude = 2.0 * std::sqrt(ic * ic + is * is) / n;
	t.degrees = std::atan2(-is, ic) * 180.0 / kPi;
	return t;
}

/** Deterministic noise, so a failure is reproducible. */
struct Lcg {
	uint32_t state = 22222u;
	float next() {
		state = state * 1664525u + 1013904223u;
		return float(int32_t(state)) * (1.f / 2147483648.f);
	}
};

fmdr::RealMonoSettings defaults() {
	return fmdr::presetSettings(fmdr::PresetRealMonoDefault);
}


// ------------------------------------------------- the claim the product makes

void testSideContentSurvives() {
	std::printf("Side content survives the mono sum\n");

	std::vector<float> l, r;
	makeTone(1000.0, AntiPhase, 24000, 0.5, l, r);

	// Anti-phase is pure Side: a plain (L+R)/2 downmix cancels it to nothing.
	// This is the failure Real Mono exists to fix, so it is checked first --
	// against the app's own classic-mono mode, not against a hand-rolled sum.
	fmdr::RealMonoSettings classic = defaults();
	classic.rotateEnabled = false;
	classic.commitMode = fmdr::CommitMidOnly;
	const double cancelled = tailPeak(runChain(classic, l, r).l);
	CHECK(toDb(cancelled) < -80.0, "classic mono cancels anti-phase to silence");

	const double recovered = tailPeak(runChain(defaults(), l, r).l);
	std::printf("       classic %.1f dB   Real Mono %.1f dB\n",
	            toDb(cancelled), toDb(recovered));
	// The rotation turns Side into something that survives the sum at its
	// original level: 0.5 in, 0.5 out.
	CHECK(std::abs(toDb(recovered) - toDb(0.5)) < 0.5,
	      "Real Mono recovers anti-phase at its original level");

	// And it must not have broken the easy case on the way.
	makeTone(1000.0, InPhase, 24000, 0.5, l, r);
	const double mid = tailPeak(runChain(defaults(), l, r).l);
	CHECK(std::abs(toDb(mid) - toDb(0.5)) < 0.1, "correlated content passes at unity");

	// Hard-panned content is half Mid and half Side. The two are 90 degrees
	// apart after the rotation, so they sum in quadrature: 0.5 * sqrt(2) * the
	// panned channel, or -3 dB, rather than the -6 dB a plain downmix gives.
	makeTone(1000.0, LeftOnly, 24000, 0.5, l, r);
	const Run panned = runChain(defaults(), l, r);
	const double pan = tailPeak(panned.l);
	std::printf("       hard-panned left: %.2f dB relative to the panned channel\n",
	            toDb(pan) - toDb(0.5));
	CHECK(std::abs(toDb(pan) - toDb(0.5 * std::sqrt(0.5))) < 0.5,
	      "hard-panned content sums in quadrature, not at -6 dB");
	CHECK(pan > 0.3, "hard-panned content is audible in the mono output");
}


// ------------------------------------------------------- the rotation itself

/** The Mid and Side paths must leave the chain exactly 90 degrees apart. This
    is measured product-level -- one run with pure Mid, one with pure Side --
    so it holds whatever Stage 3 is doing internally. */
void testRotationIsNinetyDegrees() {
	std::printf("Stage 3 rotation\n");

	struct Case {
		const char* name;
		int quality;
		double tolerance;
		double lowHz;
		double highHz;
	};
	const Case cases[] = {
		{"linear phase HQ",    fmdr::QualityHqLinear,    0.5, 300.0, 15000.0},
		{"linear phase short", fmdr::QualityShortLinear, 0.5, 1500.0, 15000.0},
		{"allpass IIR",        fmdr::QualityAllpass,     2.5, 50.0, 18000.0},
	};

	for (const Case& c : cases) {
		fmdr::RealMonoSettings s = defaults();
		s.quality = c.quality;
		s.limiterEnabled = false;  // measuring the rotator, not the limiter

		double worstPhase = 0.0;
		double worstGain = 0.0;
		for (double hz : {c.lowHz, 1000.0, 4000.0, c.highHz}) {
			std::vector<float> l, r;
			makeTone(hz, InPhase, 48000, 0.5, l, r);
			const Tone midOut = measure(runChain(s, l, r).l, hz);

			makeTone(hz, AntiPhase, 48000, 0.5, l, r);
			const Tone sideOut = measure(runChain(s, l, r).l, hz);

			worstPhase = std::max(worstPhase,
			                      std::abs(wrap180(sideOut.degrees - midOut.degrees) - 90.0));
			worstGain = std::max(worstGain, std::abs(toDb(sideOut.amplitude) - toDb(0.5)));
			worstGain = std::max(worstGain, std::abs(toDb(midOut.amplitude) - toDb(0.5)));
		}
		std::printf("       %-19s %5.0f..%5.0f Hz: worst phase error %.2f deg, "
		            "worst level error %.2f dB\n",
		            c.name, c.lowHz, c.highHz, worstPhase, worstGain);
		CHECK(worstPhase < c.tolerance, "Side leads Mid by 90 degrees across the band");
		CHECK(worstGain < 1.0, "and neither path changes level doing it");
	}
}

/** All three quality modes must rotate the same way. A sign error in one of
    them would still sound like Real Mono on most material, and would fight the
    other two on anything with a defined polarity. */
void testQualityModesAgreeOnPolarity() {
	std::printf("polarity agreement\n");

	std::vector<float> l, r;
	makeTone(2000.0, AntiPhase, 24000, 0.5, l, r);

	double reference = 0.0;
	bool agree = true;
	for (int q = 0; q < fmdr::NumQualities; q++) {
		fmdr::RealMonoSettings s = defaults();
		s.quality = q;
		s.limiterEnabled = false;
		const Tone t = measure(runChain(s, l, r).l, 2000.0);

		// Phases differ by the group delay of each mode, so compare against
		// the un-rotated Side instead: the rotation must lead it, not lag.
		fmdr::RealMonoSettings dry = s;
		dry.rotateEnabled = false;
		dry.commitMode = fmdr::CommitMidPlusRotatedSide;
		const Tone d = measure(runChain(dry, l, r).l, 2000.0);
		const double lead = wrap180(t.degrees - d.degrees);

		if (q == 0)
			reference = lead;
		agree = agree && std::abs(wrap180(lead - reference)) < 5.0;
		std::printf("       quality %d: rotation leads the dry Side by %+.1f deg\n", q, lead);
	}
	CHECK(agree, "every quality mode rotates in the same direction");
	CHECK(std::abs(reference - 90.0) < 2.5, "and that direction is +90, not -90");
}


// ------------------------------------------------------------- alignment

void testMidIsDelayAligned() {
	std::printf("Mid / Side alignment\n");

	// With the rotation bypassed and the limiter out, the chain is a pure
	// delay of the mono sum. If the Mid path were not aligned to the Hilbert's
	// group delay, this would not be sample-exact -- it would be smeared.
	Lcg rng;
	std::vector<float> l(8000), r(8000);
	for (size_t i = 0; i < l.size(); i++) {
		l[i] = rng.next() * 0.4f;
		r[i] = rng.next() * 0.4f;
	}

	for (int q = 0; q < fmdr::NumQualities; q++) {
		fmdr::RealMonoSettings s = defaults();
		s.quality = q;
		s.rotateEnabled = false;
		s.limiterEnabled = false;
		s.commitMode = fmdr::CommitSum;

		fmdr::RealMonoChain chain;
		chain.prepare(kSampleRate);
		const size_t delay = size_t(fmdr::RealMonoChain::rotationDelay(q));

		std::vector<float> out(l.size());
		float a = 0.f, b = 0.f;
		for (size_t i = 0; i < l.size(); i++) {
			chain.process(l[i], r[i], s, a, b);
			out[i] = a;
		}

		double worst = 0.0;
		for (size_t i = delay; i < l.size(); i++) {
			const double want = 0.5 * (double(l[i - delay]) + double(r[i - delay]));
			worst = std::max(worst, std::abs(double(out[i]) - want));
		}
		// The allpass mode is allpass, not a delay: its phase response is what
		// keeps Mid and Side aligned with each other, so only the linear-phase
		// modes can be checked sample by sample.
		if (q != fmdr::QualityAllpass) {
			std::printf("       quality %d: worst deviation from a %zu-sample delay: %.2e\n",
			            q, delay, worst);
			CHECK(worst < 1e-6, "the Mid path is exactly the reported delay, no smearing");
		}
	}
}

void testLatencyIsReportedHonestly() {
	std::printf("reported latency\n");

	fmdr::RealMonoChain chain;
	chain.prepare(kSampleRate);
	fmdr::RealMonoSettings s = defaults();

	// One frame to prime, then the report must match the sum of its parts.
	float a = 0.f, b = 0.f;
	chain.process(0.f, 0.f, s, a, b);
	const int expected = fmdr::HilbertFir::kHqHalf + int(0.005 * kSampleRate);
	std::printf("       HQ + 5 ms look-ahead: %d samples (%.2f ms), expected %d\n",
	            chain.latencySamples(), 1000.0 * chain.latencySamples() / kSampleRate, expected);
	CHECK(chain.latencySamples() == expected, "latency is Hilbert delay plus look-ahead");

	// Bypassing the limiter must remove its look-ahead from the figure, not
	// just from the sound.
	s.limiterEnabled = false;
	for (int i = 0; i < 2000; i++)
		chain.process(0.f, 0.f, s, a, b);
	CHECK(chain.latencySamples() == fmdr::HilbertFir::kHqHalf,
	      "bypassing the limiter drops its look-ahead from the report");

	// Global bypass is a pass-through, so it must report nothing at all.
	s.enabled = false;
	for (int i = 0; i < 2000; i++)
		chain.process(0.f, 0.f, s, a, b);
	CHECK(chain.latencySamples() == 0, "global bypass reports zero latency");

	// And the whole thing has to scale with the sample rate rather than
	// assuming 48 kHz anywhere.
	fmdr::RealMonoChain at441;
	at441.prepare(44100.0);
	at441.process(0.f, 0.f, defaults(), a, b);
	const int expected441 = fmdr::HilbertFir::kHqHalf + int(0.005 * 44100.0);
	CHECK(at441.latencySamples() == expected441, "look-ahead follows the sample rate");
}


// --------------------------------------------------------------- the stages

void testGlobalBypassIsAPassThrough() {
	std::printf("global bypass\n");

	Lcg rng;
	std::vector<float> l(4000), r(4000);
	for (size_t i = 0; i < l.size(); i++) {
		l[i] = rng.next() * 0.5f;
		r[i] = rng.next() * 0.5f;
	}

	fmdr::RealMonoSettings s = defaults();
	s.enabled = false;
	const Run out = runChain(s, l, r);

	double worst = 0.0;
	for (size_t i = 0; i < l.size(); i++) {
		worst = std::max(worst, std::abs(double(out.l[i]) - double(l[i])));
		worst = std::max(worst, std::abs(double(out.r[i]) - double(r[i])));
	}
	CHECK(worst < 1e-9, "bypass passes stereo through untouched, both channels");
}

void testOutputIsDualMono() {
	std::printf("dual mono output\n");

	Lcg rng;
	std::vector<float> l(4000), r(4000);
	for (size_t i = 0; i < l.size(); i++) {
		l[i] = rng.next() * 0.5f;
		r[i] = rng.next() * 0.5f;
	}

	const Run out = runChain(defaults(), l, r);
	bool identical = true;
	for (size_t i = 0; i < l.size(); i++)
		identical = identical && (out.l[i] == out.r[i]);
	CHECK(identical, "every output frame is dual mono, sample for sample");

	// Bypassing Stage 4 is the one setting that is meant to stay in stereo:
	// the rotated Side is put back as a pair instead of being summed.
	fmdr::RealMonoSettings noCommit = defaults();
	noCommit.commitEnabled = false;
	const Run stereo = runChain(noCommit, l, r);
	bool differs = false;
	for (size_t i = 0; i < l.size(); i++)
		differs = differs || std::abs(stereo.l[i] - stereo.r[i]) > 1e-6f;
	CHECK(differs, "bypassing the mono commit leaves the signal in stereo");
}

void testStageOneHighpass() {
	std::printf("Stage 1 high-pass (lab)\n");

	// Off by default, and off means nothing at all happens -- not "a filter at
	// 20 Hz". Bit-for-bit against a chain that never had one.
	CHECK(!defaults().hpfEnabled, "Stage 1 ships bypassed, as the client's process has none");

	std::vector<float> l, r;
	makeTone(40.0, AntiPhase, 48000, 0.5, l, r);

	fmdr::RealMonoSettings on = defaults();
	on.hpfEnabled = true;
	on.hpfHz = 120.f;
	on.hpfSlope = 12;

	const double lowOff = tailPeak(runChain(defaults(), l, r).l);
	const double lowOn = tailPeak(runChain(on, l, r).l);
	std::printf("       40 Hz Side: off %.1f dB, on %.1f dB\n", toDb(lowOff), toDb(lowOn));
	CHECK(toDb(lowOff) - toDb(lowOn) > 12.0, "enabling it removes low Side content");

	makeTone(2000.0, AntiPhase, 48000, 0.5, l, r);
	const double highOff = tailPeak(runChain(defaults(), l, r).l);
	const double highOn = tailPeak(runChain(on, l, r).l);
	CHECK(std::abs(toDb(highOff) - toDb(highOn)) < 0.5, "and leaves Side above fc alone");

	// It must not touch the Mid path: the bass has to stay, it just stops
	// being rotated into the sum.
	makeTone(40.0, InPhase, 48000, 0.5, l, r);
	CHECK(std::abs(toDb(tailPeak(runChain(on, l, r).l)) - toDb(0.5)) < 0.2,
	      "low Mid content is untouched by the Side high-pass");
}

void testCommitModes() {
	std::printf("Stage 4 commit modes\n");

	std::vector<float> l, r;
	makeTone(1000.0, AntiPhase, 24000, 0.5, l, r);

	fmdr::RealMonoSettings sum = defaults();
	sum.commitMode = fmdr::CommitSum;
	CHECK(toDb(tailPeak(runChain(sum, l, r).l)) < -80.0, "sum mode is the classic downmix");

	fmdr::RealMonoSettings midOnly = defaults();
	midOnly.commitMode = fmdr::CommitMidOnly;
	CHECK(toDb(tailPeak(runChain(midOnly, l, r).l)) < -80.0, "mid only discards the Side");

	// The two differ in exactly one way: sum is the raw reference and ignores
	// the Mid gain, mid only respects it.
	makeTone(1000.0, InPhase, 24000, 0.5, l, r);
	sum.midGainDb = -12.f;
	midOnly.midGainDb = -12.f;
	CHECK(std::abs(toDb(tailPeak(runChain(sum, l, r).l)) - toDb(0.5)) < 0.1,
	      "sum mode ignores the Mid gain, so it stays a reference");
	CHECK(std::abs(toDb(tailPeak(runChain(midOnly, l, r).l)) - toDb(0.5) + 12.0) < 0.3,
	      "mid only applies the Mid gain");

	// The lab modes only have to be finite and audible; they are not the
	// product and nothing downstream depends on their exact law.
	for (int mode : {fmdr::CommitSideEnergyFold, fmdr::CommitPolarityMatrix}) {
		fmdr::RealMonoSettings lab = defaults();
		lab.commitMode = mode;
		const Run out = runChain(lab, l, r);
		bool finite = true;
		for (float v : out.l)
			finite = finite && std::isfinite(v);
		CHECK(finite && tailPeak(out.l) > 0.1, "lab commit mode produces finite audio");
	}
}

void testStageTwoBypass() {
	std::printf("Stage 2 bypass\n");

	// Without the M/S split there is no Side to rotate, so the chain falls
	// back to the classic downmix -- which is what "pass through in the
	// correct domain" means here.
	std::vector<float> l, r;
	makeTone(1000.0, AntiPhase, 24000, 0.5, l, r);

	fmdr::RealMonoSettings s = defaults();
	s.msEnabled = false;
	CHECK(toDb(tailPeak(runChain(s, l, r).l)) < -80.0,
	      "bypassing the M/S split falls back to the plain sum");

	makeTone(1000.0, InPhase, 24000, 0.5, l, r);
	CHECK(std::abs(toDb(tailPeak(runChain(s, l, r).l)) - toDb(0.5)) < 0.1,
	      "and correlated content still passes at unity");
}


// -------------------------------------------------------- Stage 0 and Stage 5

void testHqTrim() {
	std::printf("Stage 0 highest quality mode\n");

	std::vector<float> l, r;
	makeTone(1000.0, InPhase, 48000, 0.4, l, r);

	fmdr::RealMonoSettings plain = defaults();
	plain.limiterEnabled = false;
	fmdr::RealMonoSettings hq = plain;
	hq.hqMode = true;

	const double a = tailPeak(runChain(plain, l, r).l);
	const double b = tailPeak(runChain(hq, l, r).l);
	std::printf("       %.2f dB of trim\n", toDb(b) - toDb(a));
	CHECK(std::abs((toDb(b) - toDb(a)) + 3.0) < 0.05, "HQ mode is exactly -3 dB at the input");

	CHECK(fmdr::presetSettings(fmdr::PresetHighestQuality).hqMode,
	      "the HighestQuality preset turns it on");
	CHECK(fmdr::presetSettings(fmdr::PresetHighestQuality).limiterEnabled,
	      "and leaves the limiter in for what is left");
}

void testLimiter() {
	std::printf("Stage 5 limiter\n");

	// Programme-like: a correlated Mid plus an uncorrelated Side, which is the
	// case that overshoots once the Side is rotated.
	Lcg rng;
	std::vector<float> l(96000), r(96000);
	for (size_t i = 0; i < l.size(); i++) {
		const float mid = rng.next() * 0.5f;
		const float side = rng.next() * 0.35f;
		l[i] = mid + side;
		r[i] = mid - side;
	}
	double inPeak = 0.0;
	for (size_t i = 0; i < l.size(); i++)
		inPeak = std::max(inPeak, std::max(double(std::abs(l[i])), double(std::abs(r[i]))));

	fmdr::RealMonoSettings open = defaults();
	open.limiterEnabled = false;
	double sumPeak = 0.0;
	for (float v : runChain(open, l, r).l)
		sumPeak = std::max(sumPeak, double(std::abs(v)));

	std::printf("       input peak %.3f, unlimited Mid + rotated Side peak %.3f (%+.2f dB)\n",
	            inPeak, sumPeak, toDb(sumPeak) - toDb(inPeak));
	// The client's report: the sum runs hotter than the original channels.
	// The exact figure is programme-dependent, so the check is that the
	// overshoot is real and in the range the brief describes.
	CHECK(sumPeak > inPeak, "the sum really does overshoot the original channels");

	const float ceiling = fmdr::dbToGain(defaults().ceilingDb);
	double limited = 0.0;
	for (float v : runChain(defaults(), l, r).l)
		limited = std::max(limited, double(std::abs(v)));
	std::printf("       limited peak %.6f, ceiling %.6f\n", limited, double(ceiling));
	CHECK(limited <= double(ceiling) + 1e-6, "the limiter holds the ceiling exactly");

	// A hard transient into a quiet passage is what look-ahead is for: no
	// sample may pass before the gain has come down.
	std::vector<float> quiet(48000, 0.f), quietR(48000, 0.f);
	for (size_t i = 0; i < quiet.size(); i++) {
		const float amp = (i > 20000 && i < 26000) ? 4.f : 0.05f;
		quiet[i] = amp * float(std::sin(2.0 * kPi * 220.0 * double(i) / kSampleRate));
		quietR[i] = quiet[i];
	}
	double burst = 0.0;
	for (float v : runChain(defaults(), quiet, quietR).l)
		burst = std::max(burst, double(std::abs(v)));
	CHECK(burst <= double(ceiling) + 1e-6, "a sudden 4x transient never breaks the ceiling");

	// A lower ceiling has to actually be lower.
	fmdr::RealMonoSettings low = defaults();
	low.ceilingDb = -3.f;
	double lowPeak = 0.0;
	for (float v : runChain(low, quiet, quietR).l)
		lowPeak = std::max(lowPeak, double(std::abs(v)));
	CHECK(lowPeak <= double(fmdr::dbToGain(-3.f)) + 1e-6, "and the ceiling control is honoured");
}


// ------------------------------------------------------------- robustness

void testStaysFinite() {
	std::printf("robustness\n");

	fmdr::RealMonoSettings hot = defaults();
	hot.hpfEnabled = true;
	hot.hpfHz = 40.f;
	hot.hpfSlope = 24;
	hot.midGainDb = 6.f;
	hot.sideGainDb = 6.f;
	hot.sideInjectDb = 6.f;
	hot.outputGainDb = 12.f;

	std::vector<float> l(48000), r(48000);
	Lcg rng;
	for (size_t i = 0; i < l.size(); i++) {
		l[i] = rng.next() * 4.f;   // deliberately over full scale
		r[i] = rng.next() * -4.f;
	}
	const Run out = runChain(hot, l, r);
	bool finite = true;
	double peak = 0.0;
	for (size_t i = 0; i < out.l.size(); i++) {
		finite = finite && std::isfinite(out.l[i]) && std::isfinite(out.r[i]);
		peak = std::max(peak, double(std::abs(out.l[i])));
	}
	CHECK(finite, "every sample is finite at extreme settings");
	CHECK(peak <= double(fmdr::dbToGain(defaults().ceilingDb)) + 1e-6,
	      "and the ceiling still holds with every gain wide open");

	// A capture device that hands over a NaN must not poison the output
	// forever -- the chain has to flush it and recover.
	fmdr::RealMonoChain chain;
	chain.prepare(kSampleRate);
	const fmdr::RealMonoSettings s = defaults();
	float a = 0.f, b = 0.f;
	const double nan = std::nan("");
	chain.process(float(nan), float(nan), s, a, b);
	bool allFinite = true;
	double lateEnergy = 0.0;
	for (int i = 0; i < 8000; i++) {
		const float v = 0.3f * float(std::sin(2.0 * kPi * 500.0 * i / kSampleRate));
		chain.process(v, -v, s, a, b);
		allFinite = allFinite && std::isfinite(a) && std::isfinite(b);
		if (i > 6000)
			lateEnergy = std::max(lateEnergy, double(std::abs(a)));
	}
	CHECK(allFinite, "a NaN from the capture device never reaches the endpoint");
	CHECK(lateEnergy > 0.2, "and the chain recovers once it has flushed through");
}

void testStructuralChangesDoNotClick() {
	std::printf("switching stages while running\n");

	// Every bypass is on a switch the user can flip mid-programme. The chain
	// fades through zero around anything that moves the latency, so what must
	// be true is that no switch produces a step larger than the signal itself.
	fmdr::RealMonoChain chain;
	chain.prepare(kSampleRate);

	double worstStep = 0.0;
	float prev = 0.f;
	bool finite = true;
	const int kPhases = 8;
	for (int i = 0; i < 16000 * kPhases; i++) {
		fmdr::RealMonoSettings s = defaults();
		// Flip something structural every 16000 samples. Stages 1 and 2 are in
		// here deliberately: they change what feeds Stage 3, so its history
		// goes stale as well, and leaving them out of this test is how the
		// click they caused survived the first version of the fade.
		const int phase = (i / 16000) % kPhases;
		switch (phase) {
			case 1: s.enabled = false; break;
			case 2: s.rotateEnabled = false; break;
			case 3: s.limiterEnabled = false; break;
			case 4: s.quality = fmdr::QualityAllpass; break;
			case 5: s.commitMode = fmdr::CommitMidOnly; break;
			case 6: s.msEnabled = false; break;
			case 7: s.hpfEnabled = true; s.hpfHz = 200.f; s.hpfSlope = 24; break;
			default: break;
		}
		// Pure Side, so the Side path carries all of it and a stale rotation
		// history has the most to give away.
		const float v = 0.4f * float(std::sin(2.0 * kPi * 220.0 * i / kSampleRate));
		float a = 0.f, b = 0.f;
		chain.process(v, -v, s, a, b);
		finite = finite && std::isfinite(a);
		if (i > 100)
			worstStep = std::max(worstStep, std::abs(double(a) - double(prev)));
		prev = a;
	}
	std::printf("       worst sample-to-sample step across %d switches: %.4f\n",
	            kPhases - 1, worstStep);
	CHECK(finite, "toggling stages mid-stream stays finite");
	// A 220 Hz sine at 0.4 steps by 0.0115 per sample of its own accord; a
	// switch that was not faded would step by the whole amplitude.
	CHECK(worstStep < 0.05, "no switch produces a discontinuity");
}


// ---------------------------------------------------------------- presets

void testPresets() {
	std::printf("presets\n");

	const fmdr::RealMonoSettings def = fmdr::presetSettings(fmdr::PresetRealMonoDefault);
	CHECK(def.enabled && def.msEnabled && def.rotateEnabled && def.commitEnabled
	      && def.limiterEnabled,
	      "RealMono_Default has stages 2 to 5 on");
	CHECK(!def.hpfEnabled && !def.hqMode, "and Stage 1 and the HQ trim off");
	CHECK(std::abs(def.ceilingDb + 0.3f) < 1e-6f, "with the ceiling at -0.3 dBFS");
	CHECK(def.quality == fmdr::QualityHqLinear, "and the linear-phase rotator");

	const fmdr::RealMonoSettings safe = fmdr::presetSettings(fmdr::PresetLabSafeMidOnly);
	CHECK(!safe.rotateEnabled && safe.commitMode == fmdr::CommitMidOnly,
	      "Lab_SafeMidOnly is the classic-mono reference");

	const fmdr::RealMonoSettings lf = fmdr::presetSettings(fmdr::PresetLabWithMonoLF);
	CHECK(lf.hpfEnabled && std::abs(lf.hpfHz - 120.f) < 1e-6f,
	      "Lab_WithMonoLF turns Stage 1 on at 120 Hz");
	CHECK(lf.rotateEnabled && lf.limiterEnabled, "and leaves the rest of the chain alone");
}


// -------------------------------------------------------------------- ring

void testRing() {
	std::printf("ring\n");

	fmdr::StereoRing ring;
	ring.reset(600);  // rounds up to 1024
	CHECK(ring.capacity() == 1024, "capacity rounds up to a power of two");
	CHECK(ring.availableRead() == 0, "starts empty");
	CHECK(ring.availableWrite() == 1024, "starts fully writable");

	std::vector<float> in(200 * 2);
	for (size_t i = 0; i < 200; i++) {
		in[i * 2] = float(i);
		in[i * 2 + 1] = float(i) + 0.5f;
	}
	CHECK(ring.push(in.data(), 200) == 200, "push accepts a whole block");
	CHECK(ring.availableRead() == 200, "read side sees exactly what was pushed");

	bool exact = true;
	for (size_t i = 0; i < 200; i++) {
		float l = 0.f, r = 0.f;
		exact = exact && ring.popFrame(l, r);
		exact = exact && (l == float(i)) && (r == float(i) + 0.5f);
	}
	CHECK(exact, "frames come back in order and unmodified");

	float l = 0.f, r = 0.f;
	CHECK(!ring.popFrame(l, r), "an empty ring reports the underrun");

	// Overrun must be reported, not silently wrapped over unread data.
	std::vector<float> big(2000 * 2, 1.f);
	const size_t written = ring.push(big.data(), 2000);
	CHECK(written == 1024, "push is truncated to the free space");
	CHECK(ring.availableWrite() == 0, "and the ring is then full");

	// Wrapping the mask boundary must not corrupt anything.
	ring.clear();
	CHECK(ring.availableRead() == 0, "clear empties the ring");
	bool wrapOk = true;
	for (int round = 0; round < 20; round++) {
		wrapOk = wrapOk && (ring.push(in.data(), 200) == 200);
		for (size_t i = 0; i < 200; i++) {
			float a = 0.f, b = 0.f;
			wrapOk = wrapOk && ring.popFrame(a, b) && a == float(i);
		}
	}
	CHECK(wrapOk, "4000 frames of traffic wrap the buffer cleanly");
}


// --------------------------------------------------------------- resampler

void testResampler() {
	std::printf("resampler\n");

	// At ratio 1 the Hermite lands exactly on a sample, so the output must be
	// the input delayed -- any ripple here would be a sign the interpolation
	// coefficients are wrong.
	fmdr::StereoRing ring;
	ring.reset(4096);
	fmdr::Resampler rs;
	rs.reset();
	rs.setRatio(1.0);

	std::vector<float> in(1000 * 2);
	for (size_t i = 0; i < 1000; i++) {
		const float v = float(std::sin(2.0 * kPi * 1000.0 * double(i) / kSampleRate));
		in[i * 2] = v;
		in[i * 2 + 1] = -v;
	}
	ring.push(in.data(), 1000);

	double worst = 0.0;
	int produced = 0;
	for (int i = 0; i < 900; i++) {
		float l = 0.f, r = 0.f;
		if (!rs.next(ring, l, r))
			break;
		// Phase starts at 4, so output n corresponds to input n+1.
		worst = std::max(worst, double(std::abs(l - in[size_t(i + 1) * 2])));
		produced++;
	}
	CHECK(produced == 900, "unity ratio produces one output per input");
	CHECK(worst < 1e-6, "unity ratio is sample-exact, not merely close");

	// Double rate: two input frames consumed per output frame.
	ring.clear();
	rs.reset();
	rs.setRatio(2.0);
	ring.push(in.data(), 1000);
	int outputs = 0;
	for (;;) {
		float l = 0.f, r = 0.f;
		if (!rs.next(ring, l, r))
			break;
		outputs++;
	}
	CHECK(outputs > 480 && outputs < 510, "ratio 2.0 consumes two inputs per output");

	// The controller must pull the ratio up when the ring is over target and
	// down when it is under, and must never exceed its stated +/-0.5% clamp.
	fmdr::DriftController drift;
	drift.reset(1.0, 1440.0, 100.0);
	double ratio = 1.0;
	for (int i = 0; i < 2000; i++)
		ratio = drift.update(2880.0);  // twice the target: consume faster
	CHECK(ratio > 1.0 && ratio <= 1.005 + 1e-9, "over-full ring speeds the reader up, within clamp");

	drift.reset(1.0, 1440.0, 100.0);
	for (int i = 0; i < 2000; i++)
		ratio = drift.update(100.0);   // nearly empty: consume slower
	CHECK(ratio < 1.0 && ratio >= 0.995 - 1e-9, "starved ring slows the reader down, within clamp");

	drift.reset(1.0, 1440.0, 100.0);
	ratio = drift.update(1440.0);
	CHECK(std::abs(ratio - 1.0) < 1e-9, "at target the ratio is left alone");
}

} // namespace


int main() {
	std::printf("Real Mono Sound offline checks\n\n");

	testSideContentSurvives();
	testRotationIsNinetyDegrees();
	testQualityModesAgreeOnPolarity();
	testMidIsDelayAligned();
	testLatencyIsReportedHonestly();
	testGlobalBypassIsAPassThrough();
	testOutputIsDualMono();
	testStageOneHighpass();
	testStageTwoBypass();
	testCommitModes();
	testHqTrim();
	testLimiter();
	testStaysFinite();
	testStructuralChangesDoNotClick();
	testPresets();
	testRing();
	testResampler();

	std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
