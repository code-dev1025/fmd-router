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

constexpr float kPiF = 3.14159265f;

/*  ------------------------------------------------------------------ palette

    Read off the client's screens. Named here so the whole scheme moves from
    one place when the wording and the colours get their next revision -- which
    they will, and a colour written into eight draw calls is a colour that ends
    up as seven of one and one of another. */

const Color kBannerText(255, 41, 249, 241);   // the cyan on the marble strip
const Color kBannerOutline(255, 12, 22, 14);  // and the stroke that makes it survive the veins
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
// The help spheres. Lit, shaded, rimmed and collared, in that order outwards.
const Color kHelpLit(255, 206, 178, 252);
const Color kHelpShade(255, 104, 60, 186);
const Color kHelpLitHot(255, 226, 206, 255);
const Color kHelpShadeHot(255, 128, 84, 214);
const Color kHelpEdge(255, 78, 42, 140);
const Color kHelpRing(255, 238, 236, 244);
const Color kHelpRingHot(255, 255, 255, 255);
const Color kHelpGlyph(255, 255, 255, 255);
const Color kHelpGlyphEdge(190, 46, 22, 92);

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

/*  A help button, which in the client's screens is a violet ball with a pale
    ring round it -- a sphere, not a disc. Three parts, in order:

      the ring      a light collar, which is what lifts it off a dark plate
      the body      a path gradient with its bright point up and to the left,
                    so it reads as lit from where everything else on the face
                    is lit from
      the glyph     white, with a thin dark outline so it survives the bright
                    side of the ball as well as the dark side

    A linear gradient was what this was first, and it looked like a shaded
    circle rather than a ball: the highlight has to be off-centre, and a linear
    ramp cannot put it there. */
inline void drawHelp(Gdiplus::Graphics& g, const RectF& r, bool hot,
                     const Gdiplus::FontFamily* family) {
	if (r.Width <= 2.f || r.Height <= 2.f)
		return;

	const float collar = (std::max)(1.f, r.Width * 0.085f);
	const RectF ball(r.X + collar, r.Y + collar,
	                 r.Width - collar * 2.f, r.Height - collar * 2.f);

	Gdiplus::SolidBrush ring(hot ? kHelpRingHot : kHelpRing);
	g.FillEllipse(&ring, r);

	Gdiplus::GraphicsPath body;
	body.AddEllipse(ball);
	Gdiplus::PathGradientBrush sphere(&body);
	// Up and to the left, about a third of the way in: any closer to the centre
	// and it is a disc again, any further out and it is a marble.
	sphere.SetCenterPoint(Gdiplus::PointF(ball.X + ball.Width * 0.33f,
	                                      ball.Y + ball.Height * 0.28f));
	sphere.SetCenterColor(hot ? kHelpLitHot : kHelpLit);
	Color rim[1] = {hot ? kHelpShadeHot : kHelpShade};
	INT count = 1;
	sphere.SetSurroundColors(rim, &count);
	g.FillPath(&sphere, &body);

	Gdiplus::Pen edge(kHelpEdge, (std::max)(1.f, r.Width * 0.045f));
	g.DrawEllipse(&edge, ball);

	if (!family)
		return;

	/*  Centred on its own ink rather than on the font's line box.

	    A question mark does not fill its em square -- no descender, and the
	    ink sits high in the box -- so aligning the box centres the empty space
	    as well as the glyph and the mark ends up visibly low and slightly off
	    to one side. Nudging it by a hand-picked fraction was the first fix and
	    it was only right for one font at one size.

	    So: lay the glyph out at the origin, ask the path what it actually
	    covers, and translate that to the middle of the ball. Correct for any
	    face, any size, and any glyph that might replace it. */
	Gdiplus::StringFormat format;
	format.SetAlignment(Gdiplus::StringAlignmentNear);
	format.SetLineAlignment(Gdiplus::StringAlignmentNear);
	format.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip | Gdiplus::StringFormatFlagsNoWrap);

	Gdiplus::GraphicsPath glyph;
	glyph.AddString(L"?", -1, family, Gdiplus::FontStyleBold, r.Height * 0.58f,
	                Gdiplus::PointF(0.f, 0.f), &format);

	RectF drawn;
	if (glyph.GetBounds(&drawn) == Gdiplus::Ok && drawn.Width > 0.f && drawn.Height > 0.f) {
		Gdiplus::Matrix move;
		move.Translate(r.X + r.Width * 0.5f - (drawn.X + drawn.Width * 0.5f),
		               r.Y + r.Height * 0.5f - (drawn.Y + drawn.Height * 0.5f));
		glyph.Transform(&move);
	}

	Gdiplus::Pen stroke(kHelpGlyphEdge, (std::max)(1.f, r.Width * 0.075f));
	stroke.SetLineJoin(Gdiplus::LineJoinRound);
	g.DrawPath(&stroke, &glyph);
	Gdiplus::SolidBrush ink(kHelpGlyph);
	g.FillPath(&ink, &glyph);
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

