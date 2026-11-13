//Settings file rules: see inifile.h.
#include "inifile.h"
#include <ctype.h>
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

int ini_split(char* line, char** key, char** value)
{
  char* k = line;
  while (*k == ' ' || *k == '\t')
    k++;
  if (!*k || *k == '#' || *k == ';' || *k == '[')
    return 0;
  char* same = strchr(k, '=');
  if (!same || same == k)
    return 0;
  *same = 0;
  for (char* end = same; end > k && (end[-1] == ' ' || end[-1] == '\t');)
    *--end = 0;
  for (char* p = k; *p; p++)
    *p = (char)toupper((unsigned char)*p);
  char* v = same + 1;
  ini_clean_value(v);
  while (*v == ' ' || *v == '\t')
    v++;
  *key = k;
  *value = v;
  return 1;
}

//every key of settings.ini. Keys of the launcher are here too: the game ignores them
static const char* const KNOWN_KEYS[] = {"SCALE",
                                         "WINDOW_MODE",
                                         "FULLSCREEN_SIZE",
                                         "NAME",
                                         "LANGUAGE",
                                         "MIC",
                                         "MIC_DEVICE",
                                         "MIC_GAIN",
                                         "MIC_KEY",
                                         "VOLUME",
                                         "MUTE",
                                         "PRESET",
                                         "KEY_A",
                                         "KEY_B",
                                         "KEY_SELECT",
                                         "KEY_START",
                                         "KEY_RIGHT",
                                         "KEY_LEFT",
                                         "KEY_UP",
                                         "KEY_DOWN",
                                         "KEY_R",
                                         "KEY_L",
                                         "KEY_X",
                                         "KEY_Y",
                                         "LOG",
                                         "NO_POWER_THROTTLE",
                                         "SCENE_DUMP",
                                         "SCENE_DUMP_CAPTURE",
                                         "LAUNCHER_LANGUAGE",
                                         "LAUNCHER_CLOSE_ON_PLAY"};

int ini_key_known(const char* key)
{
  for (size_t i = 0; i < sizeof KNOWN_KEYS / sizeof *KNOWN_KEYS; i++)
    if (!strcmp(key, KNOWN_KEYS[i]))
      return 1;
  return 0;
}
