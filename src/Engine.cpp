#include "Engine.h"

#include "Devices.h"
#include "MediaFile.h"

#include <audioclient.h>
#include <avrt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace fmdr {

namespace {

constexpr REFERENCE_TIME kHnsPerMs = 10000;

/** Frees a WAVEFORMATEX from GetMixFormat without a goto ladder. */
struct MixFormat {
	WAVEFORMATEX* wf = nullptr;

	~MixFormat() {
		if (wf)
			CoTaskMemFree(wf);
	}

	MixFormat() = default;
	MixFormat(const MixFormat&) = delete;
	MixFormat& operator=(const MixFormat&) = delete;
};

/** Asks MMCSS to treat this thread as audio. Failure is not fatal -- it only
    means the thread runs at normal priority and is likelier to glitch under
    load -- so it is deliberately not reported as an error. */
struct ProAudioPriority {
	HANDLE task = nullptr;
	DWORD index = 0;

	ProAudioPriority() {
		task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
	}

	~ProAudioPriority() {
		if (task)
			AvRevertMmThreadCharacteristics(task);
	}

	ProAudioPriority(const ProAudioPriority&) = delete;
	ProAudioPriority& operator=(const ProAudioPriority&) = delete;
};

/** RAII for an auto-reset event; both threads want one. */
struct EventHandle {
	HANDLE h = nullptr;

	bool create() {
		h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		return h != nullptr;
	}

	~EventHandle() {
		if (h)
			CloseHandle(h);
	}

	EventHandle() = default;
	EventHandle(const EventHandle&) = delete;
	EventHandle& operator=(const EventHandle&) = delete;
};

} // namespace


Engine::~Engine() {
	stop();
}


void Engine::setReady(Ready& ready, bool ok, std::wstring err) {
	{
		std::lock_guard<std::mutex> lock(ready.mutex);
		ready.ok = ok;
		ready.err = std::move(err);
		ready.done = true;
	}
	ready.cv.notify_all();
}


bool Engine::waitReady(Ready& ready, std::wstring& err) {
	std::unique_lock<std::mutex> lock(ready.mutex);
	// A device that never answers must not hang the UI thread forever. Ten
	// seconds is far beyond any healthy endpoint open.
	const bool signalled = ready.cv.wait_for(lock, std::chrono::seconds(10),
	                                         [&] { return ready.done; });
	if (!signalled) {
		err = L"The audio device did not respond within 10 seconds.";
		return false;
	}
	err = ready.err;
	return ready.ok;
}


void Engine::raiseAsyncError(std::wstring message) {
	std::lock_guard<std::mutex> lock(errorMutex_);
	if (asyncError_.empty())
		asyncError_ = std::move(message);
}


std::wstring Engine::takeAsyncError() {
	std::lock_guard<std::mutex> lock(errorMutex_);
	std::wstring out = std::move(asyncError_);
	asyncError_.clear();
	return out;
}


bool Engine::start(const EngineConfig& cfg, std::wstring& err) {
	stop();

	quit_.store(false, std::memory_order_release);
	underruns_.store(0, std::memory_order_relaxed);
	overruns_.store(0, std::memory_order_relaxed);
	inPeakL_.store(0.f, std::memory_order_relaxed);
	inPeakR_.store(0.f, std::memory_order_relaxed);
	outPeak_.store(0.f, std::memory_order_relaxed);
	midPeak_.store(0.f, std::memory_order_relaxed);
	sidePeak_.store(0.f, std::memory_order_relaxed);
	correlation_.store(0.f, std::memory_order_relaxed);
	minLimiterGain_.store(1.f, std::memory_order_relaxed);
	chainLatencyFrames_.store(0, std::memory_order_relaxed);
	ringTargetFrames_.store(0.0, std::memory_order_relaxed);
	fileEnded_.store(false, std::memory_order_relaxed);
	loopFile_.store(cfg.loopFile, std::memory_order_relaxed);
	paused_.store(false, std::memory_order_relaxed);
	// filePosition_ is deliberately left alone: a scrub while stopped is how
	// you choose where Play begins. Only the pending seek is dropped, since
	// seekTo has already folded it into filePosition_ for us.
	seekRequest_.store(-1, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(errorMutex_);
		asyncError_.clear();
	}

	if (cfg.fromFile && file_.frames() == 0) {
		err = L"No audio file is loaded.";
		return false;
	}

	// Capture first: it sizes the ring and establishes the source rate, both of
	// which the render thread needs before it can set its resampling ratio.
	// A file source stands in for it and honours the same contract.
	Ready captureReady;
	capture_ = std::thread(cfg.fromFile ? &Engine::fileThread : &Engine::captureThread,
	                       this, cfg, &captureReady);
	if (!waitReady(captureReady, err)) {
		quit_.store(true, std::memory_order_release);
		if (capture_.joinable())
			capture_.join();
		return false;
	}

	Ready renderReady;
	render_ = std::thread(&Engine::renderThread, this, cfg, &renderReady);
	if (!waitReady(renderReady, err)) {
		quit_.store(true, std::memory_order_release);
		if (render_.joinable())
			render_.join();
		if (capture_.joinable())
			capture_.join();
		return false;
	}

	running_.store(true, std::memory_order_release);
	return true;
}


