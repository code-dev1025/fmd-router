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
#include <commdlg.h>

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
	IDC_SRC_DEVICE = 1003,
	IDC_REFRESH = 1004,
	IDC_START = 1005,
	IDC_BANNER = 1006,
	IDC_STATUS = 1007,
	IDC_STATUS2 = 1008,

	IDC_SRC_LOOPBACK = 1040,
	IDC_SRC_FILE = 1041,
	IDC_CHOOSE_FILE = 1042,
	IDC_LOOP = 1043,
	IDC_FILE_PATH = 1044,
	IDC_SEEK = 1045,
	IDC_PAUSE = 1046,
	IDC_TIME = 1047,

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

/*  Every drop-down is a label with the value it stands for written next to it,
    never a position in a list. The wording is expected to be revised, and a
    combo whose meaning is its row number turns "reorder these more sensibly"
    into a silent change of what the audio thread does. Renaming, reordering
    and inserting are all safe here; the static_asserts below catch the one
    thing that is not, which is forgetting to list a mode at all. */
struct Option {
	const wchar_t* label;
	int value;
};

const Option kQualities[] = {
	{L"Linear phase, HQ (1023 taps)",   QualityHqLinear},
	{L"Linear phase, short (255 taps)", QualityShortLinear},
	{L"Allpass IIR (lowest latency)",   QualityAllpass},
};
static_assert(std::size(kQualities) == NumQualities,
              "every Stage 3 quality needs a row in the drop-down");

const Option kCommits[] = {
	{L"Mid + rotated Side  (product)", CommitMidPlusRotatedSide},
	{L"Sum 0.5(L+R)  (reference)",     CommitSum},
	{L"Mid only  (Side discarded)",    CommitMidOnly},
	{L"Side energy fold  (lab)",       CommitSideEnergyFold},
	{L"Polarity matrix  (lab)",        CommitPolarityMatrix},
};
static_assert(std::size(kCommits) == NumCommitModes,
              "every Stage 4 commit mode needs a row in the drop-down");

const Option kSlopes[] = {
	{L"6 dB/oct", 6},
	{L"12 dB/oct", 12},
	{L"24 dB/oct", 24},
};

/*  Both of these high-pass the Side path and nothing else -- on a mono output
    "make the bass mono" and "high-pass the Side" are the same operation, so
    there is no band split to choose. What differs is the filter alignment, so
    that is what the list is named for: Butterworth is maximally flat and -3 dB
    at fc, Linkwitz-Riley is -6 dB at fc and is the shape that would sum flat
    against its complement. At 6 dB/oct the two are the same filter. */
const Option kHpfModes[] = {
	{L"Butterworth", HpfSide},
	{L"Linkwitz-Riley", HpfCrossoverMono},
};

/*  Where the audio comes from. Three mutually exclusive answers, so three
    radio buttons rather than a checkbox that only ever covered two of them --
    and one row, because the row underneath already changes meaning with the
    choice (an endpoint list, or the file being played) and the banner already
    explains whichever one is showing. */
enum SourceMode {
	SourceDevice = 0,
	SourceLoopback,
	SourceFile,
};


struct App {
	HWND main = nullptr;
	HFONT font = nullptr;

	HWND captureCombo = nullptr;
	HWND renderCombo = nullptr;
	HWND srcDevice = nullptr;
	HWND srcLoopback = nullptr;
	HWND srcFile = nullptr;
	HWND chooseFileButton = nullptr;
	HWND loopCheck = nullptr;
	HWND filePathLabel = nullptr;
	HWND seekSlider = nullptr;
	HWND pauseButton = nullptr;
	HWND timeLabel = nullptr;
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

