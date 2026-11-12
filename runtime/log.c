//Game log: see log.h.
//
//Layout: every line goes into a ring buffer under a short lock. A writer thread copies new lines
//to the console and to the log file, so the thread that logs never waits for either. Counters for
//performance are plain interlocked integers; they are read and reset once per second by
//log_second. The time spent inside the log code itself is counted and printed too.
#include "compat.h"
#include "log.h"
#include "gpu3d.h"
#include "sysinfo.h"
#include "video.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define RING_LINES 128
#define LINE_LEN 220
#define SLOW_FRAME_US 25000 //a frame longer than this is reported (a frame is 16.7 ms)
#define SLOW_SCENE_US 20000
#define SLOW_PRESENT_US 40000
#define SLOW_SAVE_US 15000
#define SLOW_LINES_PER_SECOND 6
#define QUIET_START_SECONDS 5.0 //no slow-picture reports while the program starts
#define KEEP_LOG_FILES 10

static char ring[RING_LINES][LINE_LEN];
static uint64_t ring_count; //lines ever written
static uint64_t shown_count; //lines the writer thread has printed
static CRITICAL_SECTION ring_lock;
static HANDLE ring_event;
static int level = 1;
static int ready;
static int64_t qpc_freq, qpc_start;
static FILE* log_out;
static int log_open_tried;

//performance counters, one writer thread each, read and cleared by log_second
static volatile LONG frame_gap_max_us, scene_max_us, present_max_us;
static volatile LONG scene_count, scene_sum_us, scene_polys;
static volatile LONG slow_budget = SLOW_LINES_PER_SECOND;
static volatile LONG64 quiet_until_ticks; //slow-picture lines and hints stay silent until then
static char mode_name[32]; //how the window is shown, for the perf line. Guarded by ring_lock
static int64_t last_frame_ticks; //game thread only
static volatile LONG64 cost_ticks; //time the game's own threads spend inside the log code
static volatile LONG64
    writer_ticks; //time the writer thread spends writing (mostly waiting on the console)

static int64_t now_ticks(void)
{
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

static double uptime_seconds(void)
{
  return ready ? (double)(now_ticks() - qpc_start) / (double)qpc_freq : 0.0;
}

static LONG ticks_to_us(int64_t ticks)
{
  return (LONG)(ticks * 1000000 / qpc_freq);
}

static void add_cost(int64_t started)
{
  InterlockedExchangeAdd64(&cost_ticks, now_ticks() - started);
}

//keeps the largest value seen; several threads may race, a lost update only misses one sample
static void raise_max(volatile LONG* slot, LONG value)
{
  LONG seen;
  while (value > (seen = *slot))
    if (InterlockedCompareExchange(slot, value, seen) == seen)
      break;
}

//Slow-event lines are limited per second so a long stall cannot flood the console.
static int take_slow_slot(void)
{
  return level > 0 && InterlockedDecrement(&slow_budget) >= 0;
}

//true during start-up and right after a fullscreen switch: showing a picture is slow then, and
//that is normal, so it must not be reported as a problem.
static int in_quiet(void)
{
  return now_ticks() < quiet_until_ticks;
}

void log_quiet(double seconds)
{
  if (!ready)
    return;
  InterlockedExchange64(&quiet_until_ticks, now_ticks() + (int64_t)(seconds * (double)qpc_freq));
}

void log_set_mode(const char* name)
{
  if (!ready)
    return;
  EnterCriticalSection(&ring_lock);
  snprintf(mode_name, sizeof mode_name, "%s", name ? name : "");
  LeaveCriticalSection(&ring_lock);
}

int log_level(void)
{
  return level;
}

//---------------------------------------------------------------------------------------------
//The log file
//---------------------------------------------------------------------------------------------

static int compare_names(const void* a, const void* b)
{
  return strcmp((const char*)a, (const char*)b);
}

//names contain the date and time, so sorting them sorts by age. The oldest go.
static void prune_old_logs(void)
{
  char names[64][48];
  int count = 0;
  WIN32_FIND_DATAA found;
  HANDLE search = FindFirstFileA("logs\\log-*.log", &found);
  if (search == INVALID_HANDLE_VALUE)
    return;
  do
  {
    if (count < 64)
      snprintf(names[count++], sizeof names[0], "%s", found.cFileName);
  } while (FindNextFileA(search, &found));
  FindClose(search);
  qsort(names, (size_t)count, sizeof names[0], compare_names);
  for (int i = 0; i < count - (KEEP_LOG_FILES - 1); i++)
  {
    char path[96];
    snprintf(path, sizeof path, "logs\\%s", names[i]);
    remove(path);
  }
}

FILE* log_file(void)
{
  if (log_open_tried)
    return log_out;
  log_open_tried = 1;
  CreateDirectoryA("logs", NULL);
  prune_old_logs();
  SYSTEMTIME t;
  GetLocalTime(&t);
  char path[96];
  snprintf(path, sizeof path, "logs\\log-%04d-%02d-%02d_%02d-%02d-%02d.log", t.wYear, t.wMonth,
           t.wDay, t.wHour, t.wMinute, t.wSecond);
  log_out = fopen(path, "w");
  if (log_out)
  {
    fprintf(log_out, "DriftDS log, started %04d-%02d-%02d %02d:%02d:%02d\n", t.wYear, t.wMonth,
            t.wDay, t.wHour, t.wMinute, t.wSecond);
    fflush(log_out);
  }
  return log_out;
}

FILE* log_crash_begin(const char* reason)
{
  log_flush(); //lines queued before the crash reach the file first
  FILE* f = log_file();
  if (f)
  {
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "\n==== CRASH %04d-%02d-%02d %02d:%02d:%02d, uptime %.1f s: %s ====\n", t.wYear,
            t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, uptime_seconds(), reason);
  }
  return f;
}

