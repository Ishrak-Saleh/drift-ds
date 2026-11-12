//Window mode rules: see winmode.h.
#include "winmode.h"
#include <string.h>

//text equals word, ignoring case and spaces at both ends
static int same_word(const char* text, const char* word)
{
  while (*text == ' ' || *text == '\t')
    text++;
  size_t n = strlen(text);
  while (n &&
         (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r' || text[n - 1] == '\n'))
    n--;
  if (n != strlen(word))
    return 0;
  for (size_t i = 0; i < n; i++)
  {
    char a = text[i], b = word[i];
    if (a >= 'A' && a <= 'Z')
      a = (char)(a - 'A' + 'a');
    if (a != b)
      return 0;
  }
  return 1;
}

int winmode_parse(const char* text, int old_fullscreen_flag)
{
  if (text)
  {
    if (same_word(text, "windowed") || same_word(text, "window") || same_word(text, "0"))
      return WINMODE_WINDOWED;
    if (same_word(text, "borderless") || same_word(text, "borderless fullscreen") ||
        same_word(text, "1"))
      return WINMODE_BORDERLESS;
    if (same_word(text, "fullscreen") || same_word(text, "2"))
      return WINMODE_FULLSCREEN_ENABLED ? WINMODE_FULLSCREEN : WINMODE_BORDERLESS;
  }
  return old_fullscreen_flag == 1 ? WINMODE_BORDERLESS : WINMODE_WINDOWED;
}

//reads digits at *p into *value and moves *p past them. 0 if there are none or the number is huge
static int read_number(const char** p, long* value)
{
  long v = 0;
  int digits = 0;
  while (**p >= '0' && **p <= '9')
  {
    v = v * 10 + (**p - '0');
    if (v > 100000)
      return 0;
    (*p)++;
    digits++;
  }
  *value = v;
  return digits > 0;
}

static void skip_spaces(const char** p)
{
  while (**p == ' ' || **p == '\t')
    (*p)++;
}

int winmode_parse_size(const char* text, int* width, int* height)
{
  if (!text)
    return 0;
  const char* p = text;
  long w = 0, h = 0;
  skip_spaces(&p);
  if (!read_number(&p, &w))
    return 0;
  skip_spaces(&p);
  if (*p != 'x' && *p != 'X')
    return 0;
  p++;
  skip_spaces(&p);
  if (!read_number(&p, &h))
    return 0;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    p++;
  if (*p)
    return 0; //something after the two numbers
  if (w < 320 || w > 16384 || h < 320 || h > 16384)
    return 0;
  *width = (int)w;
  *height = (int)h;
  return 1;
}

int winmode_next(int mode)
{
  return mode == WINMODE_WINDOWED ? WINMODE_BORDERLESS : WINMODE_WINDOWED;
}

const char* winmode_name(int mode)
{
  if (mode == WINMODE_FULLSCREEN)
    return "fullscreen";
  if (mode == WINMODE_BORDERLESS)
    return "borderless fullscreen";
  return "windowed";
}
