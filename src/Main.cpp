/*  Real Mono Sound -- Win32 front end.

    The default screen is deliberately small: pick the two devices, press
    Start, and the only three things that change the sound are a preset, the
    global enable and highest quality mode. That is the control density of the
    demo the client asked the interface to match.

    Everything the brief calls a lab or developer control -- the per-stage
    bypasses, the Stage 1 high-pass, the Hilbert quality, the Mid/Side gains
    and the meters -- lives behind the Advanced button, where it can be
    reached without being in the way. No stage is code-only: every one of the
    five has a visible bypass. */

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

const wchar_t* const kAppName = L"Real Mono Sound";

enum : int {
	IDC_CAPTURE_COMBO = 1001,
	IDC_RENDER_COMBO = 1002,
	IDC_LOOPBACK = 1003,
	IDC_REFRESH = 1004,
	IDC_START = 1005,
	IDC_BANNER = 1006,
	IDC_STATUS = 1007,
	IDC_STATUS2 = 1008,

	IDC_PRESET = 1010,
	IDC_ADVANCED = 1011,
	IDC_ENABLE = 1012,
	IDC_HQ = 1013,

	IDC_S1_ENABLE = 1020,
	IDC_S1_SLOPE = 1021,
	IDC_S1_MODE = 1022,
	IDC_S2_ENABLE = 1023,
	IDC_S3_ENABLE = 1024,
	IDC_S3_QUALITY = 1025,
	IDC_S4_ENABLE = 1026,
	IDC_S4_MODE = 1027,
	IDC_S5_ENABLE = 1028,

	IDC_READOUT = 1030,
	IDC_LATENCY = 1031,

	IDC_SLIDER_BASE = 1100,        // see kSliders
	IDC_SLIDER_VALUE_BASE = 1120,
	IDC_METER_BASE = 1160,         // four: in L, in R, out, GR

	IDT_REFRESH = 1,
};

enum SliderIndex {
	SL_CEILING = 0,
	SL_HPF_FC,
	SL_MID,
	SL_SIDE,
	SL_INJECT,
	SL_OUTPUT,
	SL_LOOKAHEAD,
	SL_COUNT,
};

/** Sliders are all 0..1000 in the control and mapped to their real range here,
    so the ranges in the brief's parameter tables appear once, in one place. */
struct SliderDef {
	float min;
	float max;
	const wchar_t* unit;
	int decimals;
};

const SliderDef kSliders[SL_COUNT] = {
	{-3.f,   0.f,  L" dB", 1},   // ceiling      -3..0
	{40.f,   400.f, L" Hz", 0},  // Stage 1 fc   40..400
	{-24.f,  6.f,  L" dB", 1},   // mid gain     -24..+6
	{-24.f,  6.f,  L" dB", 1},   // side gain    -24..+6
	{-12.f,  6.f,  L" dB", 1},   // side inject  -12..+6
	{-24.f,  12.f, L" dB", 1},   // output gain  -24..+12
	{5.f,    10.f, L" ms", 1},   // look-ahead   5..10
};

const wchar_t* const kQualityNames[NumQualities] = {
	L"Linear phase, HQ (1023 taps)",
	L"Linear phase, short (255 taps)",
	L"Allpass IIR (lowest latency)",
};

const wchar_t* const kCommitNames[NumCommitModes] = {
	L"Mid + rotated Side  (product)",
	L"Sum 0.5(L+R)  (reference)",
	L"Mid only  (Side discarded)",
	L"Side energy fold  (lab)",
	L"Polarity matrix  (lab)",
};

struct SlopeOption {
	const wchar_t* label;
	int slope;
};

const SlopeOption kSlopes[] = {
	{L"6 dB/oct", 6},
	{L"12 dB/oct", 12},
	{L"24 dB/oct", 24},
};

struct HpfModeOption {
	const wchar_t* label;
	int mode;
};

const HpfModeOption kHpfModes[] = {
	{L"Side high-pass (Butterworth)", HpfSide},
	{L"Crossover to mono (LR)", HpfCrossoverMono},
};


