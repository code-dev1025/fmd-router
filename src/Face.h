#pragma once

/*  The client's face -- the window their screens are of.

    It owns no state. Everything it draws it reads back through Host::read, and
    everything it changes it changes by calling the app, which changes the
    controls on the Routing & Advanced window, which is where the settings have
    always lived. That is deliberate: two windows that each keep their own copy
    of "is the limiter on" is two windows that will one day disagree, and the
    one the client is looking at will be the wrong one.

    So the face is a view. The panel is the model. */

#include <windows.h>

namespace fmdr {
namespace face {

/** What the face draws, in the terms the face draws it in. */
struct Model {
	/** false selects the Stereo button -- the global bypass, which is a
	    genuine untouched pass-through and the A/B this product lives on. */
	bool mono = true;
	/** The Side is lifted inside the mix, so the content a plain downmix
	    destroys is the content you notice. */
	bool solo = false;
	bool highestQuality = false;
	/** Makeup gain into the limiter. */
	bool loudness = false;
	/** Lights the background: idle plate when nothing is playing, the stereo
	    or mono plate when it is. */
	bool running = false;
};

/** Everything the face is allowed to do to the app. Free functions rather than
    an interface, because there is exactly one face and exactly one app, and a
    vtable to say so would be ceremony. */
struct Host {
	Model (*read)() = nullptr;
	void (*setMono)(bool) = nullptr;
	void (*setSolo)(bool) = nullptr;
	void (*setHighestQuality)(bool) = nullptr;
	void (*setLoudness)(bool) = nullptr;
	/** Bring up the Routing & Advanced window, which is where the devices, the
	    file player, the meters and the per-stage bypasses are. */
	void (*openPanel)() = nullptr;
	/** The face is closing, and closing it closes the app. Called before the
	    window is destroyed, so the audio threads can be stopped while
	    everything they publish into is still standing. */
	void (*close)() = nullptr;
};

/** Registers the window class. Once, before create(). */
bool registerClass(HINSTANCE instance);

/** Creates the face. The window is sized to whatever fraction of the client's
    drawing fits the work area, so it is whole on a laptop as well as a
    desktop. Returns nullptr if the class or the window could not be made. */
HWND create(HINSTANCE instance, const Host& host);

/** Repaint, because something the model reports has changed. Safe before
    create() and after the window has gone. */
void refresh();

/** Releases the artwork. Call before GdiplusShutdown, or the bitmaps are
    freed against a GDI+ that has already gone. */
void shutdown();

} // namespace face
} // namespace fmdr