void Engine::stop() {
	quit_.store(true, std::memory_order_release);
	if (render_.joinable())
		render_.join();
	if (capture_.joinable())
		capture_.join();
	running_.store(false, std::memory_order_release);
}


EngineStats Engine::stats() {
	EngineStats s;
	s.running = running();
	s.captureRate = captureRate_.load(std::memory_order_relaxed);
	s.renderRate = renderRate_.load(std::memory_order_relaxed);
	s.captureChannels = captureChannels_.load(std::memory_order_relaxed);
	s.renderChannels = renderChannels_.load(std::memory_order_relaxed);
	s.underruns = underruns_.load(std::memory_order_relaxed);
	s.overruns = overruns_.load(std::memory_order_relaxed);
	s.ratio = ratio_.load(std::memory_order_relaxed);

	// Peaks are read-and-clear, so the GUI sees the loudest sample since it
	// last looked rather than whatever happened to be playing at this instant.
	s.inPeakL = inPeakL_.exchange(0.f, std::memory_order_relaxed);
	s.inPeakR = inPeakR_.exchange(0.f, std::memory_order_relaxed);
	s.outPeak = outPeak_.exchange(0.f, std::memory_order_relaxed);
	s.midPeak = midPeak_.exchange(0.f, std::memory_order_relaxed);
	s.sidePeak = sidePeak_.exchange(0.f, std::memory_order_relaxed);
	s.correlation = correlation_.load(std::memory_order_relaxed);
	s.gainReductionDb = gainToDb(minLimiterGain_.exchange(1.f, std::memory_order_relaxed));

	s.fileEnded = fileEnded_.load(std::memory_order_relaxed);
	if (file_.sampleRate > 0) {
		s.filePositionSeconds = double(filePosition_.load(std::memory_order_relaxed))
		                      / double(file_.sampleRate);
	}

	if (s.captureRate > 0) {
		s.ringMs = 1000.0 * double(ring_.availableApprox()) / double(s.captureRate);
		s.roundTripMs = s.ringMs;
	}
	if (s.captureRate > 0) {
		s.capturePeriodMs = 1000.0 * double(capturePeriodFrames_.load(std::memory_order_relaxed))
		                  / double(s.captureRate);
	}
	if (s.renderRate > 0) {
		s.roundTripMs += 1000.0 * double(renderBufferFrames_.load(std::memory_order_relaxed))
		               / double(s.renderRate);
		s.renderPeriodMs = 1000.0 * double(renderPeriodFrames_.load(std::memory_order_relaxed))
		                 / double(s.renderRate);
		// The chain's own delay is part of the round trip, not a footnote to
		// it: at the shipping settings the linear-phase Hilbert and the
		// limiter's look-ahead together are a sixth of the total.
		s.processingMs = processingLatencyMs();
		s.roundTripMs += s.processingMs;
	}
	return s;
}