//---------------------------------------------------------------------------------------------
//Lines
//---------------------------------------------------------------------------------------------

void log_msg(const char* tag, const char* fmt, ...)
{
  if (!level || !ready)
    return;
  int64_t started = now_ticks();
  char text[LINE_LEN - 24];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof text, fmt, args);
  va_end(args);
  double stamp = uptime_seconds();
  EnterCriticalSection(&ring_lock);
  snprintf(ring[ring_count % RING_LINES], LINE_LEN, "[%6.1f] %-7s %s", stamp, tag, text);
  ring_count++;
  LeaveCriticalSection(&ring_lock);
  SetEvent(ring_event);
  add_cost(started);
}

static DWORD WINAPI writer_thread(LPVOID unused)
{
  (void)unused;
  for (;;)
  {
    WaitForSingleObject(ring_event, INFINITE);
    for (;;)
    {
      char line[LINE_LEN];
      int have = 0;
      uint64_t lost = 0;
      EnterCriticalSection(&ring_lock);
      if (ring_count - shown_count > RING_LINES)
      {
        lost = ring_count - shown_count - RING_LINES;
        shown_count = ring_count - RING_LINES;
      }
      if (shown_count < ring_count)
      {
        memcpy(line, ring[shown_count % RING_LINES], LINE_LEN);
        have = 1;
      }
      LeaveCriticalSection(&ring_lock);
      if (lost)
        fprintf(stderr, "[log] %llu lines were dropped (the console is too slow)\n",
                (unsigned long long)lost);
      if (!have)
        break;
      int64_t started = now_ticks();
      fprintf(stderr, "%s\n", line);
      if (log_out)
      {
        fprintf(log_out, "%s\n", line);
        fflush(log_out); //a crash must not lose the lines before it
      }
      InterlockedExchangeAdd64(&writer_ticks, now_ticks() - started);
      EnterCriticalSection(&ring_lock);
      shown_count++; //counted as shown only after it is on screen and in the file
      LeaveCriticalSection(&ring_lock);
    }
  }
}

static void banner(void)
{
  int64_t started = now_ticks();
  log_msg("build", "DriftDS, built %s %s", __DATE__, __TIME__);
  sysinfo_log_machine();
  log_msg("log",
          "level %d, file in logs/. This game has no network code: nothing is sent anywhere.",
          level);
  log_msg("log", "reading the machine details took %.1f ms, once",
          (double)(now_ticks() - started) * 1000.0 / (double)qpc_freq);
}

