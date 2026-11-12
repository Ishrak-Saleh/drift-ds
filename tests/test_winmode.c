//test_winmode.c: checks for runtime/winmode.c. No framework, prints one line per failure.
//build and run from the project folder:
//  clang -Iruntime tests/test_winmode.c runtime/winmode.c -o tests/test_winmode.exe &&
//  tests/test_winmode.exe
#include "winmode.h"
#include <stdio.h>
#include <string.h>

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

int main(void)
{
  //the numbers are fixed: 0 = windowed, 1 = borderless fullscreen
  CHECK(winmode_parse("0", 1) == WINMODE_WINDOWED);
  CHECK(winmode_parse("1", 0) == WINMODE_BORDERLESS);

  //names
  CHECK(winmode_parse("windowed", 1) == WINMODE_WINDOWED);
  CHECK(winmode_parse("borderless", 0) == WINMODE_BORDERLESS);
  CHECK(winmode_parse("borderless fullscreen", 0) == WINMODE_BORDERLESS);

  //fullscreen (2 or the name) is shelved: it gives borderless. If it is ever switched on, it is
  //real fullscreen
#if WINMODE_FULLSCREEN_ENABLED
  CHECK(winmode_parse("2", 0) == WINMODE_FULLSCREEN);
  CHECK(winmode_parse("fullscreen", 0) == WINMODE_FULLSCREEN);
#else
  CHECK(winmode_parse("2", 0) == WINMODE_BORDERLESS);
  CHECK(winmode_parse("fullscreen", 0) == WINMODE_BORDERLESS);
  CHECK(winmode_parse("  FullScreen ", 0) == WINMODE_BORDERLESS);
#endif

  //case and spaces do not matter, a line ending from the ini file is ignored
  CHECK(winmode_parse("  WINDOWED ", 1) == WINMODE_WINDOWED);
  CHECK(winmode_parse("BORDERLESS\r", 0) == WINMODE_BORDERLESS);

  //empty or unknown: the old FULLSCREEN setting decides (1 = borderless, as before)
  CHECK(winmode_parse(NULL, 1) == WINMODE_BORDERLESS);
  CHECK(winmode_parse("", 1) == WINMODE_BORDERLESS);
  CHECK(winmode_parse("", 0) == WINMODE_WINDOWED);
  CHECK(winmode_parse("nonsense", 1) == WINMODE_BORDERLESS);
  CHECK(winmode_parse("nonsense", 0) == WINMODE_WINDOWED);
  CHECK(winmode_parse("3", 0) == WINMODE_WINDOWED);

  //FULLSCREEN_SIZE: width x height (only the shelved fullscreen uses it)
  int w = 0, h = 0;
  CHECK(winmode_parse_size("1280x720", &w, &h) && w == 1280 && h == 720);
  CHECK(winmode_parse_size("  1024 X 768 \r", &w, &h) && w == 1024 && h == 768);
  CHECK(winmode_parse_size("640x480", &w, &h) && w == 640 && h == 480);
  w = h = -1;
  CHECK(!winmode_parse_size(NULL, &w, &h) && w == -1 && h == -1); //nothing is written on failure
  CHECK(!winmode_parse_size("", &w, &h));
  CHECK(!winmode_parse_size("1280", &w, &h));
  CHECK(!winmode_parse_size("x720", &w, &h));
  CHECK(!winmode_parse_size("1280x", &w, &h));
  CHECK(!winmode_parse_size("1280x720x2", &w, &h));
  CHECK(!winmode_parse_size("1280x720 high", &w, &h));
  CHECK(!winmode_parse_size("300x200", &w, &h)); //too small
  CHECK(!winmode_parse_size("20000x720", &w, &h)); //too big
  CHECK(!winmode_parse_size("99999999999x720", &w, &h)); //huge number
  CHECK(!winmode_parse_size("abc", &w, &h));
  CHECK(w == -1 && h == -1);

  //F11 is a plain toggle and never enters fullscreen
  CHECK(winmode_next(WINMODE_WINDOWED) == WINMODE_BORDERLESS);
  CHECK(winmode_next(WINMODE_BORDERLESS) == WINMODE_WINDOWED);
  CHECK(winmode_next(WINMODE_FULLSCREEN) == WINMODE_WINDOWED);

  //names used in the log and the toast
  CHECK(!strcmp(winmode_name(WINMODE_WINDOWED), "windowed"));
  CHECK(!strcmp(winmode_name(WINMODE_FULLSCREEN), "fullscreen"));
  CHECK(!strcmp(winmode_name(WINMODE_BORDERLESS), "borderless fullscreen"));

  printf(failures ? "%d checks failed\n" : "all checks passed\n", failures);
  return failures != 0;
}
