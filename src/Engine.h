#pragma once

#include "AudioFile.h"
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
	/** Take audio from the file loaded by loadFile() rather than from an
	    endpoint. captureId and loopback are then unused. This is the path that
	    lets the chain be judged by ear with nothing else installed: no cable,
	    no second endpoint, and no third application playing. */
	bool fromFile = false;
	/** Restart the file at its end instead of stopping. On by default, because
	    the A/B this exists for is "toggle the processing while it plays" and a
	    file that stops after one pass makes that a chore. */
	bool loopFile = true;
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

	// File playback. Position is where the file is being read, which leads
	// what is audible by the round trip -- close enough for a clock in a
	// status line, and it is not pretending to be anything else.
	double filePositionSeconds = 0.0;
	/** Set once a non-looping file has played out, so the GUI can stop. */
	bool fileEnded = false;
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

	/** Decodes a file into memory, ready for a `fromFile` start. Blocks for as
	    long as the decode takes, so it belongs on the GUI thread with a wait
	    cursor up, not on an audio thread. Refused while running: the file is
	    the thing being read, and swapping it under the reader is not worth the
	    lock it would cost.

	    Returns true with a non-empty `err` when the file loaded but something
	    about it is worth saying -- it was too long and got trimmed, so far. */
	bool loadFile(const std::wstring& path, std::wstring& err);

	struct FileInfo {
		std::wstring path;
		std::wstring name;      // the last path component, for the UI
		uint32_t sampleRate = 0;
		size_t frames = 0;
		double seconds = 0.0;
		bool loaded = false;
	};

	FileInfo fileInfo() const;

	/** Live, unlike the rest of EngineConfig: turning looping off part way
	    through a pass is how you let a file end without stopping it dead. */
	void setLoopFile(bool loop) {
		loopFile_.store(loop, std::memory_order_relaxed);
	}

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
	/** Stands in for captureThread when the source is a file: same ring, same
	    metering, same contract with the render thread. */
	void fileThread(EngineConfig cfg, Ready* ready);
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

	// Decoded once by loadFile and only read after that, by whichever thread is
	// playing it. Nothing touches it while the engine runs, which is what makes
	// the bare reference in fileThread safe.
	AudioBuffer file_;
	std::wstring filePath_;
	std::atomic<size_t> filePosition_{0};
	std::atomic<bool> fileEnded_{false};
	std::atomic<bool> loopFile_{true};

	/** Where the render thread wants the ring held, in capture-rate frames.
	    A device producer cannot be told to slow down and so never reads this;
	    a file producer can, and aiming at the same number is what keeps the
	    drift controller out of the file path entirely. */
	std::atomic<double> ringTargetFrames_{0.0};

	std::mutex errorMutex_;
	std::wstring asyncError_;
};

} // namespace fmdr
