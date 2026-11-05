// test_log.c: runs runtime/log.c on its own with fake 3D counters and checks what a player would
// see and what ends up in the log file. No framework. Build and run from a scratch folder, because
// it creates a logs/ folder in the current directory:
//   clang -Iruntime tests/test_log.c runtime/log.c runtime/sysinfo.c -o test_log.exe &&
//   test_log.exe
#include "log.h"
#include "compat.h"
#include <stdio.h>
#include <string.h>

// stand-ins for the renderer and the video module that log.c asks for numbers
static uint64_t fake_scenes, fake_skipped;
uint64_t gx_frames(void)
{
  return fake_scenes;
}
uint64_t gx_skipped(void)
{
  return fake_skipped;
}
int video_scale(void)
{
  return 2;
}

static int failures;
#define CHECK(cond)                                                                                \
  do                                                                                               \
  {                                                                                                \
    if (!(cond))                                                                                   \
    {                                                                                              \
      printf("FAIL line %d: %s\n", __LINE__, #cond);                                               \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

static int64_t ms_to_ticks(double ms)
{
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return (int64_t)(ms * (double)f.QuadPart / 1000.0);
}

// does the text appear anywhere in the newest log file?
static int log_contains(const char* text)
{
  WIN32_FIND_DATAA found;
  HANDLE search = FindFirstFileA("logs\\log-*.log", &found);
  if (search == INVALID_HANDLE_VALUE)
    return 0;
  char newest[MAX_PATH] = "";
  do
  {
    if (strcmp(found.cFileName, newest) > 0)
      snprintf(newest, sizeof newest, "%s", found.cFileName);
  } while (FindNextFileA(search, &found));
  FindClose(search);
  char path[MAX_PATH + 8];
  snprintf(path, sizeof path, "logs\\%s", newest);
  FILE* f = fopen(path, "r");
  if (!f)
    return 0;
  char line[512];
  int seen = 0;
  while (fgets(line, sizeof line, f))
    if (strstr(line, text))
      seen = 1;
  fclose(f);
  return seen;
}

int main(void)
{
  _putenv("DRIFT_LOG=2");
  log_init();
  CHECK(log_level() == 2);
  log_msg("rom", "AMCE (USA), %d KB", 16384);

  // one healthy second: 60 frames, 60 scenes of about 3 ms
  for (int i = 0; i < 60; i++)
  {
    log_note_game_frame();
    log_note_scene(ms_to_ticks(3.0), 900);
    log_note_present(ms_to_ticks(8.0));
    fake_scenes++;
    Sleep(16);
  }
  log_second(60, 60);

  // a bad second: one slow frame, one slow scene, one slow present, a slow save, few pictures shown
  log_note_game_frame();
  Sleep(40);
  log_note_game_frame();
  log_note_scene(ms_to_ticks(35.0), 2100);
  log_note_present(ms_to_ticks(55.0));
  log_note_save(ms_to_ticks(30.0), 1);
  log_note_save(ms_to_ticks(2.0), 0);
  fake_skipped += 3;
  log_second(60, 30);

  // a crash report goes into the same file, after the lines before it
  FILE* lf = log_crash_begin("test crash");
  CHECK(lf != NULL);
  if (lf)
  {
    fprintf(lf, "  r0 DEADBEEF\n");
    fflush(lf);
  }
  Sleep(300); // let the writer thread finish
  log_flush();

  CHECK(log_contains("DriftDS, built"));
  CHECK(log_contains("This game has no network code"));
  CHECK(log_contains("logical cores")); // machine details from sysinfo.c
  CHECK(log_contains("load    cpu"));
  CHECK(log_contains("log cost"));
  CHECK(log_contains("showing pictures is slow"));
  CHECK(log_contains("3D scene took 35.0 ms"));
  CHECK(log_contains("==== CRASH"));
  CHECK(log_contains("test crash"));
  CHECK(log_contains("DEADBEEF"));
  printf(failures ? "%d checks failed\n" : "all checks passed\n", failures);
  return failures != 0;
}
