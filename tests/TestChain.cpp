/*  Offline checks for the parts of fmd-router that have nothing to do with
    Windows: the stereo-to-mono chain, the ring and the resampler.

    These exist because "I can hear it" is not a regression test. Every claim
    the README makes about the chain -- that it really collapses to mono, that
    the filter is genuinely in circuit, that SPREAD still does something after
    the collapse -- is asserted here against numbers.

    Build: cmake --build build --config Release --target fmd-router-tests
    Run:   build/Release/fmd-router-tests.exe   (exit code 0 = all passed) */

#include "MonoChain.h"
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

struct Sine {
	double phase = 0.0;
	double inc = 0.0;

	void set(double hz) {
		inc = 2.0 * kPi * hz / kSampleRate;
	}

	float next() {
		const float v = float(std::sin(phase));
		phase += inc;
		if (phase > 2.0 * kPi)
			phase -= 2.0 * kPi;
		return v;
	}
};

fmdr::ChainSettings baseSettings() {
	fmdr::ChainSettings s;
	s.module = fmdr::ModFlowerChild;
	s.mode = fmd::FilterCore::MODE_LP12;
	s.freqOct = 8.f;   // 20 Hz * 2^8 = 5.12 kHz
	s.res = 0.f;
	s.drive = 0.f;
	s.spread = 0.f;
	s.grit = 0.f;
	s.clip = 0.f;
	s.mix = 1.f;
	s.outGain = 1.f;
	return s;
}

enum Phase { InPhase, AntiPhase };

/** Runs a sine through the chain and returns the peak of the final quarter,
    by which point both the filter and the control smoothing have settled. */
double runPeak(const fmdr::ChainSettings& s, double toneHz, Phase phase,
               double seconds = 0.5, double amplitude = 0.5) {
	fmdr::MonoChain chain;
	chain.prepare(kSampleRate);

	Sine osc;
	osc.set(toneHz);

	const int total = int(kSampleRate * seconds);
	const int measureFrom = total * 3 / 4;
	double peak = 0.0;

	for (int i = 0; i < total; i++) {
		const float v = osc.next() * float(amplitude);
		const float l = v;
		const float r = (phase == AntiPhase) ? -v : v;
		const float y = chain.processFrame(l, r, s);
		if (i >= measureFrom)
			peak = std::max(peak, double(std::abs(y)));
	}
	return peak;
}

double toDb(double linear) {
	return (linear <= 1e-12) ? -240.0 : 20.0 * std::log10(linear);
}


// ------------------------------------------------------------------- chain

void testMonoCollapse() {
	std::printf("stereo -> mono\n");

	// Hard left/right anti-phase is pure side signal. A chain that genuinely
	// sums to mono annihilates it; one that filters in stereo and collapses
	// afterwards would not, because the two filter voices differ.
	const double side = runPeak(baseSettings(), 440.0, AntiPhase);
	CHECK(side < 1e-6, "anti-phase input collapses to silence");

	// The same tone in phase must survive, or the previous check would pass
	// trivially for a chain that outputs nothing at all.
	const double mid = runPeak(baseSettings(), 440.0, InPhase);
	CHECK(mid > 0.4, "in-phase input passes through");

	// SPREAD detunes the two filter voices. It must not reintroduce the side
	// signal -- the sum happens before the filter, so there is nothing left of
	// it to reintroduce.
	fmdr::ChainSettings spread = baseSettings();
	spread.spread = 0.8f;
	CHECK(runPeak(spread, 440.0, AntiPhase) < 1e-6,
	      "anti-phase stays silent even with SPREAD wide open");
}

void testDryPathIsTheMonoSum() {
	std::printf("MIX\n");

	fmdr::ChainSettings dry = baseSettings();
	dry.mix = 0.f;

	fmdr::MonoChain chain;
	chain.prepare(kSampleRate);

	Sine osc;
	osc.set(220.0);

	double worst = 0.0;
	for (int i = 0; i < 4800; i++) {
		const float v = osc.next() * 0.5f;
		// Deliberately unequal channels, so "the dry path is (L+R)/2" is a
		// stronger claim than "the dry path is L".
		const float l = v;
		const float r = v * 0.25f;
		const float y = chain.processFrame(l, r, dry);
		worst = std::max(worst, double(std::abs(y - (l + r) * 0.5f)));
	}
	CHECK(worst < 1e-6, "MIX at 0 is exactly the mono sum, unfiltered");
}