/*  Text with a stroke round the letterforms, which is what the client's banner
    is: green on grey marble, and the only reason it reads at all over the
    veins is that every letter is outlined.

    A drop shadow was the first attempt and it is not the same effect -- a
    shadow lands on one side and leaves the other side of the stroke sitting on
    whatever the stone happens to be doing there. The outline goes all the way
    round.

    Stroked before it is filled, and the pen is centred on the path, so the
    inner half of the stroke is painted over by the fill and what is left is a
    stroke entirely outside the letter. Filling first would eat into it. */
inline void drawOutlinedText(Gdiplus::Graphics& g, const wchar_t* text,
                             const Gdiplus::FontFamily* family, float size, int style,
                             const Color& fill, const Color& outline, float thickness,
                             const RectF& box, float weight = 0.f, float slant = 0.f) {
	if (!family || !text || size <= 0.f)
		return;
	Gdiplus::StringFormat format;
	format.SetAlignment(Gdiplus::StringAlignmentCenter);
	format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
	format.SetTrimming(Gdiplus::StringTrimmingNone);
	format.SetFormatFlags(Gdiplus::StringFormatFlagsNoClip | Gdiplus::StringFormatFlagsNoWrap);

	Gdiplus::GraphicsPath path;
	path.AddString(text, -1, family, style, size, box, &format);

	/*  The slant is sheared onto the path rather than asked of the font.

	    The banner face is a memory font with one weight in it, so whether a
	    real italic exists to be selected depends on what GDI+ decides to
	    synthesise, and that is not a thing to leave the client's heading
	    resting on. A shear is the same operation an oblique is, done where the
	    angle is ours to pick and identical on every machine.

	    Pivoted near the baseline, so the letters lean rather than slide. */
	if (slant != 0.f) {
		const float k = std::tan(slant * kPiF / 180.f);
		const float pivot = box.Y + box.Height * 0.72f;
		Gdiplus::Matrix lean(1.f, 0.f, -k, 1.f, k * pivot, 0.f);
		path.Transform(&lean);
	}

	/*  Weight fattens the letterform by stroking it in its own colour before
	    it is filled -- the pen is centred on the path, so half of that stroke
	    lands outside the outline the glyph would otherwise have had.

	    The dark outline is widened by the same amount so that what stays
	    visible of it is the width it was asked for: it runs from weight/2 out
	    to (thickness + weight)/2, which is thickness/2 either way. Emboldening
	    without that would quietly eat the outline in half. */
	const float bolder = (std::max)(0.f, weight);
	Gdiplus::Pen edge(outline, (std::max)(1.f, thickness + bolder));
	edge.SetLineJoin(Gdiplus::LineJoinRound);
	g.DrawPath(&edge, &path);

	if (bolder > 0.f) {
		Gdiplus::Pen thicken(fill, bolder);
		thicken.SetLineJoin(Gdiplus::LineJoinRound);
		g.DrawPath(&thicken, &path);
	}

	Gdiplus::SolidBrush brush(fill);
	g.FillPath(&brush, &path);
}


/*  ------------------------------------------------------------------- marble

    Value noise, and enough of it to make stone.

    The banner strip is grey marble in the client's screens and the artwork for
    it was not supplied, so it is generated. The first attempt drew wobbling
    horizontal lines and looked like brushed aluminium, because that is what
    regular curves at regular spacing look like. Stone is not regular: the
    veins wander, they branch, they vary in width along their length, and there
    is a mottle underneath them.

    All of which is one classic trick -- take a sine, and displace its argument
    by fractal noise before you take it. Where the noise is quiet the vein runs
    straight; where it is not, it wanders. Raising the result to a power turns a
    smooth band into a thin sharp line, which is what a vein is. */

inline float noiseAt(int x, int y, uint32_t seed) {
	uint32_t h = seed + uint32_t(x) * 0x9E3779B1u + uint32_t(y) * 0x85EBCA77u;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	h *= 0x297A2D39u;
	h ^= h >> 15;
	return float(h & 0xFFFFFFu) * (1.f / float(0xFFFFFF));
}

/** Bilinear value noise on a smoothstepped cell, which is cheap and, once it is
    four octaves deep, indistinguishable from the gradient kind at this scale. */
