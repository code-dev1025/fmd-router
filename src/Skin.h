#pragma once

/*  The client's skin: the three background plates, the type faces, the palette
    read off their screens, and the handful of shapes the face is drawn out of.

    Everything here is GDI+. That is a Windows system library rather than a
    third-party dependency -- the same argument Media Foundation gets in
    MediaFile.h -- and it is the only drawing API in the box that will decode a
    JPEG, take a memory font, and antialias a rounded rectangle without a
    thousand lines of help. Plain GDI does none of the three.

    The face is drawn rather than assembled out of controls, because none of
    what the client drew is a Windows control: the buttons are flat plates with
    italic serif on them, the switch is an iOS pill, and the help buttons are
    spheres. Owner-drawing eight of those is less code than skinning eight of
    them, and it is the version where the screen matches the drawing.         */

// gdiplustypes.h says min and max unqualified, and NOMINMAX -- which this
// project has to set, or windows.h's macros break std::min -- has taken them
// away. Lending it the std ones is the documented way round it, and it has to
// happen before the header is reached.
#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}

#include <windows.h>
#include <objidl.h>

#pragma warning(push)
#pragma warning(disable : 4458)  // gdiplus's own parameters shadow its members at /W4
#include <gdiplus.h>
#pragma warning(pop)

#include "Resources.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fmdr {
namespace skin {

using Gdiplus::Color;
using Gdiplus::RectF;

/*  ------------------------------------------------------------------ palette

    Read off the client's screens. Named here so the whole scheme moves from
    one place when the wording and the colours get their next revision -- which
    they will, and a colour written into eight draw calls is a colour that ends
    up as seven of one and one of another. */

const Color kBannerText(255, 0, 168, 62);    // the green on the marble strip
const Color kBannerShade(110, 0, 0, 0);      // its shadow, for legibility over the veins
const Color kPlate(255, 160, 160, 160);      // an inactive button
const Color kPlateHot(255, 184, 184, 184);   // ... under the pointer
const Color kPlateOn(255, 255, 249, 92);     // the selected yellow
const Color kPlateOnHot(255, 255, 252, 158);
const Color kPlateEdge(60, 0, 0, 0);
const Color kPlateText(255, 26, 26, 26);
const Color kScriptText(255, 0, 226, 226);   // "Highest Quality Mode"
const Color kScriptShade(120, 0, 40, 40);
const Color kSwitchOn(255, 42, 122, 246);
const Color kSwitchOff(255, 116, 116, 124);
const Color kSwitchKnob(255, 255, 255, 255);
const Color kHelpTop(255, 178, 142, 245);    // the help spheres, lit from above
const Color kHelpBottom(255, 122, 78, 206);
const Color kHelpEdge(255, 74, 44, 132);
const Color kHelpGlyph(255, 255, 255, 255);

/** Which plate is behind the face. Idle is the one the client's screens show
    with nothing playing; the other two light the band that is doing the work. */
enum Plate {
	PlateIdle = 0,
	PlateStereo = 1,
	PlateMono = 2,
	PlateCount = 3,
};


/*  ------------------------------------------------------------------- shapes */

/** The client's buttons have barely-there corners: dead square reads as
    unfinished at this size, and a full pill reads as a different product. */
inline void roundedPath(Gdiplus::GraphicsPath& path, const RectF& r, float radius) {
	if (radius <= 0.5f || r.Width <= 0.f || r.Height <= 0.f) {
		path.AddRectangle(r);
		return;
	}
	radius = (std::min)(radius, (std::min)(r.Width, r.Height) * 0.5f);
	const float d = radius * 2.f;
	path.AddArc(r.X, r.Y, d, d, 180.f, 90.f);
	path.AddArc(r.GetRight() - d, r.Y, d, d, 270.f, 90.f);
	path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.f, 90.f);
	path.AddArc(r.X, r.GetBottom() - d, d, d, 90.f, 90.f);
	path.CloseFigure();
}

inline void fillPlate(Gdiplus::Graphics& g, const RectF& r, const Color& fill, float radius) {
	Gdiplus::GraphicsPath path;
	roundedPath(path, r, radius);
	Gdiplus::SolidBrush brush(fill);
	g.FillPath(&brush, &path);
	Gdiplus::Pen pen(kPlateEdge, 1.f);
	g.DrawPath(&pen, &path);
}

/*  The iOS-style switch the client drew for highest quality mode. A pill, a
    knob at whichever end, and no travel animation: the state is what matters,
    and 150 ms of slide on a setting nobody watches is motion for its own sake. */
