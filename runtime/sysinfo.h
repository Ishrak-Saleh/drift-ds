// What the game reads about the machine it runs on, for the log. This is the ONLY file that reads
// machine details (tests/check_privacy.sh enforces it).
//
// Why it exists: when a player reports stutter, the first questions are always the same: which CPU,
// how much memory, on battery or not, which monitor refresh rate, which graphics card. The log
// answers them once at start so nobody has to ask.
//
// What it does not do: it reads no user name, computer name, file names, network details or
// account details. Everything goes to the console and to the log file in logs/ on this PC, and
// nowhere else. The game has no network code at all.
#pragma once
#include <wchar.h>

// CPU model and logical core count, Windows version, memory, power state. Called once at start.
void sysinfo_log_machine(void);

// Size, refresh rate and number of monitors, for the monitor the window is on. hwnd is an HWND.
void sysinfo_log_display(void* hwnd);

// Chooses the display mode for fullscreen on the monitor the window is on. want_w and want_h are
// the size asked for in the settings; 0 means the size the desktop has now. The refresh rate is the
// desktop's if that size has it, else the highest one below it, else the highest there is. The
// colour depth is the desktop's. It also gives back the monitor's device name (to switch that
// monitor) and the desktop's current mode (for the log). Returns 1 on success, 0 if the size does
// not exist on this monitor or Windows would not say. Nothing is logged here.
int sysinfo_pick_mode(void* hwnd, int want_w, int want_h, int* width, int* height, int* hertz,
                      wchar_t device[32], int* desktop_w, int* desktop_h, int* desktop_hz);

// Which edge of a monitor has no other monitor next to it: 0 bottom, 1 right, 2 top, 3 left (the
// first one found in that order), or -1 if every edge has a neighbour. The monitor is given as
// its rectangle. An edge that touches the border of the whole desktop has no neighbour. The
// borderless window uses it to put its one extra pixel where no other monitor can show it.
int sysinfo_free_edge(int left, int top, int right, int bottom);

// How busy the whole PC was since the previous call, 0 to 100. Returns -1 on the first call.
int sysinfo_pc_cpu_percent(void);
