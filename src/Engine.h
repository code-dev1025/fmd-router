#pragma once

#include "AudioFormat.h"
#include "Com.h"
#include "RealMonoChain.h"
#include "Resampler.h"
#include "Ring.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace fmdr {

struct EngineConfig {
	/** Endpoint to take audio from. Empty means the current default. */
	std::wstring captureId;
	/** Endpoint to play the processed mono signal to. Empty means default. */
	std::wstring renderId;
	/** When true, captureId names a *render* endpoint and its output is taken
	    by loopback instead. Useful for trying the chain without a virtual
	    cable installed -- but the user then hears the unprocessed audio too,
	    which is exactly what the cable exists to prevent. */
	bool loopback = false;
	/** How much audio to hold in the ring, in milliseconds. The floor is
	    raised automatically to whatever the two devices' buffers require. */
	double targetBufferMs = 30.0;
};


struct EngineStats {
	bool running = false;
	uint32_t captureRate = 0;
	uint32_t renderRate = 0;
	uint32_t captureChannels = 0;
	uint32_t renderChannels = 0;
	double ringMs = 0.0;         // audio currently queued
	double roundTripMs = 0.0;    // ring + chain + the render endpoint's buffer
	double processingMs = 0.0;   // the chain's own latency (Hilbert + look-ahead)
	double capturePeriodMs = 0.0;
	double renderPeriodMs = 0.0;
	double ratio = 1.0;         // resampler ratio actually in use
	uint64_t underruns = 0;
	uint64_t overruns = 0;
	float inPeakL = 0.f;
	float inPeakR = 0.f;
	float outPeak = 0.f;

	// The Advanced page's meters. Correlation is +1 for a mono source, 0 for
	// unrelated channels and -1 for anti-phase -- which is the signal that
	// would vanish entirely in a plain downmix, so it is the one number that
	// says whether Real Mono has anything to recover.
	float midPeak = 0.f;
	float sidePeak = 0.f;
	float correlation = 0.f;
	float gainReductionDb = 0.f;  // most reduction since the last read, <= 0
};


/*  Two threads, one ring.

    The capture thread pushes whatever the source endpoint gives it. The render
    thread pulls through the resampler, runs the chain, and writes mono. They
    share nothing but the lock-free ring and a few atomics, so neither can
    stall the other -- which is the entire reason this is not one thread with a
    mutex around it.

    All COM work happens on the thread that will use the object. start() blocks
    until each thread has either initialised or failed, so a bad device or an
    unsupported format is reported synchronously to the GUI rather than turning
    into silence and a puzzled user. */
class Engine {
public:
	Engine() = default;
	~Engine();

	Engine(const Engine&) = delete;
	Engine& operator=(const Engine&) = delete;

	bool start(const EngineConfig& cfg, std::wstring& err);
	void stop();

	bool running() const {
		return running_.load(std::memory_order_acquire);
	}

	EngineStats stats();

	/** Returns and clears an error raised by a thread after start() succeeded
	    -- a device unplugged mid-stream, typically. Empty if all is well. */
	std::wstring takeAsyncError();

	/** Written by the GUI thread, read by the render thread. */
	RealMonoParams params;

	/** The chain's added delay in milliseconds, or 0 while stopped. Published
	    separately from stats() because the UI shows it whether or not anything
	    is running. */
	double processingLatencyMs() const;

private:
	struct Ready {
		std::mutex mutex;
		std::condition_variable cv;
		bool done = false;
		bool ok = false;
		std::wstring err;
	};

	void captureThread(EngineConfig cfg, Ready* ready);
	void renderThread(EngineConfig cfg, Ready* ready);
	void raiseAsyncError(std::wstring message);
	/** Called once by each thread, on success or on the first failure. */
	static void setReady(Ready& ready, bool ok, std::wstring err);
	static bool waitReady(Ready& ready, std::wstring& err);

	StereoRing ring_;
	RealMonoChain chain_;
	Resampler resampler_;
	DriftController drift_;

	std::thread capture_;
	std::thread render_;

	std::atomic<bool> quit_{false};
	std::atomic<bool> running_{false};

	// Published for the GUI. Peaks are stored as the highest value seen since
	// the last stats() call, which resets them -- a meter that decays is the
	// GUI's business, not the audio thread's.
	std::atomic<uint32_t> captureRate_{0};
	std::atomic<uint32_t> renderRate_{0};
	std::atomic<uint32_t> captureChannels_{0};
	std::atomic<uint32_t> renderChannels_{0};
	std::atomic<uint32_t> capturePeriodFrames_{0};
	std::atomic<uint32_t> renderPeriodFrames_{0};
	std::atomic<uint32_t> renderBufferFrames_{0};
	std::atomic<uint64_t> underruns_{0};
	std::atomic<uint64_t> overruns_{0};
	std::atomic<float> inPeakL_{0.f};
	std::atomic<float> inPeakR_{0.f};
	std::atomic<float> outPeak_{0.f};
	std::atomic<float> midPeak_{0.f};
	std::atomic<float> sidePeak_{0.f};
	std::atomic<float> correlation_{0.f};
	std::atomic<float> minLimiterGain_{1.f};
	std::atomic<uint32_t> chainLatencyFrames_{0};
	std::atomic<double> ratio_{1.0};

	std::mutex errorMutex_;
	std::wstring asyncError_;
};

} // namespace fmdr