double Engine::processingLatencyMs() const {
	const uint32_t rate = renderRate_.load(std::memory_order_relaxed);
	if (rate == 0)
		return 0.0;
	return 1000.0 * double(chainLatencyFrames_.load(std::memory_order_relaxed)) / double(rate);
}


bool Engine::loadFile(const std::wstring& path, std::wstring& err) {
	if (running()) {
		err = L"Stop playback before loading another file.";
		return false;
	}

	AudioBuffer loaded;
	std::wstring message;
	bool ok = false;

	// Media Foundation wants the multithreaded apartment and the GUI thread is
	// deliberately an STA, so the decode gets a thread of its own. Joining it
	// immediately keeps loadFile synchronous, which is what the caller wants:
	// the file is either there and describable or it is not.
	std::thread worker([&] {
		ComScope com;
		ok = loadAudioFile(path, loaded, message);
	});
	worker.join();

	err = message;
	if (!ok)
		return false;

	file_ = std::move(loaded);
	filePath_ = path;
	filePosition_.store(0, std::memory_order_relaxed);
	fileEnded_.store(false, std::memory_order_relaxed);
	return true;
}


void Engine::seekTo(double seconds) {
	const size_t frames = file_.frames();
	if (frames == 0 || file_.sampleRate == 0)
		return;

	double frame = seconds * double(file_.sampleRate);
	if (frame < 0.0)
		frame = 0.0;
	if (frame > double(frames - 1))
		frame = double(frames - 1);
	const size_t target = size_t(frame);

	// Exactly one writer for filePosition_ at any moment: the file thread owns
	// it while running and publishes where it has actually got to, so writing
	// it from here as well would let a store from mid-fill land after this one
	// and jerk the readout backwards for a frame.
	if (running())
		seekRequest_.store(int64_t(target), std::memory_order_release);
	else
		filePosition_.store(target, std::memory_order_relaxed);
}


Engine::FileInfo Engine::fileInfo() const {
	FileInfo info;
	info.path = filePath_;
	info.name = media::fileNameOf(filePath_);
	info.sampleRate = file_.sampleRate;
	info.frames = file_.frames();
	info.seconds = file_.seconds();
	info.loaded = info.frames > 0;
	return info;
}


