/*  The client's face, drawn.

    Layout is written in the coordinates of the screens they sent -- 391 x 767
    -- and scaled once, at paint time, by whatever fraction of that fits the
    work area. So the proportions are the drawing's on every machine, the
    numbers below can be checked against the drawing with a ruler, and a laptop
    that cannot give it 767 pixels gets a smaller face rather than a clipped
    one.                                                                      */

#include "Face.h"
#include <windowsx.h>

#include "Skin.h"

#include <cmath>
#include <cstdio>

namespace fmdr {
namespace face {

namespace {

using namespace Gdiplus;

/*  ------------------------------------------------------------------ layout

    Measured off the client's screens. A Box is in design units; place() turns
    one into pixels. */

constexpr float kDesignW = 391.f;
constexpr float kDesignH = 767.f;

struct Box {
	float x, y, w, h;
};

constexpr Box kBanner      = {  0.f,   0.f, 391.f,  48.f};
constexpr Box kBannerHelp  = {356.f,  22.f,  23.f,  23.f};
constexpr Box kTitle       = { 34.f,  76.f, 323.f,  73.f};
constexpr Box kStereo      = { 42.f, 158.f, 150.f,  73.f};
constexpr Box kMono        = {200.f, 158.f, 147.f,  73.f};
constexpr Box kSolo        = {123.f, 258.f, 145.f, 148.f};
constexpr Box kSwitch      = { 38.f, 483.f,  52.f,  28.f};
constexpr Box kQualityText = { 96.f, 470.f, 256.f,  54.f};
constexpr Box kQualityHelp = {357.f, 485.f,  23.f,  23.f};
constexpr Box kLoudness    = { 98.f, 604.f, 196.f,  86.f};
constexpr Box kLoudHelp    = {299.f, 634.f,  23.f,  23.f};

/*  The client's wording, verbatim. It is expected to be revised, so it is
    gathered here rather than spelled inline at each draw call -- and the
    banner line is fitted to the window width rather than measured once, so a
    longer revision gets smaller instead of clipped. */
const wchar_t* const kBannerLine1 = L"REAL MONO SOUND - AUGMENTED MONO";
const wchar_t* const kBannerLine2 = L"MOBILITY IN HIGH FIDELITY";
const wchar_t* const kTitleText = L"Real Mono Sound";
const wchar_t* const kStereoText = L"Stereo";
const wchar_t* const kMonoText = L"Mono";
const wchar_t* const kSoloLine1 = L"SOLO";
const wchar_t* const kSoloLine2 = L"Augmented";
const wchar_t* const kSoloLine3 = L"Content";
const wchar_t* const kQualityLabel = L"Highest Quality Mode";
const wchar_t* const kLoudnessText = L"LOUDNESS";

/** Everything that can be clicked. The title is not among them: the client's
    screens show it grey in every state, so it is a heading. */
enum Widget {
	WNone = -1,
	WStereo = 0,
	WMono,
	WSolo,
	WSwitch,
	WQualityHelp,
	WLoudness,
	WLoudHelp,
	WBannerHelp,
	WCount,
};

const Box kWidgetBox[WCount] = {
    kStereo, kMono, kSolo, kSwitch, kQualityHelp, kLoudness, kLoudHelp, kBannerHelp,
};


/*  ---------------------------------------------------------------- help text

    The three question marks the client drew. Each says what the control does
    and why it is there -- the second half being the part a user cannot work
    out by pressing it. */

const wchar_t* const kHelpTitle =
    L"Real Mono Sound turns a stereo signal into a single mono feed that keeps "
    L"the difference between the two channels, instead of cancelling it the way "
    L"a plain (L+R)/2 downmix does.\n\n"
    L"Stereo\t\tHear the original, untouched. This is the A/B.\n"
    L"Mono\t\tHear the Real Mono feed.\n"
    L"SOLO Augmented Content\n"
    L"\t\tLift the recovered difference content inside the mix, so what a "
    L"normal downmix throws away is what you notice.\n\n"
    L"Choosing the input and output devices, playing a file through the chain, "
    L"the level meters and the per-stage controls are all in the Routing & "
    L"Advanced window.\n\n"
    L"Open it with Ctrl+A, or from this window's system menu (Alt+Space).";

const wchar_t* const kHelpQuality =
    L"Highest quality mode takes 3 dB off the input before the Mid and the "
    L"rotated Side are summed.\n\n"
    L"The sum of those two peaks higher than either input channel did -- about "
    L"3 dB higher on typical programme material, because the rotated Side has "
    L"the same spectrum as the Side but its peaks land somewhere else. Without "
    L"the trim, the limiter has to hand that back. Taking it at the input "
    L"instead leaves the dynamics intact.\n\n"
    L"The output is 3 dB quieter, which is the point.";

const wchar_t* const kHelpLoudness =
    L"Loudness drives the output 6 dB harder into the limiter, which is holding "
    L"the ceiling at -0.3 dBFS.\n\n"
    L"Louder, and denser: the peaks stay where they were and everything under "
    L"them comes up.\n\n"
    L"Off is unity gain, which is the level the chain was measured at. Turn it "
    L"off before comparing against a reference.";


/*  ------------------------------------------------------------------- state */

const wchar_t* const kClassName = L"RealMonoSoundFace";

/** A system menu id has to be below 0xF000 and the low four bits are the
    system's, so it is asked for on a multiple of sixteen. */
constexpr UINT kSysCommandPanel = 0x1000;

Host g_host;
HWND g_face = nullptr;
skin::Assets* g_assets = nullptr;
Bitmap* g_backBuffer = nullptr;
int g_hot = WNone;
int g_pressed = WNone;
bool g_tracking = false;


/*  ------------------------------------------------------------------ helpers */

struct Transform {
	float scale = 1.f;
	float ox = 0.f;
	float oy = 0.f;
};

Transform layoutFor(int cw, int ch) {
	Transform t;
	if (cw <= 0 || ch <= 0)
		return t;
	// Uniform, so a circle stays a circle. The window is created at the design
	// ratio, so in practice the two divisions agree and the offsets are zero;
	// they are here because a pixel of rounding either way should centre the
	// face rather than shear it.
	t.scale = (std::min)(float(cw) / kDesignW, float(ch) / kDesignH);
	t.ox = (float(cw) - kDesignW * t.scale) * 0.5f;
	t.oy = (float(ch) - kDesignH * t.scale) * 0.5f;
	return t;
}

RectF place(const Box& b, const Transform& t) {
	return RectF(t.ox + b.x * t.scale, t.oy + b.y * t.scale, b.w * t.scale, b.h * t.scale);
}

bool inside(const RectF& r, const POINT& p) {
	const float x = float(p.x);
	const float y = float(p.y);
	return x >= r.X && x < r.GetRight() && y >= r.Y && y < r.GetBottom();
}

int hitTest(HWND hwnd, POINT p) {
	RECT rc = {};
	GetClientRect(hwnd, &rc);
	const Transform t = layoutFor(rc.right, rc.bottom);
	for (int i = 0; i < WCount; i++) {
		if (inside(place(kWidgetBox[i], t), p))
			return i;
	}
	return WNone;
}

Color plateColour(bool on, bool hot) {
	if (on)
		return hot ? skin::kPlateOnHot : skin::kPlateOn;
	return hot ? skin::kPlateHot : skin::kPlate;
}

/** A button: the plate, then the client's italic serif on it. */
void drawButton(Graphics& g, const RectF& r, const wchar_t* text, float em,
                bool on, bool hot, const FontFamily* serif, float radius) {
	skin::fillPlate(g, r, plateColour(on, hot), radius);
	skin::drawText(g, text, serif, em, FontStyleItalic, skin::kPlateText, r);
}

void help(HWND owner, const wchar_t* body, const wchar_t* caption) {
	MessageBoxW(owner, body, caption, MB_ICONINFORMATION | MB_OK);
}


/*  -------------------------------------------------------------------- paint */

void paint(HWND hwnd, HDC dc) {
	RECT rc = {};
	GetClientRect(hwnd, &rc);
	const int cw = rc.right;
	const int ch = rc.bottom;
	if (cw <= 0 || ch <= 0 || !g_assets)
		return;

	// One back buffer, kept: the face repaints on a click or a state change,
	// not on a clock, but a fresh 400 x 900 surface per repaint is still an
	// allocation nobody needs.
	if (!g_backBuffer || int(g_backBuffer->GetWidth()) != cw
	    || int(g_backBuffer->GetHeight()) != ch) {
		delete g_backBuffer;
		g_backBuffer = new Bitmap(cw, ch, PixelFormat32bppPARGB);
	}

	Graphics g(g_backBuffer);
	g.SetSmoothingMode(SmoothingModeAntiAlias);
	g.SetTextRenderingHint(TextRenderingHintAntiAlias);
	g.SetPixelOffsetMode(PixelOffsetModeHalf);

	const Transform t = layoutFor(cw, ch);
	const Model m = g_host.read ? g_host.read() : Model();
	const FontFamily* serif = g_assets->serifFace();

	// ------------------------------------------------------------ background
	// Idle until something is actually playing; then the band that is doing
	// the work lights up -- the red sides for a stereo pass-through, the green
	// centre for the mono feed. Which is the client's own visual argument for
	// what the product does, so it is driven by the engine and not by the
	// button, and it goes dark again the moment the audio stops.
	const int plate = !m.running ? skin::PlateIdle
	                             : (m.mono ? skin::PlateMono : skin::PlateStereo);
	if (Bitmap* background = g_assets->background(plate, cw, ch))
		g.DrawImage(background, Rect(0, 0, cw, ch), 0, 0, cw, ch, UnitPixel);
	else
		g.Clear(Color(255, 24, 20, 20));

	// ---------------------------------------------------------------- banner
	// Edge to edge, unlike everything else, because that is how it is drawn.
	const int bannerH = int(kBanner.h * t.scale + 0.5f);
	if (Bitmap* marble = g_assets->banner(cw, bannerH))
		g.DrawImage(marble, Rect(0, 0, cw, bannerH), 0, 0, cw, bannerH, UnitPixel);

	{
		const FontFamily* mono = g_assets->bannerFace();
		const float start = kBanner.h * t.scale * 0.44f;
		// The first line has the strip to itself and takes the full width; the
		// second shares its row with the help button, so it has to stop short
		// of it at both ends -- it is centred, and a heading that clears the
		// button on one side and slides under it on the other is worse than a
		// slightly smaller one.
		const float clear = (kDesignW - kBannerHelp.x) * t.scale;
		const float first = skin::fitSize(g, kBannerLine1, mono, FontStyleBold, start,
		                                  float(cw) - 12.f * t.scale);
		const float second = skin::fitSize(g, kBannerLine2, mono, FontStyleBold, start,
		                                   float(cw) - clear * 2.f);
		// One size for both, or they read as two headings rather than one that
		// happens to take two lines.
		const float em = (std::min)(first, second);
		const float half = float(bannerH) * 0.5f;
		skin::drawText(g, kBannerLine1, mono, em, FontStyleBold, skin::kBannerText,
		               RectF(0.f, 0.f, float(cw), half), &skin::kBannerShade);
		skin::drawText(g, kBannerLine2, mono, em, FontStyleBold, skin::kBannerText,
		               RectF(0.f, half, float(cw), half), &skin::kBannerShade);
	}
	skin::drawHelp(g, place(kBannerHelp, t), g_hot == WBannerHelp, serif);

	// ----------------------------------------------------------------- title
	// Not a control. The client's screens show it grey in every state, so it
	// is a heading with the product's name on it and nothing happens when it
	// is clicked.
	const float radius = 3.f * t.scale;
	skin::fillPlate(g, place(kTitle, t), skin::kPlate, radius);
	skin::drawText(g, kTitleText, serif, 30.f * t.scale, FontStyleItalic,
	               skin::kPlateText, place(kTitle, t));

	// --------------------------------------------------------- stereo / mono
	// Two buttons for one setting: the global bypass. Mutually exclusive, and
	// exactly one of them is yellow at all times.
	drawButton(g, place(kStereo, t), kStereoText, 27.f * t.scale, !m.mono,
	           g_hot == WStereo, serif, radius);
	drawButton(g, place(kMono, t), kMonoText, 27.f * t.scale, m.mono,
	           g_hot == WMono, serif, radius);

	// ------------------------------------------------------------------ solo
	{
		const RectF box = place(kSolo, t);
		skin::fillPlate(g, box, plateColour(m.solo, g_hot == WSolo), radius);
		// Three lines with the first a little larger, as drawn. Stacked by
		// hand rather than left to the font's leading, so the block sits where
		// the drawing puts it.
		const float line = 40.f * t.scale;
		const float centres[3] = {36.f, 76.f, 116.f};
		const wchar_t* text[3] = {kSoloLine1, kSoloLine2, kSoloLine3};
		const float sizes[3] = {27.f * t.scale, 25.f * t.scale, 25.f * t.scale};
		for (int i = 0; i < 3; i++) {
			const RectF row(box.X, box.Y + centres[i] * t.scale - line * 0.5f,
			                box.Width, line);
			skin::drawText(g, text[i], serif, sizes[i], FontStyleItalic,
			               skin::kPlateText, row);
		}
	}

	// -------------------------------------------------- highest quality mode
	skin::drawSwitch(g, place(kSwitch, t), m.highestQuality, g_hot == WSwitch);
	{
		const FontFamily* script = g_assets->scriptFace();
		const RectF box = place(kQualityText, t);
		const float em = skin::fitSize(g, kQualityLabel, script, FontStyleRegular,
		                               36.f * t.scale, box.Width);
		skin::drawText(g, kQualityLabel, script, em, FontStyleRegular, skin::kScriptText,
		               box, &skin::kScriptShade);
	}
	skin::drawHelp(g, place(kQualityHelp, t), g_hot == WQualityHelp, serif);

	// -------------------------------------------------------------- loudness
	drawButton(g, place(kLoudness, t), kLoudnessText, 27.f * t.scale, m.loudness,
	           g_hot == WLoudness, serif, radius);
	skin::drawHelp(g, place(kLoudHelp, t), g_hot == WLoudHelp, serif);

	// --------------------------------------------------------------- present
	Graphics screen(dc);
	// Explicit source and destination rectangles: DrawImage(image, 0, 0) sizes
	// from the bitmap's own DPI, and a 96-versus-120 mismatch would silently
	// scale the whole face.
	screen.DrawImage(g_backBuffer, Rect(0, 0, cw, ch), 0, 0, cw, ch, UnitPixel);
}


/*  ------------------------------------------------------------------ actions */

void activate(HWND hwnd, int widget) {
	if (!g_host.read)
		return;
	const Model m = g_host.read();
	switch (widget) {
		case WStereo:
			if (g_host.setMono)
				g_host.setMono(false);
			break;
		case WMono:
			if (g_host.setMono)
				g_host.setMono(true);
			break;
		case WSolo:
			if (g_host.setSolo)
				g_host.setSolo(!m.solo);
			break;
		case WSwitch:
			if (g_host.setHighestQuality)
				g_host.setHighestQuality(!m.highestQuality);
			break;
		case WLoudness:
			if (g_host.setLoudness)
				g_host.setLoudness(!m.loudness);
			break;
		case WBannerHelp:
			help(hwnd, kHelpTitle, L"Real Mono Sound");
			break;
		case WQualityHelp:
			help(hwnd, kHelpQuality, L"Highest quality mode");
			break;
		case WLoudHelp:
			help(hwnd, kHelpLoudness, L"Loudness");
			break;
		default:
			break;
	}
	refresh();
}

void setHot(HWND hwnd, int widget) {
	if (g_hot == widget)
		return;
	g_hot = widget;
	InvalidateRect(hwnd, nullptr, FALSE);
}


/*  ------------------------------------------------------------------- window */

LRESULT CALLBACK faceProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
		case WM_CREATE: {
			// The panel is not on the face the client drew, and it has to be
			// reachable or the app cannot be told which devices to use. Two
			// ways in that cost the drawing nothing: the system menu, and a
			// shortcut. The title's help button names both.
			if (HMENU sys = GetSystemMenu(hwnd, FALSE)) {
				AppendMenuW(sys, MF_SEPARATOR, 0, nullptr);
				AppendMenuW(sys, MF_STRING, kSysCommandPanel,
				            L"&Routing && Advanced...\tCtrl+A");
			}
			return 0;
		}