inline void drawSwitch(Gdiplus::Graphics& g, const RectF& r, bool on, bool hot) {
	const float radius = r.Height * 0.5f;
	Gdiplus::GraphicsPath track;
	roundedPath(track, r, radius);

	Color body = on ? kSwitchOn : kSwitchOff;
	if (hot) {
		body = Color(255,
		             BYTE((std::min)(255, int(body.GetR()) + 24)),
		             BYTE((std::min)(255, int(body.GetG()) + 24)),
		             BYTE((std::min)(255, int(body.GetB()) + 24)));
	}
	Gdiplus::SolidBrush brush(body);
	g.FillPath(&brush, &track);
	Gdiplus::Pen edge(Color(70, 0, 0, 0), 1.f);
	g.DrawPath(&edge, &track);

	const float inset = r.Height * 0.11f;
	const float size = r.Height - inset * 2.f;
	const float x = on ? (r.GetRight() - inset - size) : (r.X + inset);
	Gdiplus::SolidBrush knob(kSwitchKnob);
	g.FillEllipse(&knob, x, r.Y + inset, size, size);
	Gdiplus::Pen knobEdge(Color(50, 0, 0, 0), 1.f);
	g.DrawEllipse(&knobEdge, x, r.Y + inset, size, size);
}

/** A help sphere: violet, lit from the top, with a white question mark. */
inline void drawHelp(Gdiplus::Graphics& g, const RectF& r, bool hot,
                     const Gdiplus::FontFamily* family) {
	Color top = kHelpTop;
	Color bottom = kHelpBottom;
	if (hot) {
		top = Color(255, 202, 174, 255);
		bottom = Color(255, 146, 102, 232);
	}
	Gdiplus::LinearGradientBrush brush(
	    Gdiplus::PointF(r.X, r.Y), Gdiplus::PointF(r.X, r.GetBottom()), top, bottom);
	g.FillEllipse(&brush, r);
	Gdiplus::Pen edge(kHelpEdge, 1.2f);
	g.DrawEllipse(&edge, r);

	if (!family)
		return;
	Gdiplus::StringFormat format;
	format.SetAlignment(Gdiplus::StringAlignmentCenter);
	format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
	Gdiplus::Font font(family, r.Height * 0.62f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
	Gdiplus::SolidBrush glyph(kHelpGlyph);
	// Nudged up by a hair: a question mark's ink sits below its own box centre,
	// so centring the box leaves it looking like it has slipped.
	const RectF box(r.X, r.Y - r.Height * 0.05f, r.Width, r.Height);
	g.DrawString(L"?", -1, &font, box, &format, &glyph);
}


/*  --------------------------------------------------------------------- text */

/** The largest size at or below `start` at which `text` still fits `maxWidth`.
    Text width is close enough to linear in the em size that one division lands
    it; the loop is there for the faces where it does not.

    Worth having rather than trusting a measured constant: the banner line is
    thirty-two characters of the client's wording across the full width of the
    window, and the wording is expected to be revised. A longer one should get
    smaller, not clipped. */
inline float fitSize(Gdiplus::Graphics& g, const wchar_t* text,
                     const Gdiplus::FontFamily* family, int style,
                     float start, float maxWidth) {
	if (!family || !text || maxWidth <= 1.f)
		return start;
	// Measured through the same StringFormat drawText will use, not the
	// typographic one: the default adds about a sixth of an em of padding at
	// each end, so measuring without it reports a string that fits and then
	// draws one that wraps. Which is exactly what it did.
	Gdiplus::StringFormat format;
	format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
	float size = start;
	for (int attempt = 0; attempt < 6; attempt++) {
		Gdiplus::Font font(family, size, style, Gdiplus::UnitPixel);
		RectF bounds;
		g.MeasureString(text, -1, &font, Gdiplus::PointF(0.f, 0.f), &format, &bounds);
		if (bounds.Width <= maxWidth || bounds.Width <= 0.f)
			break;
		size *= maxWidth / bounds.Width;
	}
	return size;
}

/** Centred text, with an optional drop shadow. Line breaks arrive in the
    string: the face's labels are three words at most, and where they break is
    part of the client's drawing rather than something to leave to a flow. */
inline void drawText(Gdiplus::Graphics& g, const wchar_t* text,
                     const Gdiplus::FontFamily* family, float size, int style,
                     const Color& colour, const RectF& box,
                     const Color* shadow = nullptr) {
	if (!family || !text)
		return;
	Gdiplus::StringFormat format;
	format.SetAlignment(Gdiplus::StringAlignmentCenter);
	format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
	format.SetTrimming(Gdiplus::StringTrimmingNone);
	// NoWrap because every break in this interface is one the client drew and
	// arrives in the string; NoClip so a descender at the edge of its box is
	// drawn rather than shaved.
	format.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip | Gdiplus::StringFormatFlagsNoWrap);

	Gdiplus::Font font(family, size, style, Gdiplus::UnitPixel);
	if (shadow) {
		Gdiplus::SolidBrush brush(*shadow);
		const float offset = (std::max)(1.f, size * 0.055f);
		const RectF shifted(box.X + offset, box.Y + offset, box.Width, box.Height);
		g.DrawString(text, -1, &font, shifted, &format, &brush);
	}
	Gdiplus::SolidBrush brush(colour);
	g.DrawString(text, -1, &font, box, &format, &brush);
}


