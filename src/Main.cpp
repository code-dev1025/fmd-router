/*  FMD Router -- Win32 front end.

    The window is deliberately plain: combo boxes, trackbars and three meters.
    The brief for this codebase says the interface gets perfected later, so the
    effort here goes into the controls being honest -- every slider is bound to
    a parameter the audio thread actually reads, and every number on screen is
    measured rather than assumed. */

#include "Devices.h"
#include "Engine.h"

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

// Visual styles. Without this the controls still work, they just render as
// Windows 95; with it, comctl32 v6 also requires InitCommonControlsEx below.
#pragma comment(linker, "/manifestdependency:\"type='win32' "                       \
                        "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                        "language='*'\"")

namespace {

using namespace fmdr;

enum : int {
	IDC_CAPTURE_COMBO = 1001,
	IDC_RENDER_COMBO = 1002,
	IDC_LOOPBACK = 1003,
	IDC_REFRESH = 1004,
	IDC_START = 1005,
	IDC_MODULE = 1006,
	IDC_MODE = 1007,
	IDC_AGGR = 1008,
	IDC_BANNER = 1009,
	IDC_STATUS = 1010,
	IDC_STATUS2 = 1011,

	IDC_SLIDER_BASE = 1100,   // eight, see kSliders
	IDC_SLIDER_NAME_BASE = 1120,
	IDC_SLIDER_VALUE_BASE = 1140,
	IDC_METER_BASE = 1160,    // three: in L, in R, out

	IDT_REFRESH = 1,
};

enum SliderIndex {
	SL_FREQ = 0,
	SL_RES,
	SL_DRIVE,
	SL_SPREAD,
	SL_GRIT,
	SL_CLIP,
	SL_MIX,
	SL_OUT,
	SL_COUNT,
};

struct SliderDef {
	const wchar_t* name;
	int defaultValue;  // 0..1000
};

// FREQ defaults to 6 octaves above 20 Hz (1.28 kHz) so the filter opens
// somewhere musical rather than at either end stop.
const SliderDef kSliders[SL_COUNT] = {
	{L"FREQ",   600},
	{L"RES",    200},
	{L"DRIVE",    0},
	{L"SPREAD",   0},
	{L"NOISE",    0},
	{L"CLIP",     0},
	{L"MIX",   1000},
	{L"OUT",    800},  // exactly 0 dB on the -60..+15 dB scale
};

const wchar_t* const kModules[] = {L"Flower Child", L"Shaped Resonator", L"Super Love"};

struct ModeOption {
	const wchar_t* label;
	int mode;
};

const ModeOption kShapedModes[] = {
	{L"BAND", fmd::FilterCore::MODE_BP},
	{L"LOW", fmd::FilterCore::MODE_LP12},
	{L"HIGH", fmd::FilterCore::MODE_HP},
};

const ModeOption kSuperLoveModes[] = {
	{L"LP 6 dB", fmd::FilterCore::MODE_LP6},
	{L"LP 12 dB", fmd::FilterCore::MODE_LP12},
	{L"BP", fmd::FilterCore::MODE_BP},
	{L"HP", fmd::FilterCore::MODE_HP},
};

const ModeOption kFlowerChildModes[] = {
	{L"LP 12 dB (fixed)", fmd::FilterCore::MODE_LP12},
};


struct App {
	HWND main = nullptr;
	HFONT font = nullptr;

	HWND captureCombo = nullptr;
	HWND renderCombo = nullptr;
	HWND loopbackCheck = nullptr;
	HWND startButton = nullptr;
	HWND moduleCombo = nullptr;
	HWND modeCombo = nullptr;
	HWND aggrCheck = nullptr;
	HWND banner = nullptr;
	HWND status = nullptr;
	HWND status2 = nullptr;
	HWND sliders[SL_COUNT] = {};
	HWND sliderNames[SL_COUNT] = {};
	HWND sliderValues[SL_COUNT] = {};
	HWND meters[3] = {};

