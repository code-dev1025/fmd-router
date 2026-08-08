#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace fmdr {

/*  Lock-free single-producer / single-consumer ring of interleaved stereo
    frames. The producer is the WASAPI capture thread, the consumer is the
    render thread; neither ever blocks the other, and neither allocates.

    Indices are free-running counters masked on use, so "full" and "empty" stay
    distinguishable without wasting a slot and without a separate count that
    both sides would have to write. Capacity is a power of two so the wrap is a
    mask rather than a modulo. */
class StereoRing {
public:
	/** Allocates room for at least `minFrames`. Not real-time safe -- call it
	    before starting the audio threads, never from one. */
	void reset(size_t minFrames) {
		size_t cap = 1;
		while (cap < minFrames)
			cap <<= 1;
		capacity_ = cap;
		mask_ = cap - 1;
		buf_.assign(cap * 2, 0.f);
		write_.store(0, std::memory_order_relaxed);
		read_.store(0, std::memory_order_relaxed);
	}

	size_t capacity() const {
		return capacity_;
	}

	/** Frames readable. Call from the consumer. */
	size_t availableRead() const {
		return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed);
	}

	/** Frames writable. Call from the producer. */
	size_t availableWrite() const {
		return capacity_ - (write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_acquire));
	}

	/** Frames readable, from a thread that is neither producer nor consumer.
	    Only ever used for the GUI's buffer-fill readout, so a torn read costs
	    nothing worse than one stale number in a label. */
	size_t availableApprox() const {
		return write_.load(std::memory_order_relaxed) - read_.load(std::memory_order_relaxed);
	}

	/** Writes up to `frames`; returns how many actually fit. A short return is
	    an overrun -- the caller decides whether to drop and count it. */
	size_t push(const float* interleaved, size_t frames) {
		const size_t w = write_.load(std::memory_order_relaxed);
		const size_t space = capacity_ - (w - read_.load(std::memory_order_acquire));
		if (frames > space)
			frames = space;
		for (size_t i = 0; i < frames; i++) {
			const size_t idx = ((w + i) & mask_) * 2;
			buf_[idx] = interleaved[i * 2];
			buf_[idx + 1] = interleaved[i * 2 + 1];
		}
		write_.store(w + frames, std::memory_order_release);
		return frames;
	}

	/** Writes `frames` of silence, for a capture packet flagged SILENT. */
	size_t pushSilence(size_t frames) {
		const size_t w = write_.load(std::memory_order_relaxed);
		const size_t space = capacity_ - (w - read_.load(std::memory_order_acquire));
		if (frames > space)
			frames = space;
		for (size_t i = 0; i < frames; i++) {
			const size_t idx = ((w + i) & mask_) * 2;
			buf_[idx] = 0.f;
			buf_[idx + 1] = 0.f;
		}
		write_.store(w + frames, std::memory_order_release);
		return frames;
	}

	/** Consumer side. Returns false when the ring is empty (an underrun). */
	bool popFrame(float& l, float& r) {
		const size_t rd = read_.load(std::memory_order_relaxed);
		if (rd == write_.load(std::memory_order_acquire))
			return false;
		const size_t idx = (rd & mask_) * 2;
		l = buf_[idx];
		r = buf_[idx + 1];
		read_.store(rd + 1, std::memory_order_release);
		return true;
	}

	/** Drops everything queued. Consumer side only, and only while the
	    producer is stopped or the frames being dropped are already stale. */
	void clear() {
		read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
	}

private:
	std::vector<float> buf_;  // two floats per frame, interleaved L R
	size_t capacity_ = 0;     // frames, always a power of two
	size_t mask_ = 0;
	std::atomic<size_t> write_{0};
	std::atomic<size_t> read_{0};
};

} // namespace fmdr
