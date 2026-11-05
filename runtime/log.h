// Game log: what the runtime tells you in the console while it runs, and the same text in a file.
//
// Where it fits: every thread (game, 3D, window, sound) can send lines here. The hot paths only
// bump counters; formatting happens once per second or on a rare event. A separate thread writes
// each line to the console and to logs/log-DATE_TIME.log, so a slow console or disk never stalls a
// frame. Crash reports go into the same file, right after the lines that led up to them.
// Why: to understand a crash or a stutter afterwards: what the PC was doing, which part was slow,
// and what happened just before the program stopped.
//
// Cost and privacy: the log measures its own cost and prints it every second. It only describes
// this program and, once at start, the machine it runs on (see sysinfo.h). The game has no network
// code, so nothing is sent anywhere. The last 10 log files are kept; older ones are deleted.
//
// Setting: LOG in settings.ini (or DRIFT_LOG): 0 = crash reports only, 1 = normal (default),
// 2 = more detail.
#pragma once
#include <stdint.h>
#include <stdio.h>

// once at start, after the settings are read. Starts the writer thread and prints the banner.
void log_init(void);

// One line: "[ 12.3] tag     text". printf style. Safe from any thread, never blocks on the
// console.
void log_msg(const char* tag, const char* fmt, ...);

// Level of detail the user chose (0, 1 or 2). Use it to skip building text nobody will read.
int log_level(void);

// Cheap counters, called from the thread that owns the event (no formatting inside).
void log_note_game_frame(void); // game thread, once per VBlank: tracks the time between frames
void log_note_scene(int64_t qpc_ticks, int polys); // 3D thread: how long a scene took to paint
void log_note_present(int64_t qpc_ticks); // paint thread: how long showing a picture took
void log_note_save(int64_t qpc_ticks, int ok); // game thread: how long writing a save took

// Once per second from the game thread: prints the performance lines and the hint, if any.
// shown_pictures = different pictures the window showed in that second.
void log_second(int game_fps, int shown_pictures);

// The log file (opened on first use, even when LOG=0), or NULL if it could not be created.
FILE* log_file(void);

// Starts a crash report in the log file: waits a moment for queued lines to be written, then writes
// a separator with the time and the reason. Returns the file (or NULL) for the caller to write the
// details to; also write them to stderr.
FILE* log_crash_begin(const char* reason);

// Waits a moment (at most 150 ms) until the writer has printed everything. Call it before the
// program exits, so the last lines are not lost.
void log_flush(void);
