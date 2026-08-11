#pragma once

/*  Compressed audio in, floats out, using Media Foundation.

    The app needed a way to hear a file through the chain without a virtual
    cable, a second endpoint and something else playing -- "does this actually
    sound right" should not require the whole routing rig to be standing. WAV
    alone would have been enough to build that on, but the material anyone
    reaches for is an MP3, so the decoder question had to be answered.

    It is answered with Media Foundation rather than a vendored decoder,
    because the codecs are already on the machine: mp3, m4a/AAC and wma always,
    FLAC since Windows 10, and Ogg Vorbis wherever the Web Media Extensions
    package is present (it ships with Windows 10 1809 and later). That keeps
    the "no third-party dependency" line in CMakeLists.txt true and keeps a
    megabyte of someone else's parser out of the tree. The cost is that this
    header is Windows-only, which the app already is -- the offline tool stays
    on AudioFile.h and stays portable.

    Everything is decoded to 32-bit float at the file's own sample rate. The
    engine resamples to the endpoint anyway, so converting rate here would be
    one more resampler in the path for nothing. */

#include "AudioFile.h"
#include "Com.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <string>

namespace fmdr {

/*  A ceiling on what will be loaded. The player holds the whole decoded file
    in memory -- which is what makes looping and instant restart free -- so the
    limit is really a limit on how much RAM a mis-click can cost. Ten minutes
    of stereo float is about 230 MB, and anything longer is a mastering job for
    realmono-wav rather than something to audition by ear. */
constexpr double kMaxFileSeconds = 600.0;

/*  The stream selectors are enumerators MSVC types as signed, and every call
    that takes one wants a DWORD. Named once here rather than warned about five
    times at /W4. */
constexpr DWORD kMfAllStreams = DWORD(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kMfFirstAudioStream = DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM);


/** RAII for MFStartup. Must be created on the thread that will use MF, and
    that thread should be in the MTA -- see Engine::loadFile. */
class MediaFoundationScope {
public:
	MediaFoundationScope() {
		ok_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
	}

	~MediaFoundationScope() {
		if (ok_)
			MFShutdown();
	}

	bool ok() const {
		return ok_;
	}

	MediaFoundationScope(const MediaFoundationScope&) = delete;
	MediaFoundationScope& operator=(const MediaFoundationScope&) = delete;

private:
	bool ok_ = false;
};


namespace media {

/** The lower-cased extension including the dot, or empty. */
inline std::wstring extensionOf(const std::wstring& path) {
	const size_t dot = path.find_last_of(L'.');
	const size_t slash = path.find_last_of(L"\\/");
	if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
		return std::wstring();
	std::wstring ext = path.substr(dot);
	std::transform(ext.begin(), ext.end(), ext.begin(),
	               [](wchar_t c) { return wchar_t(std::towlower(c)); });
	return ext;
}

/** The part after the last separator, for labelling the UI. */
inline std::wstring fileNameOf(const std::wstring& path) {
	const size_t slash = path.find_last_of(L"\\/");
	return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

/** "no codec for this" is the one failure worth explaining, because the fix is
    something the user can actually do. */
inline bool isMissingCodec(HRESULT hr) {
	return hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE
	    || hr == MF_E_UNSUPPORTED_SCHEME
	    || hr == MF_E_INVALIDMEDIATYPE
	    || hr == MF_E_TOPO_CODEC_NOT_FOUND;
}

inline std::wstring codecAdvice(const std::wstring& ext) {
	if (ext == L".ogg" || ext == L".oga") {
		return L"Windows has no Ogg Vorbis decoder here. Install \"Web Media Extensions\" "
		       L"from the Microsoft Store, or convert the file first:\n\n"
		       L"    ffmpeg -i in.ogg -c:a pcm_s24le test.wav";
	}
	return L"Windows has no decoder installed for this file type. Convert it to WAV "
	       L"first:\n\n    ffmpeg -i in" + ext + L" -c:a pcm_s24le test.wav";
}

} // namespace media


/** Decodes any format Media Foundation can open. `err` is ready to show. */
inline bool readMediaFile(const std::wstring& path, AudioBuffer& audio, std::wstring& err) {
	MediaFoundationScope mf;
	if (!mf.ok()) {
		err = L"Media Foundation could not be started, so only WAV files can be played.";
		return false;
	}

	const std::wstring ext = media::extensionOf(path);

	Com<IMFSourceReader> reader;
	HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, reader.put());
	if (FAILED(hr)) {
		if (media::isMissingCodec(hr))
			err = media::codecAdvice(ext);
		else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
		         || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
			// It was there when the dialog listed it. A raw 0x80070002 for
			// "somebody moved it" helps nobody.
			err = L"That file is no longer there.";
		else if (hr == E_ACCESSDENIED)
			err = L"That file could not be opened \x2014 access denied.";
		else
			err = describeHresult(L"Opening the file", hr);
		return false;
	}

	// Audio only, and only the first audio stream: a video file dropped in here
	// should play its soundtrack rather than fail.
	reader->SetStreamSelection(kMfAllStreams, FALSE);
	hr = reader->SetStreamSelection(kMfFirstAudioStream, TRUE);
	if (FAILED(hr)) {
		err = L"That file has no audio stream Windows can read.";
		return false;
	}