struct App {
	HWND main = nullptr;
	HFONT font = nullptr;

	HWND captureCombo = nullptr;
	HWND renderCombo = nullptr;
	HWND loopbackCheck = nullptr;
	HWND startButton = nullptr;
	HWND advancedButton = nullptr;
	HWND banner = nullptr;
	HWND status = nullptr;
	HWND status2 = nullptr;

	HWND presetCombo = nullptr;
	HWND enableCheck = nullptr;
	HWND hqCheck = nullptr;

	HWND s1Enable = nullptr;
	HWND s1Slope = nullptr;
	HWND s1Mode = nullptr;
	HWND s2Enable = nullptr;
	HWND s3Enable = nullptr;
	HWND s3Quality = nullptr;
	HWND s4Enable = nullptr;
	HWND s4Mode = nullptr;
	HWND s5Enable = nullptr;
	HWND readout = nullptr;
	HWND latency = nullptr;

	HWND sliders[SL_COUNT] = {};
	HWND sliderValues[SL_COUNT] = {};
	HWND meters[4] = {};

	// Every control that belongs to the Advanced page, so one loop shows or
	// hides the lot.
	std::vector<HWND> advanced;
	bool advancedShown = false;

	std::vector<DeviceInfo> captureDevices;
	std::vector<DeviceInfo> renderDevices;

	Engine engine;

	// Meter state lives on the GUI side: the audio thread reports a peak and
	// forgets it, and the decay that makes a meter readable is a display
	// concern with no business on a real-time thread. Index 3 is gain
	// reduction, held as a positive number of dB.
	float meterLevel[4] = {0.f, 0.f, 0.f, 0.f};
};

App g;

const int kBaseHeight = 604;
const int kAdvancedHeight = 924;


// ---------------------------------------------------------------- utilities

float sliderValue(int index) {
	const SliderDef& d = kSliders[index];
	const float norm = float(SendMessageW(g.sliders[index], TBM_GETPOS, 0, 0)) / 1000.f;
	return d.min + norm * (d.max - d.min);
}

void setSlider(int index, float value) {
	const SliderDef& d = kSliders[index];
	const float norm = (value - d.min) / (d.max - d.min);
	const int pos = int(std::max(0.f, std::min(1.f, norm)) * 1000.f + 0.5f);
	SendMessageW(g.sliders[index], TBM_SETPOS, TRUE, pos);
}