		case WM_PAINT: {
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			paint(hwnd, dc);
			EndPaint(hwnd, &ps);
			return 0;
		}

		case WM_ERASEBKGND:
			return 1;  // every pixel is painted in WM_PAINT; erasing first flickers

		case WM_PRINTCLIENT:
			// Whoever is asking -- a thumbnail, a screen reader, PrintWindow --
			// gets the same face. Without this they get whatever was left in
			// the DC, because WM_ERASEBKGND above has told the system not to
			// bother filling it.
			paint(hwnd, HDC(wp));
			return 0;

		case WM_MOUSEMOVE: {
			POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
			setHot(hwnd, hitTest(hwnd, p));
			if (!g_tracking) {
				TRACKMOUSEEVENT track = {};
				track.cbSize = sizeof(track);
				track.dwFlags = TME_LEAVE;
				track.hwndTrack = hwnd;
				g_tracking = TrackMouseEvent(&track) != FALSE;
			}
			return 0;
		}

		case WM_MOUSELEAVE:
			g_tracking = false;
			setHot(hwnd, WNone);
			return 0;

		case WM_LBUTTONDOWN: {
			POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
			g_pressed = hitTest(hwnd, p);
			if (g_pressed != WNone)
				SetCapture(hwnd);
			return 0;
		}