void Engine::fileThread(EngineConfig cfg, Ready* readyPtr) {
	Ready& ready = *readyPtr;

	// No COM and no device: the file is already decoded, and all this thread
	// does is meter it and keep the ring topped up. It exists as a thread at
	// all so that the render side cannot tell a file from a cable.
	const AudioBuffer& file = file_;
	const size_t frames = file.frames();
	const uint32_t rate = file.sampleRate;
	if (frames == 0 || rate == 0) {
		setReady(ready, false, L"No audio file is loaded.");
		return;
	}

	ring_.reset(std::max<size_t>(rate, 4096));
	ring_.clear();

	// The render thread sizes the ring's floor from the capture period. A file
	// has no device period, so it is told the one this thread actually refills
	// at -- 10 ms, which is the Windows engine default and comfortably more
	// than the 2 ms sleep below.
	capturePeriodFrames_.store(std::max<uint32_t>(rate / 100, 1), std::memory_order_relaxed);
	captureRate_.store(rate, std::memory_order_relaxed);
	captureChannels_.store(2, std::memory_order_relaxed);

	setReady(ready, true, std::wstring());

	// 2048 frames is a comfortable bite at any rate and keeps the scratch in
	// cache; the loop below takes as many bites as the ring has room for.
	std::vector<float> scratch(2048 * 2, 0.f);
	// Wherever the position was left: zero after a load, or wherever the user
	// scrubbed to before pressing Play.
	size_t pos = std::min(filePosition_.load(std::memory_order_relaxed), frames - 1);
	bool ended = false;          // the file ran out and looping is off
	size_t silenceAfterEnd = 0;  // frames of tail pushed since then

	ProAudioPriority priority;

	while (!quit_.load(std::memory_order_acquire)) {
		// A scrub lands here. Taking it at the top of a fill means the frames
		// already queued play out first and the jump is heard one ring later,
		// which is the price of not reaching into the consumer's end.
		const int64_t seek = seekRequest_.exchange(-1, std::memory_order_acquire);
		if (seek >= 0) {
			pos = std::min(size_t(seek), frames - 1);
			// Scrubbing back into the file un-ends it, or a seek after the last
			// pass would land on a player that has already given up.
			ended = false;
			silenceAfterEnd = 0;
			fileEnded_.store(false, std::memory_order_relaxed);
		}

		const bool paused = paused_.load(std::memory_order_relaxed);

		// Aim at exactly the fill the render thread is holding for, so its
		// drift controller -- which is frozen in file mode anyway, there being
		// no second clock to drift against -- sees the number it expects.
		double target = ringTargetFrames_.load(std::memory_order_relaxed);
		if (target <= 0.0)
			target = cfg.targetBufferMs * 0.001 * double(rate);

		const size_t available = ring_.availableRead();
		size_t room = (available < size_t(target)) ? size_t(target) - available : 0;
		room = std::min(room, ring_.availableWrite());

		while (room > 0) {
			const size_t chunk = std::min(room, scratch.size() / 2);
			float peakL = 0.f, peakR = 0.f;
			size_t n = 0;

			for (; n < chunk && !paused; n++) {
				if (pos >= frames) {
					if (!loopFile_.load(std::memory_order_relaxed)) {
						ended = true;
						break;
					}
					pos = 0;
				}
				const float l = file.l[pos];
				const float r = file.r[pos];
				scratch[n * 2] = l;
				scratch[n * 2 + 1] = r;
				peakL = std::max(peakL, std::abs(l));
				peakR = std::max(peakR, std::abs(r));
				pos++;
			}
			// A file that is paused or has run out still has to hand over
			// frames, or the render side would underrun and stutter where it
			// should simply go quiet.
			for (size_t i = n; i < chunk; i++) {
				scratch[i * 2] = 0.f;
				scratch[i * 2 + 1] = 0.f;
			}
			if (ended)
				silenceAfterEnd += chunk - n;

			// Same read-and-clear peak publication the capture thread uses, so
			// the IN meters read the same way whatever the source is.
			float prev = inPeakL_.load(std::memory_order_relaxed);
			while (peakL > prev
			       && !inPeakL_.compare_exchange_weak(prev, peakL, std::memory_order_relaxed))
				;
			prev = inPeakR_.load(std::memory_order_relaxed);
			while (peakR > prev
			       && !inPeakR_.compare_exchange_weak(prev, peakR, std::memory_order_relaxed))
				;

			room -= ring_.push(scratch.data(), chunk);
		}

		filePosition_.store(pos, std::memory_order_relaxed);

		// Announce the end only once the tail has had time to reach the
		// speakers: the ring, the chain's look-ahead and the endpoint buffer
		// all sit between here and the last audible sample, and stopping the
		// moment the file pointer runs out would clip it.
		if (ended && silenceAfterEnd >= size_t(rate) / 2)
			fileEnded_.store(true, std::memory_order_release);

		Sleep(2);
	}
}


