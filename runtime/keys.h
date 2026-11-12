//keys.h: remappable keyboard and mouse bindings for the 12 DS buttons
//
//role
//keeps which keys press which DS button. knows nothing about windows or sdl
//the window code (window.c) turns native key codes into the ids below and asks for the pressed
//mask the menu (menu.cpp) edits the table. the launcher writes the same settings text (see
//settings.ini below)
//
//design choices
// - key ids come from one list (KEY_LIST) so enum, names and parser cannot drift apart
// - keys that must stay fixed (F1, F10, F11, Esc, Alt, Windows keys, left mouse) are not in the
// list
//   so no code path can bind them
// - each DS button has two slots. a key lives in one slot of one button only, the newest binding
// wins
// - per event cost is table lookups only. no allocation and no string compare after load
//
//settings text in settings.ini
//  PRESET=wasd or PRESET=classic
//  TECLA_A=W,Enter   one line per DS button: TECLA_A B X Y L R START SELECT UP DOWN LEFT RIGHT
//  the value lists slot 1 and slot 2. an empty value clears the button. a missing line keeps the
//  preset
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  //DS buttons. the first 10 follow the bit order of the KEYINPUT register. X and Y are bits 10 and
  //11
  typedef enum
  {
    BTN_A,
    BTN_B,
    BTN_SELECT,
    BTN_START,
    BTN_RIGHT,
    BTN_LEFT,
    BTN_UP,
    BTN_DOWN,
    BTN_R,
    BTN_L,
    BTN_X,
    BTN_Y,
    BTN_COUNT
  } DsButton;

//every key and mouse button that can be bound. ITEM(id, "name"). the name is what the settings
//file uses letters, digits, F keys and numpad keys stay in order, window.c relies on that to fill
//its table the id DEL is not called DELETE because windows.h defines a macro with that name
#define KEY_LIST(ITEM)                                                                             \
  ITEM(A, "A")                                                                                     \
  ITEM(B, "B")                                                                                     \
  ITEM(C, "C")                                                                                     \
  ITEM(D, "D") ITEM(E, "E") ITEM(F, "F") ITEM(G, "G") ITEM(H, "H") ITEM(I, "I") ITEM(J, "J") ITEM( \
      K, "K") ITEM(L, "L") ITEM(M, "M") ITEM(N, "N") ITEM(O, "O") ITEM(P, "P") ITEM(Q, "Q")        \
      ITEM(R, "R") ITEM(S, "S") ITEM(T, "T") ITEM(U, "U") ITEM(V, "V") ITEM(W, "W") ITEM(          \
          X, "X") ITEM(Y, "Y") ITEM(Z, "Z") ITEM(0, "0") ITEM(1, "1") ITEM(2, "2") ITEM(3, "3")    \
          ITEM(4, "4") ITEM(5, "5") ITEM(6, "6") ITEM(7, "7") ITEM(8, "8") ITEM(9, "9") ITEM(      \
              F2, "F2") ITEM(F3, "F3") ITEM(F4, "F4") ITEM(F5, "F5") ITEM(F6, "F6") ITEM(F7, "F7") \
              ITEM(F8, "F8") ITEM(F9, "F9") ITEM(F12, "F12") ITEM(UP, "Up") ITEM(DOWN, "Down")     \
                  ITEM(LEFT, "Left") ITEM(RIGHT, "Right") ITEM(SPACE, "Space") ITEM(               \
                      ENTER, "Enter") ITEM(BACKSPACE, "Backspace") ITEM(TAB, "Tab")                \
                      ITEM(INSERT, "Insert") ITEM(DEL, "Delete") ITEM(HOME, "Home") ITEM(          \
                          END, "End") ITEM(PAGEUP, "PageUp")                                       \
                          ITEM(PAGEDOWN, "PageDown") ITEM(LSHIFT, "LShift") ITEM(RSHIFT, "RShift") \
                              ITEM(LCTRL, "LCtrl") ITEM(RCTRL, "RCtrl") ITEM(NUM0, "Num0")         \
                                  ITEM(NUM1, "Num1") ITEM(NUM2, "Num2") ITEM(NUM3, "Num3")         \
                                      ITEM(NUM4, "Num4") ITEM(NUM5, "Num5") ITEM(NUM6, "Num6")     \
                                          ITEM(NUM7, "Num7") ITEM(NUM8, "Num8") ITEM(NUM9, "Num9") \
                                              ITEM(COMMA, "Comma") ITEM(PERIOD, "Period") ITEM(    \
                                                  SLASH, "Slash") ITEM(SEMICOLON, "Semicolon")     \
                                                  ITEM(QUOTE, "Quote") ITEM(LBRACKET, "LBracket")  \
                                                      ITEM(RBRACKET, "RBracket") ITEM(BACKSLASH,   \
                                                                                      "Backslash") \
                                                          ITEM(MINUS, "Minus") ITEM(               \
                                                              EQUALS, "Equals") ITEM(LOW, "Low")   \
                                                              ITEM(MOUSE_RIGHT, "Mouse2")          \
                                                                  ITEM(MOUSE_MIDDLE, "Mouse3")     \
                                                                      ITEM(MOUSE_X1, "Mouse4")     \
                                                                          ITEM(MOUSE_X2, "Mouse5")

  //key ids. TK_NONE (0) means an empty slot
  enum
  {
    TK_NONE = 0,
#define KEY_ENUM(id, name) TK_##id,
    KEY_LIST(KEY_ENUM)
#undef KEY_ENUM
    TK_COUNT
  };

#define BIND_SLOTS 2

  //the whole table: two key ids for each DS button
  typedef struct
  {
    uint8_t key[BTN_COUNT][BIND_SLOTS];
  } Bindings;

  typedef enum
  {
    PRESET_WASD,
    PRESET_CLASSIC,
    PRESET_COUNT
  } Preset;

  //names. all lookups ignore case. unknown and reserved names give TK_NONE or -1
  const char* key_name(int key); //"W". empty text for TK_NONE and bad ids
  int key_from_name(const char* name);
  const char* button_name(DsButton button); //"A", "START". the text used after TECLA_
  int button_from_name(const char* name);
  const char* preset_name(Preset preset); //"wasd", "classic"
  int preset_from_name(const char* name);

  //edit the table
  void bind_preset(Bindings* b, Preset preset);
  void bind_clear(Bindings* b, DsButton button, int slot);
  int bind_set(Bindings* b, DsButton button, int slot,
               int key); //1 on success. the key leaves any other slot
  void bind_format(const Bindings* b, DsButton button, char* out, size_t size); //"W,Enter"
  int bind_parse(Bindings* b, DsButton button,
                 const char* text); //sets both slots from "W,Enter". returns keys accepted

  //load from settings. get(name) returns the value of PRESET or TECLA_x, or NULL when the line is
  //missing order: preset first, then each TECLA_ line on top of it
  typedef const char* (*SettingGetter)(const char* name);
  void bind_load(Bindings* b, SettingGetter get);
  void bind_load_env(
      Bindings* b); //same, reading the DRIFT_ environment variables (settings.ini lands there)

  //the one live table. the window reads it and the menu edits it. it loads from the environment on
  //first use
  Bindings* bind_table(void);

  //pressed DS buttons as a mask, one bit for each DsButton. down[] has one byte for each key id, 1
  //= held
  uint16_t bind_pressed_mask(const Bindings* b, const uint8_t down[TK_COUNT]);

#ifdef __cplusplus
}
#endif
