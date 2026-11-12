//test_inifile.c: checks for runtime/inifile.c. No framework, prints one line per failure.
//build and run from the project folder:
//  clang -Iruntime tests/test_inifile.c runtime/inifile.c -o tests/test_inifile.exe &&
//  tests/test_inifile.exe
#include "inifile.h"
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

//cleans a copy of the text and says whether it matches the expected result
static int cleans_to(const char* text, const char* expected)
{
  char buffer[128];
  snprintf(buffer, sizeof buffer, "%s", text);
  ini_clean_value(buffer);
  if (strcmp(buffer, expected))
    printf("  \"%s\" became \"%s\", expected \"%s\"\n", text, buffer, expected);
  return !strcmp(buffer, expected);
}

int main(void)
{
  //a note after a value is cut off, with the blanks before it
  CHECK(cleans_to("1280x720 # fullscreen only", "1280x720"));
  CHECK(cleans_to("W,Enter  #two keys", "W,Enter"));
  CHECK(cleans_to("Dipjol\t# up to 10 letters", "Dipjol"));
  CHECK(cleans_to("1 # first # second", "1"));

  //an empty value with a note stays empty
  CHECK(cleans_to(" # use the default", ""));
  CHECK(cleans_to("", ""));

  //a '#' with no blank before it belongs to the value
  CHECK(cleans_to("#hash", "#hash"));
  CHECK(cleans_to("Mic#2", "Mic#2"));
  CHECK(cleans_to("Mic#2 # my note", "Mic#2"));

  //values without notes are not changed, except blanks and line ends at the end
  CHECK(cleans_to("Dipjol", "Dipjol"));
  CHECK(cleans_to("Dipjol   ", "Dipjol"));
  CHECK(cleans_to("Dipjol\r", "Dipjol"));
  CHECK(cleans_to("  two words", "  two words"));
  CHECK(cleans_to("W,Enter", "W,Enter"));

  printf(failures ? "%d checks failed\n" : "all checks passed\n", failures);
  return failures != 0;
}