void Engine::captureThread(EngineConfig cfg, Ready* readyPtr) {
	Ready& ready = *readyPtr;
	ComScope com;

	std::wstring err;
	Com<IMMDevice> device;
	// In loopback mode the source is a *render* endpoint whose output we tap.
	const EDataFlow flow = cfg.loopback ? eRender : eCapture;
	if (!openDevice(cfg.captureId, flow, device, err)) {
		setReady(ready, false, err);
		return;
	}

	Com<IAudioClient> client;
	HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid());
	if (FAILED(hr)) {
		setReady(ready, false, describeHresult(L"Activating the capture device", hr));
		return;
	}

	MixFormat mix;
	hr = client->GetMixFormat(&mix.wf);
	if (FAILED(hr)) {
		setReady(ready, false, describeHresult(L"Reading the capture mix format", hr));
		return;
	}

	SampleFormat fmt;
	if (!describeFormat(mix.wf, fmt)) {
		setReady(ready, false,
		                 L"The capture device uses a sample format this app does not handle "
		                 L"(only 16/24/32-bit PCM and 32-bit float are supported).");
		return;
	}

	// A loopback stream is driven by whatever is playing on the render device,
	// not by a capture clock, so its event handle is not reliably signalled --
	// Microsoft's own guidance is to poll it. Real capture endpoints get the
	// event-driven path, which is both lower latency and cheaper.
	DWORD flags = cfg.loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK
	                           : AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
	// Polling needs a buffer that comfortably outlives the poll interval;
	// event-driven takes the engine default by passing 0.
	const REFERENCE_TIME duration = cfg.loopback ? (100 * kHnsPerMs) : 0;

	hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, mix.wf, nullptr);
	if (FAILED(hr)) {
		setReady(ready, false, describeHresult(L"Opening the capture stream", hr));
		return;
	}

	UINT32 bufferFrames = 0;
	if (FAILED(client->GetBufferSize(&bufferFrames)))
		bufferFrames = fmt.sampleRate / 20;  // 50 ms is a safe stand-in

	// The period, not the buffer size, is what bounds capture jitter: an
	// event-driven stream is signalled once per period and hands over one
	// period of audio. GetBufferSize is typically several periods, and sizing
	// the ring target from it costs tens of milliseconds for nothing.
	{
		REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
		uint32_t periodFrames = 0;
		if (SUCCEEDED(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod))) {
			periodFrames = uint32_t(double(defaultPeriod) * double(fmt.sampleRate)
			                        / 10000000.0 + 0.5);
		}
		if (periodFrames == 0)
			periodFrames = fmt.sampleRate / 100;  // 10 ms, the engine default
		capturePeriodFrames_.store(periodFrames, std::memory_order_relaxed);
	}

	EventHandle evt;
	if (!cfg.loopback) {
		if (!evt.create()) {
			setReady(ready, false, L"Could not create the capture event.");
			return;
		}
		hr = client->SetEventHandle(evt.h);
		if (FAILED(hr)) {
			setReady(ready, false, describeHresult(L"Attaching the capture event", hr));
			return;
		}
	}

	Com<IAudioCaptureClient> captureClient;
	hr = client->GetService(__uuidof(IAudioCaptureClient), captureClient.putVoid());
	if (FAILED(hr)) {
		setReady(ready, false, describeHresult(L"Getting the capture service", hr));
		return;
	}

	// One second of ring, so a scheduling hiccup on either side has somewhere
	// to go. Only targetBufferMs of it is used in steady state.
	ring_.reset(std::max<size_t>(fmt.sampleRate, 4096));
	ring_.clear();

	// Sized to the whole endpoint buffer, so a packet never needs chunking.
	std::vector<float> scratch(size_t(bufferFrames) * 2 + 512, 0.f);

	captureRate_.store(fmt.sampleRate, std::memory_order_relaxed);
	captureChannels_.store(fmt.channels, std::memory_order_relaxed);

	hr = client->Start();
	if (FAILED(hr)) {
		setReady(ready, false, describeHresult(L"Starting the capture stream", hr));
		return;
	}

	setReady(ready, true, std::wstring());

	{
		ProAudioPriority priority;

		while (!quit_.load(std::memory_order_acquire)) {
			if (evt.h)
				WaitForSingleObject(evt.h, 200);
			else
				Sleep(2);  // loopback: poll well inside the 100 ms buffer

			for (;;) {
				UINT32 packet = 0;
				hr = captureClient->GetNextPacketSize(&packet);
				if (FAILED(hr)) {
					raiseAsyncError(describeHresult(L"Reading from the capture device", hr));
					quit_.store(true, std::memory_order_release);
					break;
				}
				if (packet == 0)
					break;

				BYTE* data = nullptr;
				UINT32 frames = 0;
				DWORD bufferFlags = 0;
				hr = captureClient->GetBuffer(&data, &frames, &bufferFlags, nullptr, nullptr);
				if (hr == AUDCLNT_S_BUFFER_EMPTY)
					break;
				if (FAILED(hr)) {
					raiseAsyncError(describeHresult(L"Reading from the capture device", hr));
					quit_.store(true, std::memory_order_release);
					break;
				}

				if (frames > 0) {
					size_t pushed = 0;
					if (bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
						// The endpoint says "this packet is silence" and may not
						// have bothered to zero the memory. Believe the flag.
						pushed = ring_.pushSilence(frames);
					}
					else {
						const size_t chunk = std::min<size_t>(frames, scratch.size() / 2);
						toStereoFloat(fmt, data, chunk, scratch.data());

						float peakL = 0.f, peakR = 0.f;
						for (size_t i = 0; i < chunk; i++) {
							peakL = std::max(peakL, std::abs(scratch[i * 2]));
							peakR = std::max(peakR, std::abs(scratch[i * 2 + 1]));
						}
						// Publish the max since the GUI last cleared it.
						float prev = inPeakL_.load(std::memory_order_relaxed);
						while (peakL > prev
						       && !inPeakL_.compare_exchange_weak(prev, peakL, std::memory_order_relaxed))
							;
						prev = inPeakR_.load(std::memory_order_relaxed);
						while (peakR > prev
						       && !inPeakR_.compare_exchange_weak(prev, peakR, std::memory_order_relaxed))
							;

						pushed = ring_.push(scratch.data(), chunk);
					}

					if (pushed < frames)
						overruns_.fetch_add(1, std::memory_order_relaxed);
				}

				captureClient->ReleaseBuffer(frames);
			}
		}
	}

	client->Stop();
}