void log_init(void)
{
  const char* setting = getenv("DRIFT_LOG");
  if (setting && *setting)
    level = atoi(setting) < 0 ? 0 : atoi(setting) > 2 ? 2 : atoi(setting);
  if (!level)
    return; //crash reports still work: log_file() opens the file when they need it
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  qpc_freq = f.QuadPart;
  qpc_start = now_ticks();
  InitializeCriticalSection(&ring_lock);
  ring_event = CreateEventW(NULL, FALSE, FALSE, NULL);
  log_file();
  ready = 1;
  log_quiet(QUIET_START_SECONDS);

  //clicking in a console window starts "quick edit" selection, which freezes every program that
  //writes to it. Turn it off so a stray click cannot stop the game.
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode;
  if (in && in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode))
    SetConsoleMode(in, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);

  HANDLE writer = CreateThread(NULL, 0, writer_thread, NULL, 0, NULL);
  if (writer)
  {
    SetThreadPriority(writer, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(writer);
  }
  banner();
}

void log_flush(void)
{
  if (!ready)
    return;
  for (int waited = 0; waited < 150; waited += 10)
  {
    if (shown_count >= ring_count)
      return;
    Sleep(10);
  }
}

//---------------------------------------------------------------------------------------------
//Counters
//---------------------------------------------------------------------------------------------

void log_note_game_frame(void)
{
  if (!ready)
    return;
  int64_t now = now_ticks();
  if (last_frame_ticks)
  {
    LONG us = ticks_to_us(now - last_frame_ticks);
    raise_max(&frame_gap_max_us, us);
    if (us > SLOW_FRAME_US && take_slow_slot())
      log_msg("slow", "game frame took %.1f ms (a frame is 16.7 ms)", us / 1000.0);
  }
  last_frame_ticks = now;
  add_cost(now);
}

void log_note_scene(int64_t ticks, int polys)
{
  if (!ready)
    return;
  int64_t started = now_ticks();
  LONG us = ticks_to_us(ticks);
  InterlockedIncrement(&scene_count);
  InterlockedExchangeAdd(&scene_sum_us, us);
  InterlockedExchange(&scene_polys, polys);
  raise_max(&scene_max_us, us);
  if (us > SLOW_SCENE_US && take_slow_slot())
    log_msg("slow", "3D scene took %.1f ms (%d polygons)", us / 1000.0, polys);
  add_cost(started);
}

void log_note_present(int64_t ticks)
{
  if (!ready)
    return;
  int64_t started = now_ticks();
  LONG us = ticks_to_us(ticks);
  raise_max(&present_max_us, us);
  if (us > SLOW_PRESENT_US && !in_quiet() && take_slow_slot())
    log_msg("slow", "showing a picture took %.1f ms (display or graphics driver)", us / 1000.0);
  add_cost(started);
}

void log_note_save(int64_t ticks, int ok)
{
  if (!ready)
    return;
  LONG us = ticks_to_us(ticks);
  if (!ok)
    log_msg("save", "FAILED to write the save file");
  else if (us > SLOW_SAVE_US && take_slow_slot())
    log_msg("slow", "writing the save took %.1f ms (disk or antivirus)", us / 1000.0);
  else if (level >= 2)
    log_msg("save", "wrote the save in %.1f ms", us / 1000.0);
}

//---------------------------------------------------------------------------------------------
//Once per second
//---------------------------------------------------------------------------------------------

