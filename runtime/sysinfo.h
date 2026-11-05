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

// CPU model and logical core count, Windows version, memory, power state. Called once at start.
void sysinfo_log_machine(void);

// Size, refresh rate and number of monitors, for the monitor the window is on. hwnd is an HWND.
void sysinfo_log_display(void* hwnd);

// How busy the whole PC was since the previous call, 0 to 100. Returns -1 on the first call.
int sysinfo_pc_cpu_percent(void);
