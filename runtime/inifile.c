// Settings file rules: see inifile.h.
#include "inifile.h"
#include <string.h>

void ini_clean_value(char* value)
{
  for (char* p = value; *p; p++)
  {
    if (*p == '#' && p > value && (p[-1] == ' ' || p[-1] == '\t'))
    {
      *p = 0;
      break;
    }
  }
  size_t n = strlen(value);
  while (n && (value[n - 1] == ' ' || value[n - 1] == '\t' || value[n - 1] == '\r' ||
               value[n - 1] == '\n'))
    value[--n] = 0;
}