bool checked(HWND control) {
	return SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void setChecked(HWND control, bool on) {
	SendMessageW(control, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

int comboSelection(HWND combo, int fallback = 0) {
	const LRESULT sel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
	return (sel == CB_ERR) ? fallback : int(sel);
}

std::wstring formatDb(float db) {
	wchar_t buf[32];
	swprintf_s(buf, L"%+.1f dB", db);
	return buf;
}


// ------------------------------------------------------- parameter plumbing

/** The UI is the single source of truth for the settings: reading it back is
    how the audio thread and every readout stay in step with what is on
    screen. */
RealMonoSettings uiSettings() {
	RealMonoSettings s;
	s.enabled = checked(g.enableCheck);
	s.hqMode = checked(g.hqCheck);

	s.hpfEnabled = checked(g.s1Enable);
	s.hpfHz = sliderValue(SL_HPF_FC);
	s.hpfSlope = kSlopes[comboSelection(g.s1Slope, 1)].slope;
	s.hpfMode = kHpfModes[comboSelection(g.s1Mode, 0)].mode;

	s.msEnabled = checked(g.s2Enable);
	s.midGainDb = sliderValue(SL_MID);
	s.sideGainDb = sliderValue(SL_SIDE);

	s.rotateEnabled = checked(g.s3Enable);
	s.quality = comboSelection(g.s3Quality, QualityHqLinear);

	s.commitEnabled = checked(g.s4Enable);
	s.commitMode = comboSelection(g.s4Mode, CommitMidPlusRotatedSide);
	s.sideInjectDb = sliderValue(SL_INJECT);
	s.outputGainDb = sliderValue(SL_OUTPUT);

	s.limiterEnabled = checked(g.s5Enable);
	s.ceilingDb = sliderValue(SL_CEILING);
	s.lookaheadMs = sliderValue(SL_LOOKAHEAD);
	return s;
}

int slopeIndex(int slope) {
	for (int i = 0; i < int(std::size(kSlopes)); i++) {
		if (kSlopes[i].slope == slope)
			return i;
	}
	return 1;
}

int hpfModeIndex(int mode) {
	for (int i = 0; i < int(std::size(kHpfModes)); i++) {
		if (kHpfModes[i].mode == mode)
			return i;
	}
	return 0;
}

void refreshValueLabels() {
	for (int i = 0; i < SL_COUNT; i++) {
		const SliderDef& d = kSliders[i];
		wchar_t buf[64];
		const float v = sliderValue(i);
		if (d.decimals == 0)
			swprintf_s(buf, L"%.0f%s", v, d.unit);
		else if (i == SL_HPF_FC)
			swprintf_s(buf, L"%.0f%s", v, d.unit);
		else
			swprintf_s(buf, L"%+.1f%s", v, d.unit);
		SetWindowTextW(g.sliderValues[i], buf);
	}
}

/** A control whose stage is bypassed greys out, so the panel cannot show a Mid
    gain that is doing nothing without saying so. Stages stay adjustable while
    the global enable is off, because setting the chain up and then switching
    it in is exactly how the A/B is run. */
void refreshEnableStates() {
	const bool hpf = checked(g.s1Enable);
	EnableWindow(g.sliders[SL_HPF_FC], hpf);
	EnableWindow(g.sliderValues[SL_HPF_FC], hpf);
	EnableWindow(g.s1Slope, hpf);
	EnableWindow(g.s1Mode, hpf);

	const bool ms = checked(g.s2Enable);
	EnableWindow(g.sliders[SL_MID], ms);
	EnableWindow(g.sliderValues[SL_MID], ms);
	EnableWindow(g.sliders[SL_SIDE], ms);
	EnableWindow(g.sliderValues[SL_SIDE], ms);

	// Stage 3's quality is not greyed with its bypass: it sets the group delay
	// the Mid path is aligned to either way, so it is live even when the
	// rotation is switched out.

	const bool commit = checked(g.s4Enable);
	EnableWindow(g.s4Mode, commit);

	const bool limiter = checked(g.s5Enable);
	EnableWindow(g.sliders[SL_CEILING], limiter);
	EnableWindow(g.sliderValues[SL_CEILING], limiter);
	EnableWindow(g.sliders[SL_LOOKAHEAD], limiter);
	EnableWindow(g.sliderValues[SL_LOOKAHEAD], limiter);
}

/** Rate comes from the caller because EngineStats reads and clears the peak
    meters -- fetching it here would steal them from the meter timer. */
void updateLatencyLabel(uint32_t renderRate) {
	const RealMonoSettings s = uiSettings();
	const double rate = (renderRate > 0) ? double(renderRate) : 48000.0;

	const int rotation = s.enabled ? RealMonoChain::rotationDelay(s.quality) : 0;
	const double rotationMs = 1000.0 * double(rotation) / rate;
	const double lookMs = (s.enabled && s.limiterEnabled) ? double(s.lookaheadMs) : 0.0;

	wchar_t buf[256];
	swprintf_s(buf,
	           L"Latency: rotation %d smp (%.2f ms) + look-ahead %.2f ms = %.2f ms @ %.0f Hz",
	           rotation, rotationMs, lookMs, rotationMs + lookMs, rate);
	SetWindowTextW(g.latency, buf);
}

void pushParams() {
	g.engine.params.store(uiSettings());
	refreshEnableStates();
}

void applySettingsToUi(const RealMonoSettings& s) {
	setChecked(g.enableCheck, s.enabled);
	setChecked(g.hqCheck, s.hqMode);

	setChecked(g.s1Enable, s.hpfEnabled);
	setSlider(SL_HPF_FC, s.hpfHz);
	SendMessageW(g.s1Slope, CB_SETCURSEL, slopeIndex(s.hpfSlope), 0);
	SendMessageW(g.s1Mode, CB_SETCURSEL, hpfModeIndex(s.hpfMode), 0);

	setChecked(g.s2Enable, s.msEnabled);
	setSlider(SL_MID, s.midGainDb);
	setSlider(SL_SIDE, s.sideGainDb);

	setChecked(g.s3Enable, s.rotateEnabled);
	SendMessageW(g.s3Quality, CB_SETCURSEL, s.quality, 0);

	setChecked(g.s4Enable, s.commitEnabled);
	SendMessageW(g.s4Mode, CB_SETCURSEL, s.commitMode, 0);
	setSlider(SL_INJECT, s.sideInjectDb);
	setSlider(SL_OUTPUT, s.outputGainDb);

	setChecked(g.s5Enable, s.limiterEnabled);
	setSlider(SL_CEILING, s.ceilingDb);
	setSlider(SL_LOOKAHEAD, s.lookaheadMs);

	refreshValueLabels();
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
	const bool loopback = checked(g.loopbackCheck);

	bool haveCable = false;
	for (const DeviceInfo& d : g.captureDevices)
		haveCable = haveCable || d.isVirtualCable;

	if (loopback) {
		SetWindowTextW(g.banner,
		    L"Loopback mode taps a playback device, so you hear the original audio as well as "
		    L"the mono version. Use it to audition the chain; use a virtual cable for real "
		    L"interception.");
	}
	else if (!haveCable) {
		SetWindowTextW(g.banner,
		    L"No virtual audio cable found. Install VB-CABLE, set it as the Windows default "
		    L"playback device, then pick \"CABLE Output\" above \x2014 that is what stops the "
		    L"unprocessed stereo reaching your speakers. See the README.");
	}
	else {
		SetWindowTextW(g.banner,
		    L"Set the virtual cable as the Windows default playback device (Settings > System > "
		    L"Sound), capture from \"CABLE Output\" here, and play to your real speakers.");
	}
}

void refreshDevices() {
	std::wstring err;
	const bool loopback = checked(g.loopbackCheck);

	// Loopback taps a playback endpoint, so the source list becomes the render
	// list. Same combo, different meaning -- the banner says which.
	if (!enumerateDevices(loopback ? eRender : eCapture, g.captureDevices, err))
		MessageBoxW(g.main, err.c_str(), kAppName, MB_ICONWARNING | MB_OK);
	if (!enumerateDevices(eRender, g.renderDevices, err))
		MessageBoxW(g.main, err.c_str(), kAppName, MB_ICONWARNING | MB_OK);

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
		            kAppName, MB_ICONINFORMATION | MB_OK);
		return;
	}

	EngineConfig cfg;
	cfg.captureId = g.captureDevices[size_t(capSel)].id;
	cfg.renderId = g.renderDevices[size_t(renSel)].id;
	cfg.loopback = checked(g.loopbackCheck);

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
		            kAppName, MB_ICONWARNING | MB_OK);
		return;
	}

	// Routing a cable into a different card means two unrelated clocks, so the
	// ring needs more slack than a single-device path would.
	cfg.targetBufferMs = 30.0;

	pushParams();

	std::wstring err;
	if (!g.engine.start(cfg, err)) {
		MessageBoxW(g.main, err.c_str(), L"Real Mono Sound \x2014 could not start",
		            MB_ICONERROR | MB_OK);
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
		const float level = (index >= 0 && index < 4) ? g.meterLevel[index] : 0.f;
		const bool isGr = (index == 3);

		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);

		RECT rc;
		GetClientRect(hwnd, &rc);

		HBRUSH back = CreateSolidBrush(RGB(28, 28, 32));
		FillRect(dc, &rc, back);
		DeleteObject(back);

		// Gain reduction is already in dB and grows downwards, so it gets its
		// own 0..20 dB scale rather than the signal meters' -60..0.
		const float position = isGr ? std::min(1.f, level / 20.f) : meterPosition(level);
		const int width = int(float(rc.right - rc.left) * position);
		if (width > 0) {
			RECT bar = rc;
			bar.right = rc.left + width;
			COLORREF colour = isGr ? RGB(225, 170, 60) : RGB(70, 190, 110);
			if (!isGr) {
				// Green until it gets loud, amber approaching clip, red at it,
				// so a glance tells you whether the chain is running hot.
				if (level > 0.891f)       // -1 dBFS
					colour = RGB(220, 70, 70);
				else if (level > 0.501f)  // -6 dBFS
					colour = RGB(225, 170, 60);
			}
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

	const float peaks[4] = {s.inPeakL, s.inPeakR, s.outPeak, -s.gainReductionDb};
	for (int i = 0; i < 4; i++) {
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
		MessageBoxW(g.main, asyncError.c_str(), L"Real Mono Sound \x2014 audio stopped",
		            MB_ICONERROR | MB_OK);
		return;
	}

	if (g.advancedShown) {
		updateLatencyLabel(s.renderRate);

		wchar_t buf[256];
		swprintf_s(buf, L"correlation %+.2f        Mid %s        Side %s        limiter GR %.1f dB",
		           s.correlation,
		           formatDb(s.midPeak > 0.f ? 20.f * std::log10(s.midPeak) : -90.f).c_str(),
		           formatDb(s.sidePeak > 0.f ? 20.f * std::log10(s.sidePeak) : -90.f).c_str(),
		           s.gainReductionDb);
		SetWindowTextW(g.readout, buf);
	}

	if (!s.running)
		return;  // the Start/Stop handlers own the labels while stopped

	wchar_t buf[256];
	swprintf_s(buf, L"in %u Hz / %u ch   \x2192   out %u Hz / %u ch        "
	                L"device period %.1f / %.1f ms",
	           s.captureRate, s.captureChannels, s.renderRate, s.renderChannels,
	           s.capturePeriodMs, s.renderPeriodMs);
	SetWindowTextW(g.status, buf);

	swprintf_s(buf, L"buffer %.1f ms    chain %.1f ms    round trip ~%.1f ms    "
	                L"resample %.4f\x00D7    drops %llu",
	           s.ringMs, s.processingMs, s.roundTripMs, s.ratio,
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

/** Same, but remembered so the Advanced toggle can show and hide it. */
HWND advChild(const wchar_t* cls, const wchar_t* text, DWORD style,
              int x, int y, int w, int h, int id) {
	HWND hwnd = child(cls, text, style, x, y, w, h, id);
	if (hwnd)
		g.advanced.push_back(hwnd);
	return hwnd;
}

HWND makeSlider(int index, int x, int y, int w, bool advancedPage) {
	const int id = IDC_SLIDER_BASE + index;
	HWND hwnd = advancedPage
	          ? advChild(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x, y, w, 26, id)
	          : child(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x, y, w, 26, id);
	SendMessageW(hwnd, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
	SendMessageW(hwnd, TBM_SETPAGESIZE, 0, 50);
	g.sliders[index] = hwnd;
	return hwnd;
}

void buildRouting() {
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
}

void buildMainPanel() {
	child(L"BUTTON", L" Real Mono ", BS_GROUPBOX, 12, 192, 636, 176, -1);

	child(L"STATIC", L"Preset:", SS_LEFT, 26, 222, 56, 20, -1);
	g.presetCombo = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
	                      90, 218, 230, 200, IDC_PRESET);
	for (int i = 0; i < NumPresets; i++)
		SendMessageW(g.presetCombo, CB_ADDSTRING, 0, LPARAM(presetName(i)));
	SendMessageW(g.presetCombo, CB_SETCURSEL, PresetRealMonoDefault, 0);

	g.advancedButton = child(L"BUTTON", L"Advanced \x25BC", BS_PUSHBUTTON | WS_TABSTOP,
	                         470, 217, 160, 28, IDC_ADVANCED);

	g.enableCheck = child(L"BUTTON", L"Real Mono processing   (off = stereo pass-through)",
	                      BS_AUTOCHECKBOX | WS_TABSTOP, 26, 256, 440, 22, IDC_ENABLE);
	g.hqCheck = child(L"BUTTON", L"Highest quality mode   (\x2212""3 dB input, keeps the "
	                             L"dynamics under the sum)",
	                  BS_AUTOCHECKBOX | WS_TABSTOP, 26, 284, 500, 22, IDC_HQ);

	child(L"STATIC", L"Ceiling", SS_LEFT, 26, 316, 60, 20, -1);
	makeSlider(SL_CEILING, 90, 312, 300, false);
	g.sliderValues[SL_CEILING] = child(L"STATIC", L"", SS_LEFT, 400, 316, 90, 20,
	                                   IDC_SLIDER_VALUE_BASE + SL_CEILING);

	// One line, and it has to fit on one line: a static this tall clips rather
	// than wraps, and the half that would be lost is the half that says why.
	child(L"STATIC",
	      L"Difference (Side) content is heard in mono instead of cancelling to silence.",
	      SS_LEFT, 26, 342, 606, 18, -1);
}

void buildMeters() {
	child(L"BUTTON", L" Levels ", BS_GROUPBOX, 12, 376, 636, 130, -1);

	const wchar_t* meterNames[4] = {L"IN  L", L"IN  R", L"OUT", L"GR"};
	for (int i = 0; i < 4; i++) {
		const int y = 400 + i * 24;
		child(L"STATIC", meterNames[i], SS_LEFT, 26, y + 1, 48, 20, -1);
		g.meters[i] = child(L"RealMonoMeter", L"", 0, 80, y, 552, 16, IDC_METER_BASE + i);
	}
}

void buildAdvanced() {
	advChild(L"BUTTON", L" Advanced \x2014 per-stage bypass ", BS_GROUPBOX,
	         12, 600, 636, 316, -1);

	// One line that fits on one line: this static clips rather than wraps.
	advChild(L"STATIC",
	         L"Defaults follow the client's DAW recipe. Stage 1 is off because it has none.",
	         SS_LEFT, 26, 620, 606, 18, -1);

	// ------------------------------------------------------------- Stage 1
	g.s1Enable = advChild(L"BUTTON", L"Stage 1   Side high-pass   (lab, off)",
	                      BS_AUTOCHECKBOX | WS_TABSTOP, 26, 644, 300, 22, IDC_S1_ENABLE);
	advChild(L"STATIC", L"Fc", SS_LEFT, 340, 646, 24, 20, -1);
	makeSlider(SL_HPF_FC, 366, 642, 160, true);
	g.sliderValues[SL_HPF_FC] = advChild(L"STATIC", L"", SS_LEFT, 534, 646, 90, 20,
	                                     IDC_SLIDER_VALUE_BASE + SL_HPF_FC);

	advChild(L"STATIC", L"Slope:", SS_LEFT, 46, 672, 46, 20, -1);
	g.s1Slope = advChild(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
	                     96, 668, 100, 200, IDC_S1_SLOPE);
	for (const SlopeOption& o : kSlopes)
		SendMessageW(g.s1Slope, CB_ADDSTRING, 0, LPARAM(o.label));

	advChild(L"STATIC", L"Shape:", SS_LEFT, 212, 672, 50, 20, -1);
	g.s1Mode = advChild(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
	                    266, 668, 240, 200, IDC_S1_MODE);
	for (const HpfModeOption& o : kHpfModes)
		SendMessageW(g.s1Mode, CB_ADDSTRING, 0, LPARAM(o.label));

	// ------------------------------------------------------------- Stage 2
	g.s2Enable = advChild(L"BUTTON", L"Stage 2   Mid / Side split",
	                      BS_AUTOCHECKBOX | WS_TABSTOP, 26, 700, 240, 22, IDC_S2_ENABLE);
	advChild(L"STATIC", L"Mid", SS_LEFT, 280, 702, 34, 20, -1);
	makeSlider(SL_MID, 316, 698, 160, true);
	g.sliderValues[SL_MID] = advChild(L"STATIC", L"", SS_LEFT, 484, 702, 90, 20,
	                                  IDC_SLIDER_VALUE_BASE + SL_MID);

	advChild(L"STATIC", L"Side", SS_LEFT, 280, 728, 34, 20, -1);
	makeSlider(SL_SIDE, 316, 724, 160, true);
	g.sliderValues[SL_SIDE] = advChild(L"STATIC", L"", SS_LEFT, 484, 728, 90, 20,
	                                   IDC_SLIDER_VALUE_BASE + SL_SIDE);

	// ------------------------------------------------------------- Stage 3
	g.s3Enable = advChild(L"BUTTON", L"Stage 3   +90\x00B0 rotation on Side",
	                      BS_AUTOCHECKBOX | WS_TABSTOP, 26, 756, 250, 22, IDC_S3_ENABLE);
	advChild(L"STATIC", L"Quality:", SS_LEFT, 288, 758, 60, 20, -1);
	g.s3Quality = advChild(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
	                       352, 754, 272, 200, IDC_S3_QUALITY);
	for (const wchar_t* name : kQualityNames)
		SendMessageW(g.s3Quality, CB_ADDSTRING, 0, LPARAM(name));

	// ------------------------------------------------------------- Stage 4
	g.s4Enable = advChild(L"BUTTON", L"Stage 4   Mono commit",
	                      BS_AUTOCHECKBOX | WS_TABSTOP, 26, 784, 200, 22, IDC_S4_ENABLE);
	advChild(L"STATIC", L"Mode:", SS_LEFT, 240, 786, 46, 20, -1);
	g.s4Mode = advChild(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
	                    290, 782, 250, 200, IDC_S4_MODE);
	for (const wchar_t* name : kCommitNames)
		SendMessageW(g.s4Mode, CB_ADDSTRING, 0, LPARAM(name));

	advChild(L"STATIC", L"Side inject", SS_LEFT, 26, 814, 76, 20, -1);
	makeSlider(SL_INJECT, 106, 810, 130, true);
	g.sliderValues[SL_INJECT] = advChild(L"STATIC", L"", SS_LEFT, 242, 814, 80, 20,
	                                     IDC_SLIDER_VALUE_BASE + SL_INJECT);

	advChild(L"STATIC", L"Output", SS_LEFT, 340, 814, 56, 20, -1);
	makeSlider(SL_OUTPUT, 400, 810, 130, true);
	g.sliderValues[SL_OUTPUT] = advChild(L"STATIC", L"", SS_LEFT, 536, 814, 88, 20,
	                                     IDC_SLIDER_VALUE_BASE + SL_OUTPUT);

	// ------------------------------------------------------------- Stage 5
	g.s5Enable = advChild(L"BUTTON", L"Stage 5   Look-ahead limiter",
	                      BS_AUTOCHECKBOX | WS_TABSTOP, 26, 842, 240, 22, IDC_S5_ENABLE);
	advChild(L"STATIC", L"Look-ahead", SS_LEFT, 280, 844, 80, 20, -1);
	makeSlider(SL_LOOKAHEAD, 364, 840, 160, true);
	g.sliderValues[SL_LOOKAHEAD] = advChild(L"STATIC", L"", SS_LEFT, 532, 844, 90, 20,
	                                        IDC_SLIDER_VALUE_BASE + SL_LOOKAHEAD);

	g.readout = advChild(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 26, 870, 606, 20, IDC_READOUT);
	g.latency = advChild(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 26, 890, 606, 20, IDC_LATENCY);
}

void showAdvanced(bool show) {
	g.advancedShown = show;
	for (HWND control : g.advanced)
		ShowWindow(control, show ? SW_SHOW : SW_HIDE);
	SetWindowTextW(g.advancedButton, show ? L"Advanced \x25B2" : L"Advanced \x25BC");

	const DWORD style = DWORD(GetWindowLongPtrW(g.main, GWL_STYLE));
	RECT wanted = {0, 0, 660, show ? kAdvancedHeight : kBaseHeight};
	AdjustWindowRect(&wanted, style, FALSE);
	const int width = wanted.right - wanted.left;
	const int height = wanted.bottom - wanted.top;

	// Growing downwards off the bottom of the screen would hide the stage the
	// user just went looking for, so if the taller window does not fit where
	// this one is, it is walked back up until it does.
	RECT work = {};
	int x = 0, y = 0;
	UINT flags = SWP_NOMOVE | SWP_NOZORDER;
	RECT current = {};
	if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) && GetWindowRect(g.main, &current)) {
		x = current.left;
		y = current.top;
		if (y + height > work.bottom)
			y = std::max(int(work.top), int(work.bottom) - height);
		if (x + width > work.right)
			x = std::max(int(work.left), int(work.right) - width);
		flags = SWP_NOZORDER;
	}
	SetWindowPos(g.main, nullptr, x, y, width, height, flags);
}