inline float smoothNoise(float x, float y, uint32_t seed) {
	const float fx = std::floor(x);
	const float fy = std::floor(y);
	const int x0 = int(fx);
	const int y0 = int(fy);
	const float tx = x - fx;
	const float ty = y - fy;
	const float ux = tx * tx * (3.f - 2.f * tx);
	const float uy = ty * ty * (3.f - 2.f * ty);

	const float a = noiseAt(x0, y0, seed);
	const float b = noiseAt(x0 + 1, y0, seed);
	const float c = noiseAt(x0, y0 + 1, seed);
	const float d = noiseAt(x0 + 1, y0 + 1, seed);

	const float top = a + (b - a) * ux;
	const float bottom = c + (d - c) * ux;
	return top + (bottom - top) * uy;
}

inline float fbm(float x, float y, int octaves, uint32_t seed) {
	float sum = 0.f;
	float amplitude = 0.5f;
	float frequency = 1.f;
	for (int i = 0; i < octaves; i++) {
		sum += amplitude * smoothNoise(x * frequency, y * frequency, seed + uint32_t(i) * 131u);
		// Not exactly two, so the octaves do not line up their cell edges and
		// leave a grid in the result.
		frequency *= 2.03f;
		amplitude *= 0.5f;
	}
	return sum;
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
			marble_.source = makeMarble(1024, 128);
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
	    not among the files supplied -- so it is generated. Written straight into
	    the pixels rather than drawn with curves: veins are a field, not a set of
	    strokes, and the first version of this drew strokes and looked like
	    brushed aluminium for exactly that reason.

	    Per pixel: a fractal-noise turbulence bends the argument of a sine, which
	    makes a band that wanders; raising one minus its absolute value to a power
	    turns that band into a thin sharp line, which is what a vein is. Two sets
	    of them at different scales give the branching, and a slow mottle
	    underneath keeps the stone between them from being flat.

	    The domain is sampled wide and shallow -- many cells across, barely two
	    down -- so the veins run the length of the strip the way a cut slab does.

	    Fixed seed, so it is the same stone on every repaint and every machine.
	    Generated once at a size the strip is then scaled down from, which is what
	    antialiases the veins. If the real artwork turns up: drop it in
	    resources/images, give it an id in Resources.h, and this becomes a call to
	    decode(). */
	static Gdiplus::Bitmap* makeMarble(int w, int h) {
		Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
		Gdiplus::Rect area(0, 0, w, h);
		Gdiplus::BitmapData data;
		if (bmp->LockBits(&area, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &data)
		    != Gdiplus::Ok) {
			return bmp;
		}

		constexpr uint32_t kSeed = 0x9E3779B9u;
		uint8_t* const base = static_cast<uint8_t*>(data.Scan0);

		for (int y = 0; y < h; y++) {
			uint32_t* row = reinterpret_cast<uint32_t*>(base + ptrdiff_t(y) * data.Stride);
			const float v = float(y) / float(h) * 2.2f;
			for (int x = 0; x < w; x++) {
				const float u = float(x) / float(w) * 4.6f;

				// The main veins: few, wide, dark, and wandering far enough off the
				// horizontal to stop the strip reading as wood grain -- which is what
				// it did at half this much turbulence.
				const float turbulence = fbm(u, v, 5, kSeed);
				const float band = std::sin((v * 1.8f + turbulence * 6.4f) * kPiF);
				const float vein = std::pow(1.f - std::abs(band), 4.f);

				// A finer set across them, which is what reads as branching.
				const float fine = fbm(u * 2.7f, v * 3.4f, 4, kSeed + 77u);
				const float hair = std::sin((v * 5.4f + fine * 7.0f) * kPiF);
				const float thread = std::pow(1.f - std::abs(hair), 11.f);

				// The stone itself, which is not one grey, plus a slow drift so that
				// whole regions of the slab are darker than others.
				const float mottle = fbm(u * 1.9f, v * 1.9f, 3, kSeed + 191u);
				const float drift = fbm(u * 0.55f, v * 0.55f, 2, kSeed + 313u);

				float grey = 0.94f + 0.13f * (mottle - 0.5f) - 0.16f * drift;
				grey -= vein * 0.82f;
				grey -= thread * 0.30f;
				grey = (std::max)(0.f, (std::min)(1.f, grey));

				const uint32_t level = uint32_t(grey * 255.f + 0.5f);
				// A hair of blue in the light, which is what stops grey stone
				// reading as a printer running out of toner.
				const uint32_t blue = (std::min)(255u, level + 4u);
				row[x] = 0xFF000000u | (level << 16) | (level << 8) | blue;
			}
		}

		bmp->UnlockBits(&data);
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