		case WM_LBUTTONUP: {
			POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
			const int released = hitTest(hwnd, p);
			const int pressed = g_pressed;
			g_pressed = WNone;
			if (GetCapture() == hwnd)
				ReleaseCapture();
			// Only if the pointer came up on the control it went down on,
			// which is the standard let-go-elsewhere-to-cancel.
			if (pressed != WNone && pressed == released)
				activate(hwnd, released);
			return 0;
		}

		case WM_SETCURSOR:
			if (LOWORD(lp) == HTCLIENT) {
				POINT p = {};
				GetCursorPos(&p);
				ScreenToClient(hwnd, &p);
				SetCursor(LoadCursorW(nullptr, hitTest(hwnd, p) != WNone ? IDC_HAND : IDC_ARROW));
				return TRUE;
			}
			break;

		case WM_KEYDOWN:
			if (wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
				if (g_host.openPanel)
					g_host.openPanel();
				return 0;
			}
			if (wp == VK_F1) {
				help(hwnd, kHelpTitle, L"Real Mono Sound");
				return 0;
			}
			break;

		case WM_SYSCOMMAND:
			if ((wp & 0xFFF0) == kSysCommandPanel) {
				if (g_host.openPanel)
					g_host.openPanel();
				return 0;
			}
			break;