static uint64_t filetime_value(FILETIME ft)
{
  return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

void log_second(int game_fps, int shown_pictures)
{
  if (!ready)
    return;
  int64_t started = now_ticks();
  static uint64_t scenes_before, skipped_before;
  static uint64_t proc_before;
  static int64_t wall_before;

  uint64_t scenes_now = gx_frames(), skipped_now = gx_skipped();
  int scenes = (int)(scenes_now - scenes_before);
  int skipped = (int)(skipped_now - skipped_before);
  scenes_before = scenes_now;
  skipped_before = skipped_now;

  LONG n = InterlockedExchange(&scene_count, 0);
  LONG sum = InterlockedExchange(&scene_sum_us, 0);
  LONG scene_max = InterlockedExchange(&scene_max_us, 0);
  LONG frame_max = InterlockedExchange(&frame_gap_max_us, 0);
  LONG present_max = InterlockedExchange(&present_max_us, 0);
  InterlockedExchange(&slow_budget, SLOW_LINES_PER_SECOND);

  //CPU time used by this program, as "cores busy": 2.5 means the equivalent of two and a half
  //cores were running. And how busy the whole PC was, which shows other programs taking the CPU.
  FILETIME created, exited, kernel, user;
  double game_cores = -1.0; //-1 until there is an earlier sample to compare with
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
  {
    uint64_t proc = filetime_value(kernel) + filetime_value(user); //units of 100 ns
    if (wall_before)
      game_cores =
          (double)(proc - proc_before) / 1e7 / ((double)(started - wall_before) / (double)qpc_freq);
    proc_before = proc;
    wall_before = started;
  }
  int pc_cpu = sysinfo_pc_cpu_percent();

  //a short reason when the numbers are off: the thing to look at first
  const char* hint = "";
  if (in_quiet())
    hint = ""; //start-up and window changes are not gameplay
  else if (game_fps < 55)
    hint = "game is running slow";
  else if (shown_pictures < game_fps * 8 / 10)
  {
    if (present_max > SLOW_PRESENT_US)
      hint = "showing pictures is slow (display or graphics driver)";
    else if (pc_cpu >= 90)
      hint = "the PC is busy, other programs may be taking the CPU";
    else if (scene_max > SLOW_SCENE_US)
      hint = "3D scenes are slow";
    else
      hint = "fewer pictures shown than game frames";
  }

  static int legend_printed;
  if (!legend_printed)
  {
    legend_printed = 1;
    log_msg("perf", "columns: scene = average/longest ms to draw a 3D scene, frame = longest game");
    log_msg("perf",
            "frame (ms), show = longest time to show a picture (ms). load line: cpu = cores");
    log_msg("perf", "this game uses, pc = how busy the whole PC is, log cost = time spent logging "
                    "in that second,");
    log_msg("perf", "mode = how the window is shown (windowed, fullscreen, borderless fullscreen)");
  }
  char cpu_text[24] = "n/a", pc_text[16] = "n/a";
  if (game_cores >= 0.0)
    snprintf(cpu_text, sizeof cpu_text, "%.1f cores", game_cores);
  if (pc_cpu >= 0)
    snprintf(pc_text, sizeof pc_text, "%d%%", pc_cpu);
  double log_ms = (double)InterlockedExchange64(&cost_ticks, 0) * 1000.0 / (double)qpc_freq;
  double writer_ms = (double)InterlockedExchange64(&writer_ticks, 0) * 1000.0 / (double)qpc_freq;
  char mode_text[48] = "";
  EnterCriticalSection(&ring_lock);
  if (*mode_name)
    snprintf(mode_text, sizeof mode_text, " | mode %s", mode_name);
  LeaveCriticalSection(&ring_lock);
  add_cost(started); //the work above; the lines below count themselves
  log_msg("perf",
          "fps %2d | 3D %2d skip %d | screen %2d | scene %.1f/%.1f | frame %.1f | show %.1f%s",
          game_fps, scenes, skipped, shown_pictures, n ? sum / 1000.0 / n : 0.0, scene_max / 1000.0,
          frame_max / 1000.0, present_max / 1000.0, mode_text);
  log_msg("load", "cpu %s | pc %s | log cost: game threads %.2f ms/s, writer thread %.2f ms/s",
          cpu_text, pc_text, log_ms, writer_ms);
  if (*hint)
    log_msg("hint", "%s", hint);
  if (level >= 2)
    log_msg("detail", "%d polygons in the last scene, 3D scale %dx", (int)scene_polys,
            video_scale());
}