void Engine::renderThread(EngineConfig cfg, Ready* readyPtr) {
	Ready& ready = *readyPtr;
	ComScope com;

	std::wstring err;
	Com<IMMDevice> device;
	if (!openDevice(cfg.renderId, eRender, device, err)) {
		setReady(ready, false, err);
		return;
	}

	Com<IAudioClient> client;
	HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid());
	if (FAILED(hr)) {
		setReady(ready, false, describeHresult(L"Activating the playback device", hr));
		return;
	}

	MixFormat mix;
	hr = client->GetMixFormat(&mix.wf);
	if (FAILED(hr)) {
		setReady(ready, false, describeHresult(L"Reading the playback mix format", hr));
		return;
	}

	SampleFormat fmt;
	if (!describeFormat(mix.wf, fmt)) {
		setReady(ready, false,
		                 L"The playback device uses a sample format this app does not handle "
		                 L"(only 16/24/32-bit PCM and 32-bit float are supported).");
		return;
	}

	// IAudioClient3 lets a shared stream run at the driver's minimum period
	// instead of the 10 ms default -- typically 3 ms or less, and it is the
	// single biggest latency win available without going exclusive-mode. It is
	// not present or not permitted everywhere, hence the fallback.
	bool initialised = false;
	uint32_t periodFrames = 0;
	{
		Com<IAudioClient3> client3;
		if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient3), client3.putVoid()))) {
			UINT32 defaultPeriod = 0, fundamental = 0, minPeriod = 0, maxPeriod = 0;
			if (SUCCEEDED(client3->GetSharedModeEnginePeriod(
			        mix.wf, &defaultPeriod, &fundamental, &minPeriod, &maxPeriod))) {
				hr = client3->InitializeSharedAudioStream(
				    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minPeriod, mix.wf, nullptr);
				initialised = SUCCEEDED(hr);
				if (initialised)
					periodFrames = minPeriod;
			}
		}
	}
	if (!initialised) {
		hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
		                        0, 0, mix.wf, nullptr);
		if (FAILED(hr)) {
			setReady(ready, false, describeHresult(L"Opening the playback stream", hr));
			return;
		}
	}
	if (periodFrames == 0) {
		REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
		if (SUCCEEDED(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod))) {
			periodFrames = uint32_t(double(defaultPeriod) * double(fmt.sampleRate)
			                        / 10000000.0 + 0.5);
		}
		if (periodFrames == 0)
			periodFrames = fmt.sampleRate / 100;
	}
	renderPeriodFrames_.store(periodFrames, std::memory_order_relaxed);

	UINT32 bufferFrames = 0;
	hr = client->GetBufferSize(&bufferFrames);
	if (FAILED(hr) || bufferFrames == 0) {
		setReady(ready, false, describeHresult(L"Reading the playback buffer size", hr));
		return;
	}

	EventHandle evt;
	if (!evt.create()) {
		setReady(ready, false, L"Could not create the playback event.");
		return;
	}
	hr = client->SetEventHandle(evt.h);
	if (FAILED(hr)) {
		setReady(ready, false, describeHresult(L"Attaching the playback event", hr));
		return;
	}

	Com<IAudioRenderClient> renderClient;
	hr = client->GetService(__uuidof(IAudioRenderClient), renderClient.putVoid());
	if (FAILED(hr)) {
		setReady(ready, false, describeHresult(L"Getting the playback service", hr));
		return;
	}

	const uint32_t captureRate = captureRate_.load(std::memory_order_acquire);
	if (captureRate == 0) {
		setReady(ready, false, L"The capture device never reported a sample rate.");
		return;
	}

	renderRate_.store(fmt.sampleRate, std::memory_order_relaxed);
	renderChannels_.store(fmt.channels, std::memory_order_relaxed);
	renderBufferFrames_.store(bufferFrames, std::memory_order_relaxed);

	// Target fill, in capture-rate frames. The requested figure is a floor, not
	// a ceiling: the ring must always absorb the capture side's jitter plus one
	// whole render block, or it underruns every time the two devices happen to
	// wake up in an unlucky order.
	//
	// One period each side is the true minimum: the ring has to survive from
	// the moment the render thread drains a period until the capture thread
	// wakes and refills one. Anything beyond that is chosen headroom, and it
	// belongs in targetBufferMs where the user can see it, rather than being
	// silently added here -- the earlier version budgeted the whole render
	// buffer and cost 20 ms for nothing on hardware that never needed it.
	// If this proves too tight on some device the underrun counter says so.
	const uint32_t capturePeriod = capturePeriodFrames_.load(std::memory_order_acquire);
	const double renderPeriodInCaptureFrames =
	    double(periodFrames) * double(captureRate) / double(fmt.sampleRate);
	const double floorFrames = double(capturePeriod) + renderPeriodInCaptureFrames;
	double targetFrames = cfg.targetBufferMs * 0.001 * double(captureRate);
	targetFrames = std::max(targetFrames, floorFrames);
	targetFrames = std::min(targetFrames, double(ring_.capacity()) * 0.5);
	// Only a file producer reads this, and only so it can aim at the same fill.
	ringTargetFrames_.store(targetFrames, std::memory_order_release);

	const double baseRatio = double(captureRate) / double(fmt.sampleRate);
	const double blockRateHz = double(fmt.sampleRate) / double(bufferFrames);

	chain_.prepare(double(fmt.sampleRate));
	chain_.reset();
	resampler_.reset();
	resampler_.setRatio(baseRatio);
	drift_.reset(baseRatio, targetFrames, blockRateHz);
	ratio_.store(baseRatio, std::memory_order_relaxed);

	// Hand the endpoint a full buffer of silence before starting, so the first
	// event arrives on schedule rather than into an already-starved device.
	{
		BYTE* data = nullptr;
		if (SUCCEEDED(renderClient->GetBuffer(bufferFrames, &data)))
			renderClient->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
	}

	hr = client->Start();
	if (FAILED(hr)) {
		setReady(ready, false, describeHresult(L"Starting the playback stream", hr));
		return;
	}

	setReady(ready, true, std::wstring());

	{
		ProAudioPriority priority;

		// Start primed: play silence until the ring has reached its target, so
		// the resampler never begins mid-drought. Any later underrun drops back
		// into this state, which costs one quiet moment and then recovers,
		// instead of stuttering once per block for as long as the source lags.
		bool priming = true;

		while (!quit_.load(std::memory_order_acquire)) {
			const DWORD waited = WaitForSingleObject(evt.h, 200);
			if (waited == WAIT_TIMEOUT)
				continue;
			if (waited != WAIT_OBJECT_0)
				break;

			UINT32 padding = 0;
			hr = client->GetCurrentPadding(&padding);
			if (FAILED(hr)) {
				raiseAsyncError(describeHresult(L"Writing to the playback device", hr));
				break;
			}
			if (padding >= bufferFrames)
				continue;
			const UINT32 frames = bufferFrames - padding;

			BYTE* data = nullptr;
			hr = renderClient->GetBuffer(frames, &data);
			if (FAILED(hr)) {
				raiseAsyncError(describeHresult(L"Writing to the playback device", hr));
				break;
			}

			const RealMonoSettings settings = params.snapshot();
			float blockPeak = 0.f;
			float blockMid = 0.f, blockSide = 0.f;
			float minGain = 1.f;
			double sumLR = 0.0, sumLL = 0.0, sumRR = 0.0;

			for (UINT32 i = 0; i < frames; i++) {
				float l = 0.f, r = 0.f;

				if (priming) {
					if (ring_.availableRead() >= size_t(targetFrames)) {
						priming = false;
						resampler_.reset();
					}
				}
				if (!priming && !resampler_.next(ring_, l, r)) {
					priming = true;
					underruns_.fetch_add(1, std::memory_order_relaxed);
					l = r = 0.f;
				}

				// Mid and Side are metered before the chain, on the signal as
				// it arrived: what the Advanced page is asking is "how much
				// Side is in this source", not "how much survived".
				blockMid = std::max(blockMid, std::abs(0.5f * (l + r)));
				blockSide = std::max(blockSide, std::abs(0.5f * (l - r)));
				sumLR += double(l) * double(r);
				sumLL += double(l) * double(l);
				sumRR += double(r) * double(r);

				// The chain runs even while priming, on silence. Its delay
				// lines, filter state and control smoothing stay live, so
				// audio resumes into a settled chain rather than a
				// discontinuity.
				float yL = 0.f, yR = 0.f;
				chain_.process(l, r, settings, yL, yR);
				minGain = std::min(minGain, chain_.limiterGain());

				blockPeak = std::max(blockPeak, std::max(std::abs(yL), std::abs(yR)));
				writeFrontPairFrame(fmt, data + size_t(i) * fmt.frameBytes, yL, yR);
			}

			renderClient->ReleaseBuffer(frames, 0);

			float prev = outPeak_.load(std::memory_order_relaxed);
			while (blockPeak > prev
			       && !outPeak_.compare_exchange_weak(prev, blockPeak, std::memory_order_relaxed))
				;
			prev = midPeak_.load(std::memory_order_relaxed);
			while (blockMid > prev
			       && !midPeak_.compare_exchange_weak(prev, blockMid, std::memory_order_relaxed))
				;
			prev = sidePeak_.load(std::memory_order_relaxed);
			while (blockSide > prev
			       && !sidePeak_.compare_exchange_weak(prev, blockSide, std::memory_order_relaxed))
				;
			prev = minLimiterGain_.load(std::memory_order_relaxed);
			while (minGain < prev
			       && !minLimiterGain_.compare_exchange_weak(prev, minGain, std::memory_order_relaxed))
				;

			// Correlation over the block, on a silence guard: an idle stream
			// would otherwise divide zero by zero and publish a NaN into a
			// label.
			const double denom = std::sqrt(sumLL * sumRR);
			correlation_.store(denom > 1e-12 ? float(sumLR / denom) : 0.f,
			                   std::memory_order_relaxed);
			chainLatencyFrames_.store(uint32_t(chain_.latencySamples()),
			                          std::memory_order_relaxed);

			// One drift correction per block. Frozen while priming, because the
			// fill is deliberately abnormal then and feeding that to the
			// controller would wind it up against a condition it cannot fix.
			//
			// Also frozen for a file, and that is not an optimisation: drift
			// correction exists because two crystals disagree, and a file has
			// no crystal. Its ratio is exactly rate-in over rate-out, so
			// letting the controller trim it would trade a real 0 cents for up
			// to 8 cents of invented pitch error on the one source that can be
			// compared against the client's references note for note.
			if (!priming && !cfg.fromFile) {
				const double ratio = drift_.update(double(ring_.availableRead()));
				resampler_.setRatio(ratio);
				ratio_.store(ratio, std::memory_order_relaxed);
			}
		}
	}

	client->Stop();
}

} // namespace fmdr