		case WM_CLOSE:
			// Closing the face closes the app. The host stops the audio and
			// takes the panel down first, while everything they publish into
			// is still standing.
			if (g_host.close)
				g_host.close();
			DestroyWindow(hwnd);
			return 0;

		case WM_DESTROY:
			g_face = nullptr;
			PostQuitMessage(0);
			return 0;

		default:
			break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace


bool registerClass(HINSTANCE instance) {
	WNDCLASSEXW cls = {};
	cls.cbSize = sizeof(cls);
	cls.lpfnWndProc = faceProc;
	cls.hInstance = instance;
	cls.hCursor = nullptr;  // WM_SETCURSOR picks between the arrow and the hand
	cls.lpszClassName = kClassName;
	return RegisterClassExW(&cls) != 0;
}


HWND create(HINSTANCE instance, const Host& host) {
	g_host = host;

	if (!g_assets) {
		g_assets = new skin::Assets();
		g_assets->load(instance);
	}

	// Fixed size, as the panel is: the layout scales but it does not reflow,
	// and a window that can be dragged to a shape its contents do not take is
	// worse than one that cannot be dragged at all.
	//
	// The size is chosen rather than fixed, though. The drawing is 767 tall,
	// which with a title bar does not fit the work area of a 1366 x 768
	// laptop, so the largest whole-face scale that does fit is used -- capped
	// just above 1:1, because past that it is a poster and not an interface.
	const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

	RECT probe = {0, 0, 1000, 1000};
	AdjustWindowRect(&probe, style, FALSE);
	const float frameW = float((probe.right - probe.left) - 1000);
	const float frameH = float((probe.bottom - probe.top) - 1000);

	RECT work = {0, 0, 1280, 1024};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
	const float roomW = float(work.right - work.left) * 0.48f - frameW;
	const float roomH = float(work.bottom - work.top) * 0.96f - frameH;

	float scale = (std::min)(roomW / kDesignW, roomH / kDesignH);
	scale = (std::max)(0.55f, (std::min)(scale, 1.15f));

	RECT wanted = {0, 0, int(kDesignW * scale + 0.5f), int(kDesignH * scale + 0.5f)};
	AdjustWindowRect(&wanted, style, FALSE);
	const int width = wanted.right - wanted.left;
	const int height = wanted.bottom - wanted.top;
	const int x = int(work.left) + 24;
	const int y = int(work.top)
	            + (std::max)(0, (int(work.bottom - work.top) - height) / 2);

	g_face = CreateWindowExW(0, kClassName, L"Real Mono Sound", style,
	                         x, y, width, height, nullptr, nullptr, instance, nullptr);
	return g_face;
}


void refresh() {
	if (g_face)
		InvalidateRect(g_face, nullptr, FALSE);
}


void shutdown() {
	delete g_backBuffer;
	g_backBuffer = nullptr;
	delete g_assets;
	g_assets = nullptr;
	g_face = nullptr;
}

} // namespace face
} // namespace fmdr
