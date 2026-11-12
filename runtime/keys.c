//keys.c: implementation of the key bindings. see keys.h for the role and the design choices
#include "keys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//---- names -------------------------------------------------------------------------------------
#define KEY_NAME(id, name) name,
static const char* const KEY_NAMES[TK_COUNT] = {"", KEY_LIST(KEY_NAME)};
#undef KEY_NAME

static const char* const BUTTON_NAMES[BTN_COUNT] = {
    "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"};

static const char* const PRESET_NAMES[PRESET_COUNT] = {"wasd", "classic"};

//compare two texts ignoring case. returns 1 when equal
static int same_text(const char* a, const char* b)
{
  for (; *a && *b; a++, b++)
  {
    char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
    char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
    if (x != y)
      return 0;
  }
  return *a == *b;
}

const char* key_name(int key)
{
  return key > 0 && key < TK_COUNT ? KEY_NAMES[key] : "";
}

int key_from_name(const char* name)
{
  if (!name || !*name)
    return TK_NONE;
  for (int i = 1; i < TK_COUNT; i++)
    if (same_text(name, KEY_NAMES[i]))
      return i;
  return TK_NONE;
}

const char* button_name(DsButton button)
{
  return button >= 0 && button < BTN_COUNT ? BUTTON_NAMES[button] : "";
}

int button_from_name(const char* name)
{
  if (!name)
    return -1;
  for (int i = 0; i < BTN_COUNT; i++)
    if (same_text(name, BUTTON_NAMES[i]))
      return i;
  return -1;
}

const char* preset_name(Preset preset)
{
  return preset >= 0 && preset < PRESET_COUNT ? PRESET_NAMES[preset] : "";
}

int preset_from_name(const char* name)
{
  if (!name)
    return -1;
  for (int i = 0; i < PRESET_COUNT; i++)
    if (same_text(name, PRESET_NAMES[i]))
      return i;
  return -1;
}

//---- editing -----------------------------------------------------------------------------------
static int valid_slot(DsButton button, int slot)
{
  return button >= 0 && button < BTN_COUNT && slot >= 0 && slot < BIND_SLOTS;
}

void bind_clear(Bindings* b, DsButton button, int slot)
{
  if (valid_slot(button, slot))
    b->key[button][slot] = TK_NONE;
}

int bind_set(Bindings* b, DsButton button, int slot, int key)
{
  if (!valid_slot(button, slot) || key <= TK_NONE || key >= TK_COUNT)
    return 0;
  //the key leaves every other slot first. newest binding wins
  for (int i = 0; i < BTN_COUNT; i++)
    for (int s = 0; s < BIND_SLOTS; s++)
      if (b->key[i][s] == key)
        b->key[i][s] = TK_NONE;
  b->key[button][slot] = (uint8_t)key;
  return 1;
}

void bind_format(const Bindings* b, DsButton button, char* out, size_t size)
{
  if (size == 0)
    return;
  out[0] = 0;
  if (button < 0 || button >= BTN_COUNT)
    return;
  const char* first = key_name(b->key[button][0]);
  const char* second = key_name(b->key[button][1]);
  if (*second)
    snprintf(out, size, "%s,%s", first, second); //empty first slot keeps its place: ",Enter"
  else
    snprintf(out, size, "%s", first);
}

//split "W,Enter" and set slot 1 and slot 2. an empty or unknown token leaves its slot empty
int bind_parse(Bindings* b, DsButton button, const char* text)
{
  if (button < 0 || button >= BTN_COUNT)
    return 0;
  for (int s = 0; s < BIND_SLOTS; s++)
    b->key[button][s] = TK_NONE;
  int accepted = 0;
  for (int slot = 0; slot < BIND_SLOTS && text; slot++)
  {
    const char* end = strchr(text, ',');
    size_t length = end ? (size_t)(end - text) : strlen(text);
    char token[24];
    if (length >= sizeof token)
      length = sizeof token - 1;
    memcpy(token, text, length);
    token[length] = 0;
    //trim spaces on both sides
    char* start = token;
    while (*start == ' ')
      start++;
    for (size_t n = strlen(start); n > 0 && start[n - 1] == ' '; n--)
      start[n - 1] = 0;
    if (bind_set(b, button, slot, key_from_name(start)))
      accepted++;
    text = end ? end + 1 : NULL;
  }
  return accepted;
}

//---- presets -----------------------------------------------------------------------------------
typedef struct
{
  DsButton button;
  const char* keys;
} PresetRow;

//racing layout. W accelerates and S brakes. A and D steer with the arrows as a second option
//Space drifts. Shift throws the item. Shift + Up arrow throws it forward (the game reads L +
//d-pad)
static const PresetRow WASD[] = {
    {BTN_A, "W,Enter"},       {BTN_B, "S,Backspace"}, {BTN_X, "E"},         {BTN_Y, "Q"},
    {BTN_L, "LShift,RShift"}, {BTN_R, "Space"},       {BTN_START, "Tab"},   {BTN_SELECT, "RCtrl"},
    {BTN_UP, "Up"},           {BTN_DOWN, "Down"},     {BTN_LEFT, "A,Left"}, {BTN_RIGHT, "D,Right"},
};

//the layout of the first port: arrows for the d-pad and X Z S A Q W for the face and shoulder
//buttons
static const PresetRow CLASSIC[] = {
    {BTN_A, "X"},   {BTN_B, "Z"},       {BTN_X, "S"},         {BTN_Y, "A"},
    {BTN_L, "Q"},   {BTN_R, "W"},       {BTN_START, "Enter"}, {BTN_SELECT, "Backspace"},
    {BTN_UP, "Up"}, {BTN_DOWN, "Down"}, {BTN_LEFT, "Left"},   {BTN_RIGHT, "Right"},
};

void bind_preset(Bindings* b, Preset preset)
{
  memset(b, 0, sizeof *b);
  const PresetRow* rows = preset == PRESET_CLASSIC ? CLASSIC : WASD;
  for (int i = 0; i < BTN_COUNT; i++)
    bind_parse(b, rows[i].button, rows[i].keys);
}

//---- loading -----------------------------------------------------------------------------------
void bind_load(Bindings* b, SettingGetter get)
{
  int preset = preset_from_name(get("PRESET"));
  bind_preset(b, preset < 0 ? PRESET_WASD : (Preset)preset);
  for (int i = 0; i < BTN_COUNT; i++)
  {
    char name[24];
    snprintf(name, sizeof name, "KEY_%s", BUTTON_NAMES[i]);
    const char* value = get(name);
    if (value)
      bind_parse(b, (DsButton)i, value); //a present line wins, even an empty one
  }
}

//settings.ini lines reach the game as DRIFT_<KEY> environment variables (read_settings in main.c)
static const char* env_getter(const char* name)
{
  char variable[40];
  snprintf(variable, sizeof variable, "DRIFT_%s", name);
  return getenv(variable);
}

void bind_load_env(Bindings* b)
{
  bind_load(b, env_getter);
}

//---- live state --------------------------------------------------------------------------------
Bindings* bind_table(void)
{
  static Bindings table;
  static int loaded;
  if (!loaded)
  {
    bind_load_env(&table);
    loaded = 1;
  }
  return &table;
}

uint16_t bind_pressed_mask(const Bindings* b, const uint8_t down[TK_COUNT])
{
  uint16_t mask = 0;
  for (int i = 0; i < BTN_COUNT; i++)
    if (down[b->key[i][0]] || down[b->key[i][1]])
      mask |= (uint16_t)(1u << i); //down[TK_NONE] is always 0
  return mask;
}