/*  ---------------------------------------------------------------- the assets

    Loaded once at startup and held for the life of the process. Scaled copies
    are cached against the size they were made for, so a repaint at an
    unchanged size is a blit rather than a resample of a 785 x 1578 plate. */

class Assets {
public:
	Assets() = default;
	Assets(const Assets&) = delete;
	Assets& operator=(const Assets&) = delete;

	~Assets() {
		release();
	}

	/** True when all three plates decoded. The fonts fall back rather than
	    fail: a face in the wrong type is still usable, a face with no
	    background is not what the client drew at all. */
	bool load(HINSTANCE instance) {
		static const int kIds[PlateCount] = {
		    IDR_BACKGROUND_IDLE, IDR_BACKGROUND_STEREO, IDR_BACKGROUND_MONO};
		bool ok = true;
		for (int i = 0; i < PlateCount; i++) {
			plates_[i].source = decode(instance, kIds[i]);
			ok = ok && (plates_[i].source != nullptr);
		}

		addMemoryFont(instance, IDR_FONT_BANNER);
		addMemoryFont(instance, IDR_FONT_SCRIPT);
		addMemoryFont(instance, IDR_FONT_BANNER_ALT);

		// Consolas is on every Windows machine, but the embedded copy is the
		// one the client's screens were drawn with, so it is asked for first
		// and the installed one is only the fallback. Roboto Mono, also
		// supplied, stands behind both.
		banner_ = family(L"Consolas", true);
		if (!banner_)
			banner_ = family(L"Consolas", false);
		if (!banner_)
			banner_ = family(L"Roboto Mono", true);
		if (!banner_)
			banner_ = family(L"Courier New", false);

		script_ = family(L"Great Vibes", true);
		if (!script_)
			script_ = family(L"Segoe Script", false);
		if (!script_)
			script_ = family(L"Segoe UI", false);

		// The button face is an italic serif, which is not among the files
		// supplied -- so it comes from the system, where Georgia has shipped
		// since Windows 2000 and Times New Roman since before that.
		serif_ = family(L"Georgia", false);
		if (!serif_)
			serif_ = family(L"Times New Roman", false);

		return ok;
	}

	void release() {
		for (int i = 0; i < PlateCount; i++)
			plates_[i].discard();
		marble_.discard();
		delete banner_;
		delete script_;
		delete serif_;
		banner_ = nullptr;
		script_ = nullptr;
		serif_ = nullptr;
	}

	/** One of the three plates, scaled to fill `w` x `h`. */
	Gdiplus::Bitmap* background(int plate, int w, int h) {
		if (plate < 0 || plate >= PlateCount)
			plate = PlateIdle;
		return scaled(plates_[plate], w, h);
	}

	/** The banner strip, at the size the face wants it. */
	Gdiplus::Bitmap* banner(int w, int h) {
		if (!marble_.source)
			marble_.source = makeMarble(800, 120);
		return scaled(marble_, w, h);
	}

	const Gdiplus::FontFamily* bannerFace() const { return banner_; }
	const Gdiplus::FontFamily* scriptFace() const { return script_; }
	const Gdiplus::FontFamily* serifFace() const { return serif_; }

private:
	struct Cached {
		Gdiplus::Bitmap* source = nullptr;
		Gdiplus::Bitmap* scaled = nullptr;
		int w = 0;
		int h = 0;

		void discard() {
			delete scaled;
			delete source;
			scaled = nullptr;
			source = nullptr;
			w = 0;
			h = 0;
		}
	};

	static Gdiplus::Bitmap* scaled(Cached& cache, int w, int h) {
		if (!cache.source || w <= 0 || h <= 0)
			return nullptr;
		if (cache.scaled && cache.w == w && cache.h == h)
			return cache.scaled;
		delete cache.scaled;
		cache.scaled = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
		Gdiplus::Graphics g(cache.scaled);
		g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
		g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
		g.DrawImage(cache.source, Gdiplus::Rect(0, 0, w, h),
		            0, 0, INT(cache.source->GetWidth()), INT(cache.source->GetHeight()),
		            Gdiplus::UnitPixel);
		cache.w = w;
		cache.h = h;
		return cache.scaled;
	}