	std::vector<DeviceInfo> captureDevices;
	std::vector<DeviceInfo> renderDevices;

	Engine engine;

	// Meter state lives on the GUI side: the audio thread reports a peak and
	// forgets it, and the decay that makes a meter readable is a display
	// concern with no business on a real-time thread.
	float meterLevel[3] = {0.f, 0.f, 0.f};
};

App g;


// ---------------------------------------------------------------- utilities

float sliderNorm(HWND slider) {
	return float(SendMessageW(slider, TBM_GETPOS, 0, 0)) / 1000.f;
}

/** OUT is the one slider that is not a plain 0..1: it is dB, so that the
    useful part of the travel is not squeezed into the top tenth. The -60..+15
    span puts unity at exactly 0.8, which is a slider position that can be
    reached and labelled without rounding to "-0.0 dB". */
float outGainDb(float norm) {
	return -60.f + norm * 75.f;
}

std::wstring formatSliderValue(int index, float norm) {
	wchar_t buf[64];
	switch (index) {
		case SL_FREQ: {
			const float hz = 20.f * std::pow(2.f, norm * 10.f);
			if (hz >= 1000.f)
				swprintf_s(buf, L"%.2f kHz", hz / 1000.f);
			else
				swprintf_s(buf, L"%.0f Hz", hz);
			break;
		}
		case SL_OUT: {
			const float db = outGainDb(norm);
			if (norm <= 0.f)
				swprintf_s(buf, L"-inf dB");
			else
				swprintf_s(buf, L"%+.1f dB", db);
			break;
		}
		case SL_MIX:
			swprintf_s(buf, L"%.0f%% wet", norm * 100.f);
			break;
		default:
			swprintf_s(buf, L"%.0f%%", norm * 100.f);
			break;
	}
	return std::wstring(buf);
}

int currentModule() {
	const LRESULT sel = SendMessageW(g.moduleCombo, CB_GETCURSEL, 0, 0);
	return (sel == CB_ERR) ? ModFlowerChild : int(sel);
}

const ModeOption* modeTable(int module, int& count) {
	switch (module) {
		case ModShapedResonator:
			count = int(std::size(kShapedModes));
			return kShapedModes;
		case ModSuperLove:
			count = int(std::size(kSuperLoveModes));
			return kSuperLoveModes;
		default:
			count = int(std::size(kFlowerChildModes));
			return kFlowerChildModes;
	}
}


// ------------------------------------------------------- parameter plumbing

void pushParams() {
	ChainParams& p = g.engine.params;
	const int module = currentModule();

	p.module.store(module, std::memory_order_relaxed);

	int modeCount = 0;
	const ModeOption* modes = modeTable(module, modeCount);
	LRESULT sel = SendMessageW(g.modeCombo, CB_GETCURSEL, 0, 0);
	if (sel == CB_ERR || sel >= modeCount)
		sel = 0;
	p.mode.store(modes[sel].mode, std::memory_order_relaxed);

	p.aggressive.store(
	    (module == ModFlowerChild
	     && SendMessageW(g.aggrCheck, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0,
	    std::memory_order_relaxed);

	p.freqOct.store(sliderNorm(g.sliders[SL_FREQ]) * 10.f, std::memory_order_relaxed);
	p.res.store(sliderNorm(g.sliders[SL_RES]), std::memory_order_relaxed);
	p.drive.store(sliderNorm(g.sliders[SL_DRIVE]), std::memory_order_relaxed);
	p.spread.store(sliderNorm(g.sliders[SL_SPREAD]), std::memory_order_relaxed);
	p.grit.store(sliderNorm(g.sliders[SL_GRIT]), std::memory_order_relaxed);
	p.clip.store(sliderNorm(g.sliders[SL_CLIP]), std::memory_order_relaxed);
	p.mix.store(sliderNorm(g.sliders[SL_MIX]), std::memory_order_relaxed);

	const float outNorm = sliderNorm(g.sliders[SL_OUT]);
	const float gain = (outNorm <= 0.f) ? 0.f : std::pow(10.f, outGainDb(outNorm) / 20.f);
	p.outGain.store(gain, std::memory_order_relaxed);
}

void refreshSliderLabels() {
	for (int i = 0; i < SL_COUNT; i++)
		SetWindowTextW(g.sliderValues[i], formatSliderValue(i, sliderNorm(g.sliders[i])).c_str());
}

/** The grit control is one knob wearing three names, exactly as the three
    panels label it. Rebuilding the mode list here keeps the UI from ever
    offering a mode the selected module does not have. */
void applyModuleToUi() {
	const int module = currentModule();

	SetWindowTextW(g.sliderNames[SL_GRIT], (module == ModShapedResonator) ? L"CRNCH" : L"NOISE");
	EnableWindow(g.aggrCheck, module == ModFlowerChild);

	int count = 0;
	const ModeOption* modes = modeTable(module, count);
	SendMessageW(g.modeCombo, CB_RESETCONTENT, 0, 0);
	for (int i = 0; i < count; i++)
		SendMessageW(g.modeCombo, CB_ADDSTRING, 0, LPARAM(modes[i].label));
	SendMessageW(g.modeCombo, CB_SETCURSEL, (module == ModSuperLove) ? 1 : 0, 0);
	EnableWindow(g.modeCombo, count > 1);

	pushParams();
}


// ------------------------------------------------------------ device combos

void fillCombo(HWND combo, const std::vector<DeviceInfo>& devices, bool preferVirtualCable) {
	SendMessageW(combo, CB_RESETCONTENT, 0, 0);
	int select = -1;
	for (size_t i = 0; i < devices.size(); i++) {
		std::wstring label = devices[i].name;
		if (devices[i].isDefault)
			label += L"   [default]";
		SendMessageW(combo, CB_ADDSTRING, 0, LPARAM(label.c_str()));

		// Preselect the cable for capture and the default for playback --
		// which is the routing that actually works, so the app opens already
		// pointing at the right thing.
		if (preferVirtualCable) {
			if (devices[i].isVirtualCable && select < 0)
				select = int(i);
		}
		else if (devices[i].isDefault && select < 0) {
			select = int(i);
		}
	}
	if (select < 0 && !devices.empty())
		select = 0;
	if (select >= 0)
		SendMessageW(combo, CB_SETCURSEL, select, 0);
}

void updateBanner() {
	const bool loopback = SendMessageW(g.loopbackCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;

	bool haveCable = false;
	for (const DeviceInfo& d : g.captureDevices)
		haveCable = haveCable || d.isVirtualCable;

	if (loopback) {
		SetWindowTextW(g.banner,
		    L"Loopback mode taps a playback device, so you hear the original audio as well as "
		    L"the processed version. Use it to audition the chain; use a virtual cable for real "
		    L"interception.");
	}
	else if (!haveCable) {
		SetWindowTextW(g.banner,
		    L"No virtual audio cable found. Install VB-CABLE, set it as the Windows default "
		    L"playback device, then pick \"CABLE Output\" above \x2014 that is what stops the "
		    L"unprocessed audio reaching your speakers. See the README.");
	}
	else {
		SetWindowTextW(g.banner,
		    L"Set the virtual cable as the Windows default playback device (Settings > System > "
		    L"Sound), capture from \"CABLE Output\" here, and play to your real speakers.");
	}
}

void refreshDevices() {
	std::wstring err;
	const bool loopback = SendMessageW(g.loopbackCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;

	// Loopback taps a playback endpoint, so the source list becomes the render
	// list. Same combo, different meaning -- the banner says which.
	if (!enumerateDevices(loopback ? eRender : eCapture, g.captureDevices, err))
		MessageBoxW(g.main, err.c_str(), L"FMD Router", MB_ICONWARNING | MB_OK);
	if (!enumerateDevices(eRender, g.renderDevices, err))
		MessageBoxW(g.main, err.c_str(), L"FMD Router", MB_ICONWARNING | MB_OK);

	fillCombo(g.captureCombo, g.captureDevices, !loopback);
	fillCombo(g.renderCombo, g.renderDevices, false);
	updateBanner();
}


// ------------------------------------------------------------ start / stop

void updateStartButton() {
	SetWindowTextW(g.startButton, g.engine.running() ? L"Stop" : L"Start");
	EnableWindow(g.captureCombo, !g.engine.running());
	EnableWindow(g.renderCombo, !g.engine.running());
	EnableWindow(g.loopbackCheck, !g.engine.running());
}

void toggleEngine() {
	if (g.engine.running()) {
		g.engine.stop();
		updateStartButton();
		SetWindowTextW(g.status, L"Stopped.");
		SetWindowTextW(g.status2, L"");
		return;
	}

	const LRESULT capSel = SendMessageW(g.captureCombo, CB_GETCURSEL, 0, 0);
	const LRESULT renSel = SendMessageW(g.renderCombo, CB_GETCURSEL, 0, 0);
	if (capSel == CB_ERR || renSel == CB_ERR) {
		MessageBoxW(g.main, L"Choose both a source and a playback device first.",
		            L"FMD Router", MB_ICONINFORMATION | MB_OK);
		return;
	}

	EngineConfig cfg;
	cfg.captureId = g.captureDevices[size_t(capSel)].id;
	cfg.renderId = g.renderDevices[size_t(renSel)].id;
	cfg.loopback = SendMessageW(g.loopbackCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;

	// Loopback taps everything playing on a device, including whatever this app
	// just wrote there. Pointing both ends at one device is a feedback loop
	// that goes to full scale in well under a second, so it is refused rather
	// than warned about.
	if (cfg.loopback && cfg.captureId == cfg.renderId) {
		MessageBoxW(g.main,
		            L"Loopback cannot tap the same device it plays to \x2014 the output would be "
		            L"captured and fed back through the chain.\n\n"
		            L"Pick a different playback device, or use a virtual audio cable instead, "
		            L"which is what this app is really built for.",
		            L"FMD Router", MB_ICONWARNING | MB_OK);
		return;
	}

	// Routing a cable into a different card means two unrelated clocks, so the
	// ring needs more slack than a single-device path would.
	cfg.targetBufferMs = 30.0;

	pushParams();

	std::wstring err;
	if (!g.engine.start(cfg, err)) {
		MessageBoxW(g.main, err.c_str(), L"FMD Router \x2014 could not start", MB_ICONERROR | MB_OK);
		SetWindowTextW(g.status, L"Failed to start.");
		SetWindowTextW(g.status2, L"");
		return;
	}

	updateStartButton();
	SetWindowTextW(g.status, L"Running.");
}


// ----------------------------------------------------------------- metering

/** Linear peak -> 0..1 across a -60..0 dB scale, which is where a meter is
    actually readable. */
float meterPosition(float linear) {
	if (linear <= 0.00001f)
		return 0.f;
	const float db = 20.f * std::log10(linear);
	const float pos = (db + 60.f) / 60.f;
	return std::max(0.f, std::min(1.f, pos));
}

LRESULT CALLBACK meterProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	if (msg == WM_PAINT) {
		const int index = int(GetWindowLongPtrW(hwnd, GWLP_ID)) - IDC_METER_BASE;
		const float level = (index >= 0 && index < 3) ? g.meterLevel[index] : 0.f;

		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);

		RECT rc;
		GetClientRect(hwnd, &rc);

		HBRUSH back = CreateSolidBrush(RGB(28, 28, 32));
		FillRect(dc, &rc, back);
		DeleteObject(back);

		const int width = int((rc.right - rc.left) * meterPosition(level));
		if (width > 0) {
			RECT bar = rc;
			bar.right = rc.left + width;
			// Green until it gets loud, amber approaching clip, red at it, so a
			// glance tells you whether the chain is running hot.
			COLORREF colour = RGB(70, 190, 110);
			if (level > 0.891f)       // -1 dBFS
				colour = RGB(220, 70, 70);
			else if (level > 0.501f)  // -6 dBFS
				colour = RGB(225, 170, 60);
			HBRUSH fill = CreateSolidBrush(colour);
			FillRect(dc, &bar, fill);
			DeleteObject(fill);
		}

		FrameRect(dc, &rc, HBRUSH(GetStockObject(GRAY_BRUSH)));
		EndPaint(hwnd, &ps);
		return 0;
	}
	if (msg == WM_ERASEBKGND)
		return 1;  // fully painted in WM_PAINT; erasing first would flicker
	return DefWindowProcW(hwnd, msg, wp, lp);
}

void onTimer() {
	const EngineStats s = g.engine.stats();

	const float peaks[3] = {s.inPeakL, s.inPeakR, s.outPeak};
	for (int i = 0; i < 3; i++) {
		// Rise instantly to a new peak, fall gently, so transients are visible
		// but the bar does not strobe.
		g.meterLevel[i] = std::max(peaks[i], g.meterLevel[i] * 0.82f);
		if (g.meterLevel[i] < 0.00001f)
			g.meterLevel[i] = 0.f;
		InvalidateRect(g.meters[i], nullptr, FALSE);
	}

	const std::wstring asyncError = g.engine.takeAsyncError();
	if (!asyncError.empty()) {
		g.engine.stop();
		updateStartButton();
		SetWindowTextW(g.status, L"Stopped after a device error.");
		SetWindowTextW(g.status2, L"");
		MessageBoxW(g.main, asyncError.c_str(), L"FMD Router \x2014 audio stopped",
		            MB_ICONERROR | MB_OK);
		return;
	}

	if (!s.running)
		return;  // the Start/Stop handlers own the labels while stopped

	wchar_t buf[256];
	swprintf_s(buf, L"in %u Hz / %u ch   \x2192   out %u Hz / %u ch        "
	                L"device period %.1f / %.1f ms",
	           s.captureRate, s.captureChannels, s.renderRate, s.renderChannels,
	           s.capturePeriodMs, s.renderPeriodMs);
	SetWindowTextW(g.status, buf);

	swprintf_s(buf, L"buffer %.1f ms        round trip ~%.1f ms        "
	                L"resample %.4f\x00D7        drops %llu",
	           s.ringMs, s.roundTripMs, s.ratio,
	           static_cast<unsigned long long>(s.underruns + s.overruns));
	SetWindowTextW(g.status2, buf);
}


// ------------------------------------------------------------ construction

HWND child(const wchar_t* cls, const wchar_t* text, DWORD style,
           int x, int y, int w, int h, int id) {
	HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
	                            x, y, w, h, g.main, HMENU(INT_PTR(id)),
	                            HINSTANCE(GetWindowLongPtrW(g.main, GWLP_HINSTANCE)), nullptr);
	if (hwnd && g.font)
		SendMessageW(hwnd, WM_SETFONT, WPARAM(g.font), TRUE);
	return hwnd;
}

void buildUi() {
	g.font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
	                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
	                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

	child(L"BUTTON", L" Routing ", BS_GROUPBOX, 12, 8, 636, 176, -1);

	child(L"STATIC", L"Take audio from:", SS_LEFT, 26, 36, 120, 20, -1);
	g.captureCombo = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
	                       152, 32, 480, 320, IDC_CAPTURE_COMBO);

	g.loopbackCheck = child(L"BUTTON", L"Loopback \x2014 tap a playback device (you also hear "
	                                   L"the original)",
	                        BS_AUTOCHECKBOX | WS_TABSTOP, 152, 62, 480, 22, IDC_LOOPBACK);

	child(L"STATIC", L"Play processed to:", SS_LEFT, 26, 94, 120, 20, -1);
	g.renderCombo = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
	                      152, 90, 480, 320, IDC_RENDER_COMBO);

	// Three lines: the banner carries the routing instructions, and truncating
	// those is exactly the wrong thing to save 18 pixels on.
	g.banner = child(L"STATIC", L"", SS_LEFT, 26, 118, 606, 58, IDC_BANNER);

	child(L"BUTTON", L" Chain ", BS_GROUPBOX, 12, 192, 636, 330, -1);

	child(L"STATIC", L"Module:", SS_LEFT, 26, 221, 60, 20, -1);
	g.moduleCombo = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
	                      90, 217, 190, 200, IDC_MODULE);
	for (const wchar_t* name : kModules)
		SendMessageW(g.moduleCombo, CB_ADDSTRING, 0, LPARAM(name));
	SendMessageW(g.moduleCombo, CB_SETCURSEL, 0, 0);

