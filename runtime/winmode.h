// The ways the game window can be shown, and the small rules around them.
//
// Where it fits: window.c owns the real window and does the Windows calls. This file only holds
// the parts that need no Windows at all (reading the setting, the F11 rule, the names), so tests
// can run them on their own.
// Why: the log and the on-screen toast must use the same names, and the settings numbers must stay
// the same from one version to the next.
#pragma once

// Real fullscreen (the display mode of the monitor is switched) is SHELVED: 0 = players cannot
// reach it. The code for it stays in window.c, so bringing it back means setting this to 1.
// Why shelved: in that mode the picture stalls for a moment whenever the Windows volume overlay
// appears (a real stall of the display output, caused by Windows and the driver). Borderless
// fullscreen with its one extra pixel does not stall.
#define WINMODE_FULLSCREEN_ENABLED 0

enum
{
  WINMODE_WINDOWED = 0, // a normal window with a title bar
  WINMODE_FULLSCREEN = 1, // the whole monitor, the display mode is taken over (CDS_FULLSCREEN)
  WINMODE_BORDERLESS = 2, // a window without a border stretched over the whole monitor
  WINMODE_COUNT
};

// Reads the WINDOW_MODE setting. The numbers are fixed: 0 = windowed, 1 = borderless fullscreen,
// 2 = fullscreen. The names are "windowed", "borderless" (also "borderless fullscreen") and
// "fullscreen". Case does not matter. Fullscreen (2 or the name) gives borderless while it is
// shelved, so a player who asks for it still gets a full-screen game. If the text is empty or not
// recognised, the old FULLSCREEN setting decides: 1 gives borderless, anything else windowed.
// The result is a WINMODE_ value (not the setting number).
int winmode_parse(const char* text, int old_fullscreen_flag);

// Reads the FULLSCREEN_SIZE setting: "1280x720" (an X in either case, spaces around the numbers and
// at the ends are fine). Both numbers must be between 320 and 16384. Returns 1 and fills width and
// height, or returns 0 for empty text, missing parts, extra text or numbers out of range. The
// caller then uses the size the desktop has now. Only the shelved fullscreen mode uses it.
int winmode_parse_size(const char* text, int* width, int* height);

// What F11 switches to: a plain toggle. Windowed goes to borderless, borderless goes to windowed.
// F11 never enters fullscreen. If the game is in fullscreen (only possible when it is not
// shelved), F11 goes to windowed.
int winmode_next(int mode);

// The name used in the log and in the toast: "windowed", "fullscreen", "borderless fullscreen".
const char* winmode_name(int mode);
