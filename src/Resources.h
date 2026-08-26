#pragma once

/*  Ids for the client's artwork, which is compiled into the executable rather
    than loaded from a folder beside it.

    The alternative -- shipping a `resources` directory next to the exe -- makes
    the app's appearance depend on a path, and an app that renders as grey
    rectangles because someone moved a folder is a support call. Everything the
    face draws is in the binary, so there is one file to hand over. */

#define IDR_BACKGROUND_IDLE     101
#define IDR_BACKGROUND_STEREO   102
#define IDR_BACKGROUND_MONO     103

#define IDR_FONT_BANNER         201
#define IDR_FONT_SCRIPT         202
#define IDR_FONT_BANNER_ALT     203