	/*  GDI+ decodes from a stream and it decodes lazily, so the bytes have to
	    outlive the first look at the pixels. Copying into an HGLOBAL the stream
	    owns, and taking a detached clone before the stream goes, is the version
	    of that with no lifetime question left in it. */
	static Gdiplus::Bitmap* decode(HINSTANCE instance, int id) {
		DWORD size = 0;
		const void* bytes = find(instance, id, size);
		if (!bytes)
			return nullptr;

		HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
		if (!mem)
			return nullptr;
		void* dst = GlobalLock(mem);
		if (!dst) {
			GlobalFree(mem);
			return nullptr;
		}
		std::memcpy(dst, bytes, size);
		GlobalUnlock(mem);

		IStream* stream = nullptr;
		if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) {
			GlobalFree(mem);
			return nullptr;
		}

		Gdiplus::Bitmap* decoded = Gdiplus::Bitmap::FromStream(stream);
		Gdiplus::Bitmap* owned = nullptr;
		if (decoded && decoded->GetLastStatus() == Gdiplus::Ok) {
			owned = decoded->Clone(0, 0, INT(decoded->GetWidth()), INT(decoded->GetHeight()),
			                       PixelFormat32bppPARGB);
			if (owned && owned->GetLastStatus() != Gdiplus::Ok) {
				delete owned;
				owned = nullptr;
			}
		}
		delete decoded;
		stream->Release();  // takes the HGLOBAL with it
		return owned;
	}

	static const void* find(HINSTANCE instance, int id, DWORD& size) {
		HRSRC res = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
		if (!res)
			return nullptr;
		size = SizeofResource(instance, res);
		HGLOBAL handle = LoadResource(instance, res);
		const void* bytes = handle ? LockResource(handle) : nullptr;
		return (bytes && size > 0) ? bytes : nullptr;
	}

	/** Resource memory is mapped for the life of the module, which is exactly
	    the lifetime AddMemoryFont asks of what it is handed -- so no copy. */
	void addMemoryFont(HINSTANCE instance, int id) {
		DWORD size = 0;
		const void* bytes = find(instance, id, size);
		if (bytes)
			private_.AddMemoryFont(bytes, INT(size));
	}

	Gdiplus::FontFamily* family(const wchar_t* name, bool embedded) {
		Gdiplus::FontFamily* f = new Gdiplus::FontFamily(name, embedded ? &private_ : nullptr);
		if (f->GetLastStatus() == Gdiplus::Ok && f->IsAvailable())
			return f;
		delete f;
		return nullptr;
	}

	/*  The client's banner sits on a strip of grey marble, and that strip was
	    not among the files supplied -- so it is generated rather than replaced
	    with a flat fill, which is plainly not what the screens show. Light
	    stone with darker veins running the length of it, from a fixed seed, so
	    it is the same strip on every repaint and on every machine.

	    If the real artwork turns up: drop it in resources/images, give it an id
	    in Resources.h, and this becomes a call to decode(). */
	static Gdiplus::Bitmap* makeMarble(int w, int h) {
		Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
		Gdiplus::Graphics g(bmp);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		g.Clear(Color(255, 203, 203, 206));

		uint32_t seed = 0x9E3779B9u;
		auto next = [&seed]() {
			seed = seed * 1664525u + 1013904223u;
			return seed >> 8;
		};
		auto range = [&next](int lo, int hi) {
			return lo + int(next() % uint32_t(hi - lo + 1));
		};

		const int points = 16;
		// static_cast, not size_t(points): the functional-cast spelling makes the
		// whole line parse as a function declaration instead of a vector.
		std::vector<Gdiplus::PointF> vein(static_cast<size_t>(points));
		for (int i = 0; i < 150; i++) {
			const int level = range(64, 196);
			const int alpha = range(24, 120);
			const float thickness = float(range(4, 26)) * 0.1f;
			const float y0 = float(range(-4, h + 4));
			const float wobble = float(range(1, 6)) * float(h) * 0.018f;
			const float phase = float(range(0, 62)) * 0.1f;
			const float turns = float(range(20, 90)) * 0.1f;
			for (int p = 0; p < points; p++) {
				const float t = float(p) / float(points - 1);
				vein[size_t(p)].X = t * float(w);
				vein[size_t(p)].Y = y0 + wobble * std::sin(phase + t * turns);
			}
			Gdiplus::Pen pen(Color(BYTE(alpha), BYTE(level), BYTE(level),
			                       BYTE((std::min)(255, level + 3))),
			                 thickness);
			g.DrawCurve(&pen, vein.data(), points);
		}
		return bmp;
	}

	Cached plates_[PlateCount];
	Cached marble_;
	Gdiplus::PrivateFontCollection private_;
	Gdiplus::FontFamily* banner_ = nullptr;
	Gdiplus::FontFamily* script_ = nullptr;
	Gdiplus::FontFamily* serif_ = nullptr;
};

} // namespace skin
} // namespace fmdr