	// True between the first TB_THUMBTRACK and the TB_ENDTRACK that closes it.
	// While it is set the timer leaves the seek bar alone, or the thumb would
	// be dragged out from under the mouse thirty times a second.
	bool scrubbing = false;

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

// The banner is three lines of routing instructions normally, and two when the
// file transport needs the third. Both heights are declared here because
// refreshSourceControls resizes between them and the numbers have to agree.
/*  The routing box has sixty pixels below the playback combo, and the file
    transport wants twenty-four of them. That leaves one line of banner, not
    two: at this font two lines need forty and would have the seek bar sitting
    on their descenders. It is the right line to give up — the three-line
    banner exists for the virtual-cable instructions, and a file source has no
    cable to explain. */
const int kBannerHeight = 58;
const int kBannerHeightWithTransport = 22;

// Resolution of the seek bar. A thousand steps is a quarter of a second on a
// four-minute file, which is finer than anyone scrubs by hand.
const int kSeekTicks = 1000;


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

/** Fills a drop-down from an option table, in table order. */
template <size_t N>
void fillOptions(HWND combo, const Option (&table)[N]) {
	SendMessageW(combo, CB_RESETCONTENT, 0, 0);
	for (size_t i = 0; i < N; i++)
		SendMessageW(combo, CB_ADDSTRING, 0, LPARAM(table[i].label));
}

/** What is selected, as a value rather than a row number. A selection outside
    the table cannot happen from the UI, but it is bounded here anyway: the
    alternative failure is reading past the array into whatever follows it. */
template <size_t N>
int optionValue(HWND combo, const Option (&table)[N], int fallback) {
	const int i = comboSelection(combo, -1);
	if (i < 0 || size_t(i) >= N)
		return fallback;
	return table[i].value;
}

/** The row a value sits on, for putting a preset back on screen. */
template <size_t N>
int optionIndex(const Option (&table)[N], int value) {
	for (size_t i = 0; i < N; i++) {
		if (table[i].value == value)
			return int(i);
	}
	return 0;
}

std::wstring formatDb(float db) {
	wchar_t buf[32];
	swprintf_s(buf, L"%+.1f dB", db);
	return buf;
}

/** Seconds as m:ss, which is how long a piece of music is. */
std::wstring formatClock(double seconds) {
	if (seconds < 0.0)
		seconds = 0.0;
	const int total = int(seconds);
	wchar_t buf[32];
	swprintf_s(buf, L"%d:%02d", total / 60, total % 60);
	return buf;
}

/** Which of the three source radios is on. */
SourceMode sourceMode() {
	if (checked(g.srcFile))
		return SourceFile;
	if (checked(g.srcLoopback))
		return SourceLoopback;
	return SourceDevice;
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
	s.hpfSlope = optionValue(g.s1Slope, kSlopes, 12);
	s.hpfMode = optionValue(g.s1Mode, kHpfModes, HpfSide);

	s.msEnabled = checked(g.s2Enable);
	s.midGainDb = sliderValue(SL_MID);
	s.sideGainDb = sliderValue(SL_SIDE);

	s.rotateEnabled = checked(g.s3Enable);
	s.quality = optionValue(g.s3Quality, kQualities, QualityHqLinear);

	s.commitEnabled = checked(g.s4Enable);
	s.commitMode = optionValue(g.s4Mode, kCommits, CommitMidPlusRotatedSide);
	s.sideInjectDb = sliderValue(SL_INJECT);
	s.outputGainDb = sliderValue(SL_OUTPUT);

	s.limiterEnabled = checked(g.s5Enable);
	s.ceilingDb = sliderValue(SL_CEILING);
	s.lookaheadMs = sliderValue(SL_LOOKAHEAD);
	return s;
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
	SendMessageW(g.s1Slope, CB_SETCURSEL, optionIndex(kSlopes, s.hpfSlope), 0);
	SendMessageW(g.s1Mode, CB_SETCURSEL, optionIndex(kHpfModes, s.hpfMode), 0);

	setChecked(g.s2Enable, s.msEnabled);
	setSlider(SL_MID, s.midGainDb);
	setSlider(SL_SIDE, s.sideGainDb);

	setChecked(g.s3Enable, s.rotateEnabled);
	SendMessageW(g.s3Quality, CB_SETCURSEL, optionIndex(kQualities, s.quality), 0);

	setChecked(g.s4Enable, s.commitEnabled);
	SendMessageW(g.s4Mode, CB_SETCURSEL, optionIndex(kCommits, s.commitMode), 0);
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

/** Two rows change meaning with the source. The capture combo and the file
    path take turns on the first -- in file mode the endpoint list has nothing
    to say and the path has -- and the banner gives up its third line to the
    transport on the second. Nothing moves that a device user would notice, and
    the window is one height throughout. */
void refreshSourceControls() {
	const bool file = (sourceMode() == SourceFile);
	const bool running = g.engine.running();
	const bool haveFile = g.engine.fileInfo().loaded;

	ShowWindow(g.captureCombo, file ? SW_HIDE : SW_SHOW);
	ShowWindow(g.filePathLabel, file ? SW_SHOW : SW_HIDE);
	EnableWindow(g.chooseFileButton, file && !running);
	EnableWindow(g.loopCheck, file);

	const int bannerHeight = file ? kBannerHeightWithTransport : kBannerHeight;
	SetWindowPos(g.banner, nullptr, 0, 0, 606, bannerHeight,
	             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

	for (HWND control : {g.seekSlider, g.pauseButton, g.timeLabel})
		ShowWindow(control, file ? SW_SHOW : SW_HIDE);

	// Scrubbing a file that is not playing is how you choose where Play starts,
	// so the bar is live while stopped. Pause is not: there is nothing to hold.
	EnableWindow(g.seekSlider, file && haveFile);
	EnableWindow(g.pauseButton, file && running);
	if (!running)
		SetWindowTextW(g.pauseButton, L"Pause");
}

/** The clock, from whatever the seek bar is currently showing. */
void refreshTimeLabel(double positionSeconds) {
	const Engine::FileInfo info = g.engine.fileInfo();
	const std::wstring text = formatClock(positionSeconds) + L" / " + formatClock(info.seconds);
	SetWindowTextW(g.timeLabel, text.c_str());
}

/** Where the seek bar is pointing, in seconds. */
double seekSliderSeconds() {
	const Engine::FileInfo info = g.engine.fileInfo();
	const double norm = double(SendMessageW(g.seekSlider, TBM_GETPOS, 0, 0)) / double(kSeekTicks);
	return norm * info.seconds;
}

void setSeekSlider(double seconds) {
	const Engine::FileInfo info = g.engine.fileInfo();
	const double norm = (info.seconds > 0.0) ? (seconds / info.seconds) : 0.0;
	const int pos = int(std::max(0.0, std::min(1.0, norm)) * kSeekTicks + 0.5);
	SendMessageW(g.seekSlider, TBM_SETPOS, TRUE, pos);
}

void updateBanner() {
	const SourceMode mode = sourceMode();

	if (mode == SourceFile) {
		const Engine::FileInfo info = g.engine.fileInfo();
		// One line, and it has to fit on one: the transport has the rest of the
		// space and this static clips rather than wrapping.
		if (!info.loaded) {
			SetWindowTextW(g.banner,
			    L"Choose a WAV, MP3, FLAC or M4A \x2014 it plays through the chain, "
			    L"no cable needed.");
			return;
		}
		// No file name here: the row above already shows the whole path, and a
		// banner whose length depends on the file name is a banner that clips
		// its own instructions on a long one.
		const std::wstring text =
		    formatClock(info.seconds) + L", " + std::to_wstring(info.sampleRate)
		    + L" Hz \x2014 leave it looping and toggle \"Real Mono processing\" to A/B.";
		SetWindowTextW(g.banner, text.c_str());
		return;
	}

	bool haveCable = false;
	for (const DeviceInfo& d : g.captureDevices)
		haveCable = haveCable || d.isVirtualCable;

	if (mode == SourceLoopback) {
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
	const bool loopback = (sourceMode() == SourceLoopback);

	// Loopback taps a playback endpoint, so the source list becomes the render
	// list. Same combo, different meaning -- the banner says which.
	if (!enumerateDevices(loopback ? eRender : eCapture, g.captureDevices, err))
		MessageBoxW(g.main, err.c_str(), kAppName, MB_ICONWARNING | MB_OK);
	if (!enumerateDevices(eRender, g.renderDevices, err))
		MessageBoxW(g.main, err.c_str(), kAppName, MB_ICONWARNING | MB_OK);

	fillCombo(g.captureCombo, g.captureDevices, !loopback);
	fillCombo(g.renderCombo, g.renderDevices, false);
	refreshSourceControls();
	updateBanner();
}


// ------------------------------------------------------------- file playback

/** Picks a file and decodes it, or explains why it could not. The decode is
    synchronous and can take a second on a long MP3, so the cursor says so --
    the message loop is not running during it, which is also why the wait
    cursor set here is not undone by the next WM_SETCURSOR. */
void chooseFile() {
	// Long paths exist and MAX_PATH does not fit them; the dialog is happy to
	// write into whatever is provided.
	std::vector<wchar_t> path(4096, L'\0');

	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = g.main;
	// Filters are double-null terminated pairs, which is why these look like
	// they are missing their separators.
	ofn.lpstrFilter =
	    L"Audio files\0*.wav;*.mp3;*.ogg;*.oga;*.flac;*.m4a;*.aac;*.wma\0"
	    L"WAV\0*.wav\0"
	    L"MP3\0*.mp3\0"
	    L"Ogg Vorbis\0*.ogg;*.oga\0"
	    L"FLAC\0*.flac\0"
	    L"All files\0*.*\0";
	ofn.lpstrFile = path.data();
	ofn.nMaxFile = DWORD(path.size());
	ofn.lpstrTitle = L"Choose a file to play through Real Mono";
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

	if (!GetOpenFileNameW(&ofn))
		return;  // cancelled, which is not an error

	const HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
	std::wstring err;
	const bool ok = g.engine.loadFile(path.data(), err);
	SetCursor(previous);

	if (!ok) {
		SetWindowTextW(g.filePathLabel, L"");
		updateBanner();
		MessageBoxW(g.main, err.c_str(), L"Real Mono Sound \x2014 could not load that file",
		            MB_ICONWARNING | MB_OK);
		return;
	}

	const Engine::FileInfo info = g.engine.fileInfo();
	SetWindowTextW(g.filePathLabel, info.path.c_str());
	setSeekSlider(0.0);
	refreshTimeLabel(0.0);
	refreshSourceControls();
	updateBanner();

	// A file that loaded but has something worth saying about it -- one that
	// was too long to hold, so far. It is playable either way, so this is told
	// after the UI has already accepted it.
	if (!err.empty())
		MessageBoxW(g.main, err.c_str(), kAppName, MB_ICONINFORMATION | MB_OK);
}


// ------------------------------------------------------------ start / stop

void updateStartButton() {
	const bool running = g.engine.running();
	// "Play" rather than "Start" for a file: the button does the same thing,
	// but what it does to a file has a name everybody already knows.
	SetWindowTextW(g.startButton,
	               running ? L"Stop" : (sourceMode() == SourceFile ? L"Play" : L"Start"));
	EnableWindow(g.captureCombo, !running);
	EnableWindow(g.renderCombo, !running);
	EnableWindow(g.srcDevice, !running);
	EnableWindow(g.srcLoopback, !running);
	EnableWindow(g.srcFile, !running);
	refreshSourceControls();
}

void toggleEngine() {
	if (g.engine.running()) {
		g.engine.stop();
		// Stop rewinds; Pause is the control that holds a position. Without
		// this the pause state would also survive into the next Play and the
		// file would sit there silently looking broken.
		g.engine.setPaused(false);
		g.engine.seekTo(0.0);
		setSeekSlider(0.0);
		refreshTimeLabel(0.0);
		updateStartButton();
		SetWindowTextW(g.status, L"Stopped.");
		SetWindowTextW(g.status2, L"");
		return;
	}

	const SourceMode mode = sourceMode();
	const LRESULT renSel = SendMessageW(g.renderCombo, CB_GETCURSEL, 0, 0);
	if (renSel == CB_ERR) {
		MessageBoxW(g.main, L"Choose a playback device first.",
		            kAppName, MB_ICONINFORMATION | MB_OK);
		return;
	}

	EngineConfig cfg;
	cfg.renderId = g.renderDevices[size_t(renSel)].id;
	cfg.loopback = (mode == SourceLoopback);
	cfg.fromFile = (mode == SourceFile);
	cfg.loopFile = checked(g.loopCheck);

	if (cfg.fromFile) {
		if (!g.engine.fileInfo().loaded) {
			MessageBoxW(g.main, L"Choose a file to play first.",
			            kAppName, MB_ICONINFORMATION | MB_OK);
			return;
		}
	}
	else {
		const LRESULT capSel = SendMessageW(g.captureCombo, CB_GETCURSEL, 0, 0);
		if (capSel == CB_ERR) {
			MessageBoxW(g.main, L"Choose a source device first.",
			            kAppName, MB_ICONINFORMATION | MB_OK);
			return;
		}
		cfg.captureId = g.captureDevices[size_t(capSel)].id;
	}

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

	// The seek bar follows the file, except while it is being dragged -- then
	// the user owns it and the clock reads from the thumb instead.
	if (sourceMode() == SourceFile && !g.scrubbing) {
		setSeekSlider(s.filePositionSeconds);
		refreshTimeLabel(s.filePositionSeconds);
	}

	// A file that has played out with looping off stops the engine, rather than
	// leaving it running on silence with the Stop button still lit.
	if (s.fileEnded && g.engine.running()) {
		g.engine.stop();
		// Rewound, or the next Play would start on the last frame and end again
		// before anything came out of the speakers.
		g.engine.setPaused(false);
		g.engine.seekTo(0.0);
		setSeekSlider(0.0);
		refreshTimeLabel(0.0);
		updateStartButton();
		SetWindowTextW(g.status, L"The file finished.");
		SetWindowTextW(g.status2, L"");
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

	// The position has its own readout next to the seek bar, so the status line
	// stays what it is for either source: formats and device periods.
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

	// WS_GROUP starts the radio set; the Choose button carries WS_GROUP to end
	// it, or the render combo further down would be swept into the same group
	// and the arrow keys would walk off the end of the source row into it.
	g.srcDevice = child(L"BUTTON", L"Device",
	                    BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 152, 36, 72, 22,
	                    IDC_SRC_DEVICE);
	g.srcLoopback = child(L"BUTTON", L"Loopback",
	                      BS_AUTORADIOBUTTON, 232, 36, 88, 22, IDC_SRC_LOOPBACK);
	g.srcFile = child(L"BUTTON", L"File",
	                  BS_AUTORADIOBUTTON, 328, 36, 60, 22, IDC_SRC_FILE);
	g.chooseFileButton = child(L"BUTTON", L"Choose file\x2026",
	                           BS_PUSHBUTTON | WS_GROUP | WS_TABSTOP, 396, 32, 116, 26,
	                           IDC_CHOOSE_FILE);
	g.loopCheck = child(L"BUTTON", L"Loop", BS_AUTOCHECKBOX | WS_TABSTOP,
	                    524, 36, 64, 22, IDC_LOOP);

	// The endpoint list and the loaded file share this row; refreshSourceControls
	// shows whichever the source radios have made relevant. SS_PATHELLIPSIS
	// eats the middle of a long path rather than the file name at the end of it.
	g.captureCombo = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
	                       152, 62, 480, 320, IDC_CAPTURE_COMBO);
	g.filePathLabel = child(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS,
	                        152, 66, 480, 20, IDC_FILE_PATH);

	// Device is the shipping routing; looping is what a test file is for.
	setChecked(g.srcDevice, true);
	setChecked(g.loopCheck, true);

	child(L"STATIC", L"Play processed to:", SS_LEFT, 26, 94, 120, 20, -1);
	g.renderCombo = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
	                      152, 90, 480, 320, IDC_RENDER_COMBO);

	// Three lines: the banner carries the routing instructions, and truncating
	// those is exactly the wrong thing to save 18 pixels on. In file mode it
	// gives up its third line to the transport below -- see kBannerHeight.
	g.banner = child(L"STATIC", L"", SS_LEFT, 26, 118, 606, kBannerHeight, IDC_BANNER);

	// The transport lives on the line the banner lends it, so the window is
	// the same height with a file loaded as without one.
	g.seekSlider = child(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
	                     24, 148, 404, 26, IDC_SEEK);
	SendMessageW(g.seekSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, kSeekTicks));
	SendMessageW(g.seekSlider, TBM_SETPAGESIZE, 0, kSeekTicks / 20);
	g.pauseButton = child(L"BUTTON", L"Pause", BS_PUSHBUTTON | WS_TABSTOP,
	                      436, 148, 78, 26, IDC_PAUSE);
	g.timeLabel = child(L"STATIC", L"0:00 / 0:00", SS_LEFT, 524, 152, 108, 20, IDC_TIME);
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
	// Sized to the label: a checkbox is clickable across its whole width, and a
	// 500 px one behind four characters of text toggles when you click nowhere
	// near it.
	g.hqCheck = child(L"BUTTON", L"\x2212""3 dB",
	                  BS_AUTOCHECKBOX | WS_TABSTOP, 26, 284, 90, 22, IDC_HQ);

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
	         L"Stage 1 high-passes the Side path so the bass stays mono. Off by default.",
	         SS_LEFT, 26, 620, 606, 18, -1);

	// ------------------------------------------------------------- Stage 1
	// No "(off)" in the label: the checkbox already says whether it is on, and
	// a label that says otherwise the moment it is ticked is worse than none.
	g.s1Enable = advChild(L"BUTTON", L"Stage 1   Side high-pass",
	                      BS_AUTOCHECKBOX | WS_TABSTOP, 26, 644, 200, 22, IDC_S1_ENABLE);
	advChild(L"STATIC", L"Fc", SS_LEFT, 340, 646, 24, 20, -1);
	makeSlider(SL_HPF_FC, 366, 642, 160, true);
	g.sliderValues[SL_HPF_FC] = advChild(L"STATIC", L"", SS_LEFT, 534, 646, 90, 20,
	                                     IDC_SLIDER_VALUE_BASE + SL_HPF_FC);

	advChild(L"STATIC", L"Slope:", SS_LEFT, 46, 672, 46, 20, -1);
	g.s1Slope = advChild(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
	                     96, 668, 100, 200, IDC_S1_SLOPE);
	fillOptions(g.s1Slope, kSlopes);

	advChild(L"STATIC", L"Shape:", SS_LEFT, 212, 672, 50, 20, -1);
	g.s1Mode = advChild(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
	                    266, 668, 160, 200, IDC_S1_MODE);
	fillOptions(g.s1Mode, kHpfModes);

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
	fillOptions(g.s3Quality, kQualities);

	// ------------------------------------------------------------- Stage 4
	g.s4Enable = advChild(L"BUTTON", L"Stage 4   Mono sum",
	                      BS_AUTOCHECKBOX | WS_TABSTOP, 26, 784, 170, 22, IDC_S4_ENABLE);
	advChild(L"STATIC", L"Mode:", SS_LEFT, 240, 786, 46, 20, -1);
	g.s4Mode = advChild(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
	                    290, 782, 250, 200, IDC_S4_MODE);
	fillOptions(g.s4Mode, kCommits);

	advChild(L"STATIC", L"Side inject", SS_LEFT, 26, 814, 76, 20, -1);
	makeSlider(SL_INJECT, 106, 810, 130, true);
	g.sliderValues[SL_INJECT] = advChild(L"STATIC", L"", SS_LEFT, 242, 814, 80, 20,
	                                     IDC_SLIDER_VALUE_BASE + SL_INJECT);

	advChild(L"STATIC", L"Output", SS_LEFT, 340, 814, 56, 20, -1);
	makeSlider(SL_OUTPUT, 400, 810, 130, true);
	g.sliderValues[SL_OUTPUT] = advChild(L"STATIC", L"", SS_LEFT, 536, 814, 88, 20,
	                                     IDC_SLIDER_VALUE_BASE + SL_OUTPUT);

	// ------------------------------------------------------------- Stage 5
	g.s5Enable = advChild(L"BUTTON", L"Stage 5   Limiter",
	                      BS_AUTOCHECKBOX | WS_TABSTOP, 26, 842, 150, 22, IDC_S5_ENABLE);
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
			// Loopback lists render endpoints where Device lists capture ones,
			// so changing the source relists; File hides the list entirely.
			// refreshDevices does all three plus the banner.
			if ((id == IDC_SRC_DEVICE || id == IDC_SRC_LOOPBACK || id == IDC_SRC_FILE)
			    && code == BN_CLICKED) {
				refreshDevices();
				updateStartButton();
				return 0;
			}
			if (id == IDC_CHOOSE_FILE && code == BN_CLICKED) {
				chooseFile();
				return 0;
			}
			// Looping is live, so it goes straight to the file thread rather
			// than through the chain settings.
			if (id == IDC_LOOP && code == BN_CLICKED) {
				g.engine.setLoopFile(checked(g.loopCheck));
				return 0;
			}
			if (id == IDC_PAUSE && code == BN_CLICKED) {
				// The button is disabled while stopped, but "the control was
				// greyed" is not a state check: a BM_CLICK sent to it still
				// arrives here, and acting on one would leave "Paused." sitting
				// under a Play button with nothing to resume.
				if (!g.engine.running())
					return 0;
				const bool paused = !g.engine.paused();
				g.engine.setPaused(paused);
				SetWindowTextW(g.pauseButton, paused ? L"Resume" : L"Pause");
				SetWindowTextW(g.status, paused ? L"Paused." : L"Running.");
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
			// The seek bar is a trackbar but not a chain parameter, so it is
			// taken out of the line before the rest are republished.
			if (lp && HWND(lp) == g.seekSlider) {
				const int notify = LOWORD(wp);
				if (notify == TB_THUMBTRACK) {
					// Mid-drag: show where the thumb is, but do not jump the
					// audio there on every pixel of a scrub.
					g.scrubbing = true;
					refreshTimeLabel(seekSliderSeconds());
				}
				else {
					g.scrubbing = false;
					const double seconds = seekSliderSeconds();
					g.engine.seekTo(seconds);
					refreshTimeLabel(seconds);
				}
				return 0;
			}
			// Every other trackbar reports here; the control handle in lParam
			// says which one, and it is cheaper to just republish all of them
			// than to work out the index and republish one.
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