void testFilterIsInCircuit() {
	std::printf("filter\n");

	fmdr::ChainSettings s = baseSettings();
	s.freqOct = 4.6438f;  // 20 Hz * 2^4.6438 = 500 Hz

	const double low = runPeak(s, 100.0, InPhase);
	const double high = runPeak(s, 10000.0, InPhase);
	const double rejection = toDb(low) - toDb(high);

	std::printf("       100 Hz %.1f dB, 10 kHz %.1f dB, rejection %.1f dB\n",
	            toDb(low), toDb(high), rejection);
	// A 12 dB/oct low pass 4.3 octaves below the tone should give ~50 dB. The
	// bar is set well under that so this checks "the filter is wired in", not
	// the exact slope, which belongs to fmd-vcv's own tests.
	CHECK(rejection > 30.0, "LP12 rejects 10 kHz far more than 100 Hz");

	// Super Love is the module with a full mode control. Flower Child is not:
	// its panel has no mode switch, so MonoChain pins it to LP12 -- which is
	// checked below rather than assumed, because it is easy to "fix" that pin
	// by accident and only notice when the wrong product ships.
	// Probed symmetrically about the 500 Hz corner: 25 Hz and 10 kHz are both
	// about 4.3 octaves out, so LP and HP face the same test. (100 Hz is only
	// 2.3 octaves down, which is a genuine ~28 dB for a 12 dB/oct slope -- a
	// threshold of 30 there would be testing the probe, not the filter.)
	fmdr::ChainSettings hp = s;
	hp.module = fmdr::ModSuperLove;
	hp.mode = fmd::FilterCore::MODE_HP;
	const double hpLow = runPeak(hp, 25.0, InPhase, 1.0);
	const double hpHigh = runPeak(hp, 10000.0, InPhase);
	std::printf("       HP: 25 Hz %.1f dB, 10 kHz %.1f dB, rejection %.1f dB\n",
	            toDb(hpLow), toDb(hpHigh), toDb(hpHigh) - toDb(hpLow));
	CHECK(toDb(hpHigh) - toDb(hpLow) > 30.0, "HP mode rejects the opposite end");

	fmdr::ChainSettings pinned = s;
	pinned.module = fmdr::ModFlowerChild;
	pinned.mode = fmd::FilterCore::MODE_HP;  // ignored: no mode control on this panel
	CHECK(std::abs(toDb(runPeak(pinned, 10000.0, InPhase)) - toDb(high)) < 0.5,
	      "Flower Child stays LP12 whatever mode is asked for");
}

void testSpreadStillMattersInMono() {
	std::printf("SPREAD after the collapse\n");

	// The design note in MonoChain.h claims SPREAD stays meaningful in mono
	// because the sum is fed through both detuned voices. If that were wrong --
	// if the code collapsed after filtering -- these two would be identical.
	fmdr::ChainSettings narrow = baseSettings();
	narrow.res = 0.7f;
	narrow.freqOct = 6.f;

	fmdr::ChainSettings wide = narrow;
	wide.spread = 0.7f;

	const double a = runPeak(narrow, 1280.0, InPhase);
	const double b = runPeak(wide, 1280.0, InPhase);
	std::printf("       spread 0: %.1f dB, spread 0.7: %.1f dB\n", toDb(a), toDb(b));
	CHECK(std::abs(toDb(a) - toDb(b)) > 6.0, "SPREAD changes the mono output");
}

void testStaysFinite() {
	std::printf("robustness\n");

	fmdr::ChainSettings hot = baseSettings();
	hot.res = 1.f;
	hot.drive = 1.f;
	hot.grit = 1.f;
	hot.clip = 1.f;
	hot.spread = 1.f;
	hot.outGain = 8.f;
	hot.aggressive = true;

	fmdr::MonoChain chain;
	chain.prepare(kSampleRate);

	Sine osc;
	osc.set(50.0);
	bool finite = true;
	double peak = 0.0;
	for (int i = 0; i < int(kSampleRate * 2); i++) {
		const float v = osc.next() * 4.f;  // deliberately over full scale
		const float y = chain.processFrame(v, -v * 0.5f, hot);
		finite = finite && std::isfinite(y);
		peak = std::max(peak, double(std::abs(y)));
	}
	CHECK(finite, "every sample is finite at extreme settings");
	std::printf("       peak %.2f (clamped to +/-1 at the endpoint)\n", peak);

	// A module change resets the filter; it must not produce a NaN on the way.
	fmdr::MonoChain switcher;
	switcher.prepare(kSampleRate);
	bool switchFinite = true;
	for (int i = 0; i < 4800; i++) {
		fmdr::ChainSettings s = baseSettings();
		s.module = (i / 480) % fmdr::NumModules;
		s.mode = fmd::FilterCore::MODE_BP;
		s.res = 0.9f;
		switchFinite = switchFinite && std::isfinite(switcher.processFrame(0.5f, 0.4f, s));
	}
	CHECK(switchFinite, "switching modules mid-stream stays finite");
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
	Sine osc;
	osc.set(1000.0);
	for (size_t i = 0; i < 1000; i++) {
		const float v = osc.next();
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
	std::printf("fmd-router offline checks\n\n");

	testMonoCollapse();
	testDryPathIsTheMonoSum();
	testFilterIsInCircuit();
	testSpreadStillMattersInMono();
	testStaysFinite();
	testRing();
	testResampler();

	std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