	child(L"STATIC", L"Mode:", SS_LEFT, 300, 221, 46, 20, -1);
	g.modeCombo = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
	                    350, 217, 150, 200, IDC_MODE);

	g.aggrCheck = child(L"BUTTON", L"AGGR", BS_AUTOCHECKBOX | WS_TABSTOP,
	                    516, 219, 100, 22, IDC_AGGR);

	for (int i = 0; i < SL_COUNT; i++) {
		const int y = 254 + i * 32;
		g.sliderNames[i] = child(L"STATIC", kSliders[i].name, SS_LEFT, 26, y + 4, 62, 20,
		                         IDC_SLIDER_NAME_BASE + i);
		g.sliders[i] = child(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
		                     92, y, 420, 26, IDC_SLIDER_BASE + i);
		SendMessageW(g.sliders[i], TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
		SendMessageW(g.sliders[i], TBM_SETPAGESIZE, 0, 50);
		SendMessageW(g.sliders[i], TBM_SETPOS, TRUE, kSliders[i].defaultValue);
		g.sliderValues[i] = child(L"STATIC", L"", SS_LEFT, 522, y + 4, 110, 20,
		                          IDC_SLIDER_VALUE_BASE + i);
	}

	child(L"BUTTON", L" Levels ", BS_GROUPBOX, 12, 530, 636, 142, -1);

	const wchar_t* meterNames[3] = {L"IN  L", L"IN  R", L"OUT"};
	for (int i = 0; i < 3; i++) {
		const int y = 556 + i * 24;
		child(L"STATIC", meterNames[i], SS_LEFT, 26, y + 1, 48, 20, -1);
		g.meters[i] = child(L"FmdMeter", L"", 0, 80, y, 552, 16, IDC_METER_BASE + i);
	}

	// Two lines: formats and device periods on the first, the live numbers on
	// the second. One line ellipsized the part worth reading.
	g.status = child(L"STATIC", L"Stopped.", SS_LEFT | SS_ENDELLIPSIS,
	                 26, 626, 606, 20, IDC_STATUS);
	g.status2 = child(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS,
	                  26, 646, 606, 20, IDC_STATUS2);

	child(L"BUTTON", L"Refresh devices", BS_PUSHBUTTON | WS_TABSTOP,
	      12, 682, 150, 30, IDC_REFRESH);
	g.startButton = child(L"BUTTON", L"Start", BS_DEFPUSHBUTTON | WS_TABSTOP,
	                      498, 682, 150, 30, IDC_START);
}