	// A partial type -- major and subtype only. The source reader fills in the
	// rest and inserts the decoder and any converter it needs.
	Com<IMFMediaType> wanted;
	hr = MFCreateMediaType(wanted.put());
	if (SUCCEEDED(hr))
		hr = wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	if (SUCCEEDED(hr))
		hr = wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
	if (SUCCEEDED(hr)) {
		hr = reader->SetCurrentMediaType(kMfFirstAudioStream, nullptr, wanted.get());
	}
	if (FAILED(hr)) {
		err = media::isMissingCodec(hr)
		    ? media::codecAdvice(ext)
		    : describeHresult(L"Setting up the decoder", hr);
		return false;
	}

	// What the reader settled on. Rate and channel count are not knowable
	// before this point -- they come out of the file, not out of the request.
	uint32_t rate = 0, channels = 0;
	auto readFormat = [&]() -> bool {
		Com<IMFMediaType> actual;
		if (FAILED(reader->GetCurrentMediaType(kMfFirstAudioStream, actual.put())))
			return false;
		return SUCCEEDED(actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate))
		    && SUCCEEDED(actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels))
		    && rate > 0 && channels > 0;
	};
	if (!readFormat()) {
		err = L"The decoder did not report a sample rate and channel count.";
		return false;
	}

	audio.sampleRate = rate;
	audio.bits = 32;
	audio.isFloat = true;
	audio.l.clear();
	audio.r.clear();

	const size_t maxFrames = size_t(kMaxFileSeconds * double(rate));
	bool truncated = false;

	for (;;) {
		DWORD flags = 0;
		Com<IMFSample> sample;
		hr = reader->ReadSample(kMfFirstAudioStream, 0, nullptr, &flags,
		                        nullptr, sample.put());
		if (FAILED(hr)) {
			err = describeHresult(L"Decoding the file", hr);
			return false;
		}
		if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
			break;
		if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
			const uint32_t previousRate = rate;
			if (!readFormat()) {
				err = L"The decoder changed format mid-file and did not say to what.";
				return false;
			}
			if (rate != previousRate) {
				// A file that changes sample rate part way through would need
				// the whole buffer resampled to one rate. Refuse it rather
				// than play the tail at the wrong speed.
				err = L"That file changes sample rate part way through, which this player "
				      L"does not handle. Convert it to a single-rate WAV first.";
				return false;
			}
		}
		if (!sample)
			continue;  // a gap in the stream, not the end of it

		Com<IMFMediaBuffer> buffer;
		if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())))
			continue;

		BYTE* data = nullptr;
		DWORD length = 0;
		if (FAILED(buffer->Lock(&data, nullptr, &length)))
			continue;

		const float* samples = reinterpret_cast<const float*>(data);
		const size_t frames = size_t(length) / (sizeof(float) * channels);
		// More than two channels: take the front pair; mono doubles up. Same
		// rule as a live endpoint, so a file and a device sound the same.
		const size_t right = (channels > 1) ? 1 : 0;

		size_t take = frames;
		if (audio.l.size() + take > maxFrames) {
			take = (maxFrames > audio.l.size()) ? (maxFrames - audio.l.size()) : 0;
			truncated = true;
		}
		audio.l.reserve(audio.l.size() + take);
		audio.r.reserve(audio.r.size() + take);
		for (size_t i = 0; i < take; i++) {
			audio.l.push_back(samples[i * channels]);
			audio.r.push_back(samples[i * channels + right]);
		}

		buffer->Unlock();
		if (truncated)
			break;
	}

	if (audio.l.empty()) {
		err = L"That file decoded to no audio at all.";
		return false;
	}
	if (truncated) {
		// Not an error: the file is loaded and playable, and saying so is more
		// use than refusing it outright.
		wchar_t buf[256];
		swprintf_s(buf, L"Only the first %.0f minutes were loaded \x2014 the rest would not "
		                L"fit in memory. Use realmono-wav for the whole file.",
		           kMaxFileSeconds / 60.0);
		err = buf;
	}
	return true;
}


/** Loads any supported file. WAV goes through this project's own reader first
    so the app and the offline tool agree sample for sample on the format the
    validation actually uses; Media Foundation is the fallback, which is what
    picks up an ADPCM or A-law WAV the portable reader will not touch.

    Returns true with a non-empty `err` for a warning worth showing over a file
    that did load -- a truncated one, at present. */
inline bool loadAudioFile(const std::wstring& path, AudioBuffer& audio, std::wstring& err) {
	err.clear();
	const std::wstring ext = media::extensionOf(path);

	if (ext == L".wav" || ext == L".wave") {
		std::string wavErr;
		if (readWav(path, audio, wavErr)) {
			if (audio.frames() == 0) {
				err = L"That WAV file has no audio in it.";
				return false;
			}
			if (audio.seconds() > kMaxFileSeconds) {
				const size_t keep = size_t(kMaxFileSeconds * double(audio.sampleRate));
				audio.l.resize(keep);
				audio.r.resize(keep);
				wchar_t buf[256];
				swprintf_s(buf, L"Only the first %.0f minutes were loaded \x2014 the rest "
				                L"would not fit in memory. Use realmono-wav for the whole file.",
				           kMaxFileSeconds / 60.0);
				err = buf;
			}
			return true;
		}
		// Compressed WAV variants exist and Windows can decode several of them.
		std::wstring mediaErr;
		if (readMediaFile(path, audio, mediaErr)) {
			err = mediaErr;
			return true;
		}
		// The RIFF reader looked at the actual bytes, so its complaint is the
		// specific one; report that rather than "Windows could not open it".
		err = std::wstring(wavErr.begin(), wavErr.end());
		err = L"Could not read that WAV: " + err;
		return false;
	}

	return readMediaFile(path, audio, err);
}

} // namespace fmdr
