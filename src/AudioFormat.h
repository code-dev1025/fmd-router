#pragma once

#include <windows.h>
#include <mmreg.h>

#include <cstdint>

namespace fmdr {

/*  WASAPI hands you whatever the endpoint's mix format happens to be. In shared
    mode that is nearly always 32-bit float, but "nearly always" is not a thing
    to build a real-time thread on, so the integer widths are handled too.

    Channel layout: audio is read as stereo and written as mono-to-front-pair.
    For sources with more than two channels only the first two are taken, which
    are front L/R in every WAVE layout -- browsers emit stereo and Windows
    places stereo in the front pair, so this is the correct read in practice
    rather than a shortcut. It does mean a genuine 5.1 source loses its
    surrounds; that is documented in the README, not silently swallowed. */

/*  KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT, written out rather than included.
    Their definitions move between ks.h, ksmedia.h and mmreg.h depending on
    which of those a translation unit happens to have reached first, and the
    values have been fixed since WAVEFORMATEXTENSIBLE was introduced. */
inline const GUID kSubtypePcm =
    {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline const GUID kSubtypeIeeeFloat =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};


struct SampleFormat {
	enum Kind {
		Unsupported = 0,
		Float32,
		Int16,
		Int24,
		Int32,
	};

	Kind kind = Unsupported;
	uint32_t channels = 0;
	uint32_t sampleRate = 0;
	uint32_t frameBytes = 0;  // blockAlign

	bool valid() const {
		return kind != Unsupported && channels > 0 && sampleRate > 0 && frameBytes > 0;
	}
};


/** Reads a WAVEFORMATEX (or EXTENSIBLE) into something the converters can use.
    Returns false for formats this app will not pretend to handle. */
inline bool describeFormat(const WAVEFORMATEX* wf, SampleFormat& out) {
	if (!wf)
		return false;

	out = SampleFormat();
	out.channels = wf->nChannels;
	out.sampleRate = wf->nSamplesPerSec;
	out.frameBytes = wf->nBlockAlign;

	WORD tag = wf->wFormatTag;
	WORD bits = wf->wBitsPerSample;
	WORD validBits = bits;

	if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
		const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
		validBits = ext->Samples.wValidBitsPerSample;
		if (validBits == 0 || validBits > bits)
			validBits = bits;
		if (IsEqualGUID(ext->SubFormat, kSubtypeIeeeFloat))
			tag = WAVE_FORMAT_IEEE_FLOAT;
		else if (IsEqualGUID(ext->SubFormat, kSubtypePcm))
			tag = WAVE_FORMAT_PCM;
		else
			return false;
	}

	if (tag == WAVE_FORMAT_IEEE_FLOAT) {
		if (bits != 32)
			return false;  // 64-bit float endpoints do not exist in practice
		out.kind = SampleFormat::Float32;
	}
	else if (tag == WAVE_FORMAT_PCM) {
		switch (bits) {
			case 16: out.kind = SampleFormat::Int16; break;
			case 24: out.kind = SampleFormat::Int24; break;
			// 32-bit containers carrying 24 valid bits still scale by the full
			// container width, because the low bits are zero either way.
			case 32: out.kind = SampleFormat::Int32; break;
			default: return false;
		}
	}
	else {
		return false;
	}

	return out.valid();
}


/** One frame's worth of source samples -> a single float, for channel `ch`. */
inline float readSample(const SampleFormat& f, const uint8_t* frame, uint32_t ch) {
	switch (f.kind) {
		case SampleFormat::Float32:
			return reinterpret_cast<const float*>(frame)[ch];

		case SampleFormat::Int16:
			return float(reinterpret_cast<const int16_t*>(frame)[ch]) * (1.f / 32768.f);

		case SampleFormat::Int24: {
			const uint8_t* p = frame + ch * 3;
			// Little-endian packed 24-bit, sign-extended into an int32.
			int32_t v = (int32_t(p[0]) << 8) | (int32_t(p[1]) << 16) | (int32_t(p[2]) << 24);
			return float(v) * (1.f / 2147483648.f);
		}

		case SampleFormat::Int32:
			return float(reinterpret_cast<const int32_t*>(frame)[ch]) * (1.f / 2147483648.f);

		default:
			return 0.f;
	}
}


/** Source block -> interleaved stereo float. `dst` holds frames*2 floats. */
inline void toStereoFloat(const SampleFormat& f, const uint8_t* src, size_t frames, float* dst) {
	const uint32_t right = (f.channels > 1) ? 1u : 0u;  // mono sources double up
	for (size_t i = 0; i < frames; i++) {
		const uint8_t* frame = src + i * f.frameBytes;
		dst[i * 2] = readSample(f, frame, 0);
		dst[i * 2 + 1] = readSample(f, frame, right);
	}
}


/** Writes one float to channel `ch` of a destination frame. */
inline void writeSample(const SampleFormat& f, uint8_t* frame, uint32_t ch, float v) {
	// Clamp before conversion: an integer endpoint wraps on overflow, which is
	// a full-scale click, and even a float endpoint can upset a downstream APO.
	if (v > 1.f) v = 1.f;
	if (v < -1.f) v = -1.f;

	switch (f.kind) {
		case SampleFormat::Float32:
			reinterpret_cast<float*>(frame)[ch] = v;
			break;

		case SampleFormat::Int16:
			reinterpret_cast<int16_t*>(frame)[ch] = int16_t(v * 32767.f);
			break;

		case SampleFormat::Int24: {
			const int32_t s = int32_t(v * 8388607.f);
			uint8_t* p = frame + ch * 3;
			p[0] = uint8_t(s & 0xff);
			p[1] = uint8_t((s >> 8) & 0xff);
			p[2] = uint8_t((s >> 16) & 0xff);
			break;
		}

		case SampleFormat::Int32:
			reinterpret_cast<int32_t*>(frame)[ch] = int32_t(double(v) * 2147483647.0);
			break;

		default:
			break;
	}
}


/** A stereo pair -> one destination frame, written to the front pair. In
    normal operation the chain hands over dual mono (l == r), which is what a
    multi-room system wants: every speaker gets the same coherent feed. It is
    still written as a pair rather than as one value because global bypass is a
    genuine stereo pass-through, and because bypassing the mono commit leaves
    the chain in its M/S domain on purpose.

    Channels beyond the front pair are zeroed rather than fed, to keep a centre
    speaker or LFE out of it. */
inline void writeFrontPairFrame(const SampleFormat& f, uint8_t* frame, float l, float r) {
	if (f.channels == 1) {
		// A mono endpoint gets the sum of the pair, which for dual mono is
		// simply the signal back again.
		writeSample(f, frame, 0, 0.5f * (l + r));
		return;
	}
	writeSample(f, frame, 0, l);
	writeSample(f, frame, 1, r);
	for (uint32_t ch = 2; ch < f.channels; ch++)
		writeSample(f, frame, ch, 0.f);
}

} // namespace fmdr