void buildUi() {
	g.font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
	                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
	                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

	buildRouting();
	buildMainPanel();
	buildMeters();

	// Two lines: formats and device periods on the first, the live numbers on
	// the second. One line ellipsized the part worth reading.
	g.status = child(L"STATIC", L"Stopped.", SS_LEFT | SS_ENDELLIPSIS,
	                 26, 514, 606, 20, IDC_STATUS);
	g.status2 = child(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS,
	                  26, 534, 606, 20, IDC_STATUS2);

	child(L"BUTTON", L"Refresh devices", BS_PUSHBUTTON | WS_TABSTOP,
	      12, 562, 150, 30, IDC_REFRESH);
	g.startButton = child(L"BUTTON", L"Start", BS_DEFPUSHBUTTON | WS_TABSTOP,
	                      498, 562, 150, 30, IDC_START);

	buildAdvanced();
}


LRESULT CALLBACK mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
		case WM_CREATE:
			g.main = hwnd;
			buildUi();
			refreshDevices();
			// Opening on the shipping preset means the defaults on screen are
			// the defaults in the brief, and there is one place that decides
			// what those are.
			applySettingsToUi(presetSettings(PresetRealMonoDefault));
			showAdvanced(false);
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
			if (id == IDC_ADVANCED && code == BN_CLICKED) {
				showAdvanced(!g.advancedShown);
				return 0;
			}
			if (id == IDC_PRESET && code == CBN_SELCHANGE) {
				applySettingsToUi(presetSettings(comboSelection(g.presetCombo)));
				return 0;
			}
			// Every other checkbox and combo is a chain parameter, and they all
			// republish the same way.
			if (code == BN_CLICKED || code == CBN_SELCHANGE) {
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
				refreshValueLabels();
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
	meterClass.lpszClassName = L"RealMonoMeter";
	RegisterClassExW(&meterClass);

	WNDCLASSEXW mainClass = {};
	mainClass.cbSize = sizeof(mainClass);
	mainClass.lpfnWndProc = mainProc;
	mainClass.hInstance = instance;
	mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	mainClass.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
	mainClass.lpszClassName = L"RealMonoSoundMain";
	if (!RegisterClassExW(&mainClass)) {
		MessageBoxW(nullptr, L"Could not register the window class.", kAppName,
		            MB_ICONERROR | MB_OK);
		return 1;
	}

	// Fixed size: the layout is absolute, and a resizable window that does not
	// re-lay-out is worse than one that cannot be resized. The one height
	// change is the Advanced page, which the app makes itself.
	const DWORD style = (WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));
	RECT wanted = {0, 0, 660, kBaseHeight};
	AdjustWindowRect(&wanted, style, FALSE);

	HWND window = CreateWindowExW(0, mainClass.lpszClassName,
	                              L"Real Mono Sound \x2014 stereo to true mono",
	                              style, CW_USEDEFAULT, CW_USEDEFAULT,
	                              wanted.right - wanted.left, wanted.bottom - wanted.top,
	                              nullptr, nullptr, instance, nullptr);
	if (!window) {
		MessageBoxW(nullptr, L"Could not create the window.", kAppName, MB_ICONERROR | MB_OK);
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