LRESULT CALLBACK mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
		case WM_CREATE:
			g.main = hwnd;
			buildUi();
			refreshDevices();
			applyModuleToUi();
			refreshSliderLabels();
			SetTimer(hwnd, IDT_REFRESH, 33, nullptr);
			return 0;

		case WM_COMMAND: {
			const int id = LOWORD(wp);
			const int code = HIWORD(wp);
			if (id == IDC_REFRESH && code == BN_CLICKED) {
				refreshDevices();
				return 0;
			}
			if (id == IDC_START && code == BN_CLICKED) {
				toggleEngine();
				return 0;
			}
			if (id == IDC_LOOPBACK && code == BN_CLICKED) {
				refreshDevices();
				return 0;
			}
			if (id == IDC_MODULE && code == CBN_SELCHANGE) {
				applyModuleToUi();
				return 0;
			}
			if ((id == IDC_MODE && code == CBN_SELCHANGE)
			    || (id == IDC_AGGR && code == BN_CLICKED)) {
				pushParams();
				return 0;
			}
			break;
		}

		case WM_HSCROLL:
			// Every trackbar reports here; the control handle in lParam says
			// which one, and it is cheaper to just republish all of them than
			// to work out the index and republish one.
			if (lp) {
				pushParams();
				refreshSliderLabels();
			}
			return 0;

		case WM_CTLCOLORSTATIC:
			// This is a plain window rather than a dialog, so labels, group
			// boxes and trackbar backgrounds do not get the dialog face colour
			// for free -- without this they paint on white rectangles.
			SetBkMode(HDC(wp), TRANSPARENT);
			return LRESULT(GetSysColorBrush(COLOR_BTNFACE));

		case WM_TIMER:
			if (wp == IDT_REFRESH)
				onTimer();
			return 0;

		case WM_CLOSE:
			// Stop the audio threads before the window goes away: they publish
			// into g, and g outliving them is the only ordering that is safe.
			g.engine.stop();
			DestroyWindow(hwnd);
			return 0;

		case WM_DESTROY:
			KillTimer(hwnd, IDT_REFRESH);
			PostQuitMessage(0);
			return 0;

		default:
			break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace


int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
	// The GUI thread only enumerates devices; the audio threads make their own
	// MTA. An STA here is the conventional choice for a window thread.
	const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
	InitCommonControlsEx(&icc);

	WNDCLASSEXW meterClass = {};
	meterClass.cbSize = sizeof(meterClass);
	meterClass.lpfnWndProc = meterProc;
	meterClass.hInstance = instance;
	meterClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	meterClass.lpszClassName = L"FmdMeter";
	RegisterClassExW(&meterClass);

	WNDCLASSEXW mainClass = {};
	mainClass.cbSize = sizeof(mainClass);
	mainClass.lpfnWndProc = mainProc;
	mainClass.hInstance = instance;
	mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	mainClass.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
	mainClass.lpszClassName = L"FmdRouterMain";
	if (!RegisterClassExW(&mainClass)) {
		MessageBoxW(nullptr, L"Could not register the window class.", L"FMD Router",
		            MB_ICONERROR | MB_OK);
		return 1;
	}

	// Fixed size: the layout is absolute, and a resizable window that does not
	// re-lay-out is worse than one that cannot be resized.
	const DWORD style = (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));
	RECT wanted = {0, 0, 660, 724};
	AdjustWindowRect(&wanted, style, FALSE);

	HWND window = CreateWindowExW(0, mainClass.lpszClassName, L"FMD Router \x2014 stereo to mono",
	                              style, CW_USEDEFAULT, CW_USEDEFAULT,
	                              wanted.right - wanted.left, wanted.bottom - wanted.top,
	                              nullptr, nullptr, instance, nullptr);
	if (!window) {
		MessageBoxW(nullptr, L"Could not create the window.", L"FMD Router", MB_ICONERROR | MB_OK);
		return 1;
	}

	ShowWindow(window, show);
	UpdateWindow(window);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		// IsDialogMessage gives tab traversal and arrow keys on the trackbars.
		if (!IsDialogMessageW(window, &msg)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	if (g.font)
		DeleteObject(g.font);
	if (SUCCEEDED(comHr))
		CoUninitialize();
	return int(msg.wParam);
}
