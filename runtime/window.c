//PS5/PS4 gamepad (read directly over USB or Bluetooth): cross = A, circle = B,
//square = Y, triangle = X, L1/L2 = L, R1/R2 = R, Options = Start, Create = Select
#include <windows.h>
#include <xinput.h>

#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <GL/gl.h>
#include "video.h"
#include "keys.h"
#include "log.h"
#include "sysinfo.h"
#include "winmode.h"
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

void save_write_pending(void); //ds.c. not included from ds.h: its R macro clashes with windows.h

#define SCALE 2

volatile uint16_t g_keyinput = 0x3FF;
volatile uint16_t g_keyxy = 0x3;
volatile int g_stylus, g_stylus_x, g_stylus_y;

static HWND window;
//images are painted in a separate thread (paint_thread), with OpenGL and at the pace of the
//monitor. They go through a queue of NM images, each with the time it is due to be shown
//(QueryPerformanceCounter): so the in-between images (DRIFT_FPS: 120, 180 or 240) fit spread
//between two game frames. On each refresh the painter shows the newest one whose time has come. If
//it runs late it skips the extras. The game never waits.
typedef struct
{
  uint32_t low[256 * 384];
  uint32_t hd[1024 * 1536];
} Frame;
#define NM 8
static Frame frames[NM];
static int64_t frame_ts[NM]; //when to show it. 0 = not ready yet
static int i_write; //the next one to be filled
static volatile int i_viewing = -1; //the one the painter has (not touched)
static volatile LONG pictures_shown; //different images shown (for the title: those seen per second)
static int i_reserved;
static const Frame* last = &frames[0]; //the last complete game image (for BMP captures)
static CRITICAL_SECTION cs_frames, cs_gdi;
static HANDLE ev_paint;
static uint32_t* image = frames[0].low; //the one that compose() paints
static BITMAPINFO bmi;
static volatile uint16_t keyboard_in = 0x3FF, keyboard_xy = 3; //keyboard state (0 = pressed)
//where the image is drawn inside the window (changes in full screen)
static int dst_x, dst_y, dst_w = 256 * SCALE, dst_h = 384 * SCALE;
//how the window is shown now (WINMODE_*, see winmode.h). Written on the window thread under
//cs_gdi, read by the paint thread
static volatile int window_mode = WINMODE_WINDOWED;
//both fullscreen modes show the two screens side by side, the windowed one stacks them
static int is_full(void)
{
  return window_mode != WINMODE_WINDOWED;
}
//in full screen: the two screens side by side (main on the left, touch on the right)
static uint32_t image_side[512 * 192];
static BITMAPINFO bmi_side;
static RECT rect_normal;
//Rescale (DRIFT_SCALE > 1): the same image at big size, with the 3D painted at more resolution
int video_scale(void);
const uint32_t* video_hd(int bottom);
static uint32_t *image_hd = frames[0].hd, image_side_hd[2048 * 768];
static BITMAPINFO bmi_hd, bmi_side_hd;
static LONG style_normal;

static void calculate_target(void)
{
  RECT r;
  GetClientRect(window, &r);
  int w = r.right, h = r.bottom;
  if (w <= 0 || h <= 0)
    return;
  int iw = is_full() ? 512 : 256, ih = is_full() ? 192 : 384; //size of the image
  if (w * ih > h * iw)
  {
    dst_h = h;
    dst_w = h * iw / ih;
  } //spare width: bands on the sides
  else
  {
    dst_w = w;
    dst_h = w * ih / iw;
  } //spare height: bands above and below
  dst_x = (w - dst_w) / 2;
  dst_y = (h - dst_h) / 2;
}

//---- Toast: a small label at the top right that names the new window mode for a moment ----
//set_mode() stores the text and the time under cs_gdi. The paint thread reads them on every
//picture, so the label shows and fades without any extra thread or timer
#define TOAST_MS 2000
#define TOAST_FADE_MS 500
static wchar_t toast_text[64];
static int64_t toast_start; //QueryPerformanceCounter ticks when it appeared. 0 = never shown
static int toast_serial; //grows with every new toast, so the painted label is rebuilt

static void toast_show(const wchar_t* text)
{
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  EnterCriticalSection(&cs_gdi);
  wcsncpy(toast_text, text, 63);
  toast_text[63] = 0;
  toast_start = now.QuadPart;
  toast_serial++;
  LeaveCriticalSection(&cs_gdi);
}

//1 while the toast is fully shown, fading to 0 in its last half second, 0 when it is gone
static float toast_alpha(int64_t start)
{
  if (!start)
    return 0.0f;
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  double ms = (double)(now.QuadPart - start) * 1000.0 / (double)freq.QuadPart;
  if (ms < 0.0 || ms >= TOAST_MS)
    return 0.0f;
  if (ms > TOAST_MS - TOAST_FADE_MS)
    return (float)((TOAST_MS - ms) / TOAST_FADE_MS);
  return 1.0f;
}

//letter height in pixels: follows the window height so it reads well at 1080p and in a small
//window
static int toast_px(int window_height)
{
  int px = window_height / 28;
  return px < 18 ? 18 : px;
}

static HFONT toast_font(int px)
{
  return CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

//the label on a GDI device context (only used when OpenGL is not available)
static void toast_draw_gdi(HDC dc, int cw, int ch)
{
  wchar_t text[64];
  EnterCriticalSection(&cs_gdi);
  wcscpy(text, toast_text);
  int64_t start = toast_start;
  LeaveCriticalSection(&cs_gdi);
  if (toast_alpha(start) <= 0.0f)
    return;
  int px = toast_px(ch);
  HFONT font = toast_font(px);
  HGDIOBJ old = SelectObject(dc, font);
  SIZE size;
  GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
  int pad = px / 2, w = size.cx + 2 * pad, h = size.cy + pad;
  RECT box = {cw - w - pad, pad, cw - pad, pad + h};
  HBRUSH back = CreateSolidBrush(RGB(24, 24, 24));
  FillRect(dc, &box, back);
  DeleteObject(back);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  TextOutW(dc, box.left + pad, box.top + pad / 2, text, (int)wcslen(text));
  SelectObject(dc, old);
  DeleteObject(font);
}

static void compose(HDC dc)
{ //draws everything (image, bands) on dc
  RECT r;
  GetClientRect(window, &r);
  HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
  RECT b1 = {0, 0, r.right, dst_y}, b2 = {0, dst_y + dst_h, r.right, r.bottom};
  RECT b3 = {0, dst_y, dst_x, dst_y + dst_h}, b4 = {dst_x + dst_w, dst_y, r.right, dst_y + dst_h};
  FillRect(dc, &b1, black);
  FillRect(dc, &b2, black);
  FillRect(dc, &b3, black);
  FillRect(dc, &b4, black);
  SetStretchBltMode(dc, COLORONCOLOR);
  int e = video_scale();
  if (e > 1 && is_full())
  {
    int an = 256 * e;
    for (int y = 0; y < 192 * e; y++)
    {
      memcpy(image_side_hd + (size_t)y * an * 2, image_hd + (size_t)y * an, (size_t)an * 4);
      memcpy(image_side_hd + (size_t)y * an * 2 + an, image_hd + (size_t)(192 * e + y) * an,
             (size_t)an * 4);
    }
    StretchDIBits(dc, dst_x, dst_y, dst_w, dst_h, 0, 0, 512 * e, 192 * e, image_side_hd,
                  &bmi_side_hd, DIB_RGB_COLORS, SRCCOPY);
  }
  else if (e > 1)
  {
    StretchDIBits(dc, dst_x, dst_y, dst_w, dst_h, 0, 0, 256 * e, 384 * e, image_hd, &bmi_hd,
                  DIB_RGB_COLORS, SRCCOPY);
  }
  else if (is_full())
  {
    for (int y = 0; y < 192; y++)
    { //left: top screen. right: bottom screen
      memcpy(image_side + y * 512, image + y * 256, 256 * 4);
      memcpy(image_side + y * 512 + 256, image + (192 + y) * 256, 256 * 4);
    }
    StretchDIBits(dc, dst_x, dst_y, dst_w, dst_h, 0, 0, 512, 192, image_side, &bmi_side,
                  DIB_RGB_COLORS, SRCCOPY);
  }
  else
  {
    StretchDIBits(dc, dst_x, dst_y, dst_w, dst_h, 0, 0, 256, 384, image, &bmi, DIB_RGB_COLORS,
                  SRCCOPY);
  }
  toast_draw_gdi(dc, r.right, r.bottom);
}

//Double buffer: composed in an invisible image and sent to the screen at once
//(otherwise a flicker shows between painting the game and painting the notice).
static HDC mem_dc;
static HBITMAP mem_bmp;
static int mem_w, mem_h;

static void draw_en(HDC dc)
{
  RECT r;
  GetClientRect(window, &r);
  if (r.right <= 0 || r.bottom <= 0)
    return;
  if (!mem_dc || mem_w != r.right || mem_h != r.bottom)
  {
    if (mem_bmp)
      DeleteObject(mem_bmp);
    if (!mem_dc)
      mem_dc = CreateCompatibleDC(dc);
    mem_bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
    SelectObject(mem_dc, mem_bmp);
    mem_w = r.right;
    mem_h = r.bottom;
  }
  compose(mem_dc);
  BitBlt(dc, 0, 0, mem_w, mem_h, mem_dc, 0, 0, SRCCOPY);
}

//---- Fullscreen: a real display mode switch (CDS_FULLSCREEN) ----
//The monitor is switched to the size set in FULLSCREEN_SIZE (empty = the size the desktop has
//now). The refresh rate is the desktop's if the size has it. The desktop mode comes back when
//fullscreen ends, and Windows reverts it by itself if the game dies.
static wchar_t full_device[32]; //the monitor that was taken over
static int full_w, full_h, full_hz; //the mode it was switched to, kept to take it over again
static int desk_w, desk_h, desk_hz; //the mode it had before (for the log)
static int want_full_w, want_full_h; //FULLSCREEN_SIZE from settings.ini. 0 = the desktop size
static int display_taken; //1 while the display mode is ours

//takes the display mode over. fresh = choose the mode first (otherwise the one kept from the last
//time is used, because a minimized window cannot say which monitor it is on).
//1 = done, 0 = failed (the reason is logged)
static int display_take(int fresh)
{
  if (fresh && !sysinfo_pick_mode(window, want_full_w, want_full_h, &full_w, &full_h, &full_hz,
                                  full_device, &desk_w, &desk_h, &desk_hz))
  {
    if (want_full_w > 0)
      log_msg("window", "fullscreen: this monitor has no %dx%d mode (or Windows would not say)",
              want_full_w, want_full_h);
    else
      log_msg("window", "fullscreen: Windows would not say what the monitor is doing now");
    return 0;
  }
  DEVMODEW dm;
  memset(&dm, 0, sizeof dm);
  dm.dmSize = sizeof dm;
  dm.dmPelsWidth = (DWORD)full_w;
  dm.dmPelsHeight = (DWORD)full_h;
  dm.dmDisplayFrequency = (DWORD)full_hz;
  dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
  LONG r = ChangeDisplaySettingsExW(full_device, &dm, NULL, CDS_FULLSCREEN | CDS_TEST, NULL);
  if (r == DISP_CHANGE_SUCCESSFUL)
    r = ChangeDisplaySettingsExW(full_device, &dm, NULL, CDS_FULLSCREEN, NULL);
  if (r != DISP_CHANGE_SUCCESSFUL)
  {
    log_msg("window", "fullscreen: Windows refused %dx%d at %d Hz (code %ld)", full_w, full_h,
            full_hz, (long)r);
    return 0;
  }
  display_taken = 1;
  return 1;
}

//puts the desktop display mode back. Does nothing if it was never taken
static void display_restore(void)
{
  if (!display_taken)
    return;
  ChangeDisplaySettingsExW(full_device, NULL, NULL, 0, NULL);
  display_taken = 0;
}

//the window over its whole monitor, without border (both fullscreen modes use this). The monitor
//rectangle is read now, so after a display mode switch it is the new size.
//borderless = the borderless mode: the window is always one pixel bigger than the monitor, on an
//edge with no other monitor (not a setting, players cannot turn it off). A window that is not
//exactly the monitor size does not get the driver's direct way of showing the picture, which
//stalls when the Windows volume overlay appears. Bigger (not smaller) keeps the monitor covered,
//so no stripe of desktop shows
static void place_popup(int borderless)
{
  MONITORINFO mi = {sizeof mi};
  GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &mi);
  int x = mi.rcMonitor.left, y = mi.rcMonitor.top;
  int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
  if (borderless)
  {
    static const char* const side_name[4] = {"bottom", "right", "top", "left"};
    int edge = sysinfo_free_edge(mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right,
                                 mi.rcMonitor.bottom);
    if (edge == 0)
      h += 1;
    else if (edge == 1)
      w += 1;
    else if (edge == 2)
    {
      y -= 1;
      h += 1;
    }
    else if (edge == 3)
    {
      x -= 1;
      w += 1;
    }
    if (edge >= 0)
      log_msg("window", "edge pixel: +1 px on the %s", side_name[edge]);
    else
      log_msg("window", "edge pixel: no free edge, skipped");
  }
  SetWindowLongW(window, GWL_STYLE,
                 (style_normal & ~(LONG)(WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)) | WS_POPUP);
  SetWindowPos(window, HWND_TOP, x, y, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static const wchar_t* toast_name(int mode)
{
  if (mode == WINMODE_FULLSCREEN)
    return L"Fullscreen";
  if (mode == WINMODE_BORDERLESS)
    return L"Borderless fullscreen";
  return L"Windowed";
}

//switches the window to a mode. startup = the first call, when the window was just made: no
//toast then. A fullscreen that Windows refuses becomes borderless fullscreen, and says so.
static void set_mode(int mode, int startup)
{
  int from = window_mode;
  //before the switch: the slow picture can end before or after the log line below. A display
  //mode switch takes longer: the monitor has to find its signal again
  log_quiet(mode == WINMODE_FULLSCREEN || from == WINMODE_FULLSCREEN ? 3.0 : 1.0);
  int actual = mode, failed = 0;
  if (from == WINMODE_WINDOWED && mode != WINMODE_WINDOWED)
  { //remember the normal window to come back to
    GetWindowRect(window, &rect_normal);
    style_normal = GetWindowLongW(window, GWL_STYLE);
  }
  if (mode == WINMODE_FULLSCREEN && !display_taken && !display_take(1))
  {
    actual = WINMODE_BORDERLESS;
    failed = 1;
  }
  if (actual != WINMODE_FULLSCREEN)
    display_restore();
  EnterCriticalSection(&cs_gdi);
  window_mode = actual; //before the window moves: the resize that follows must see the new mode
  LeaveCriticalSection(&cs_gdi);
  if (actual == WINMODE_WINDOWED)
  {
    SetWindowLongW(window, GWL_STYLE, style_normal);
    SetWindowPos(window, NULL, rect_normal.left, rect_normal.top,
                 rect_normal.right - rect_normal.left, rect_normal.bottom - rect_normal.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
  }
  else
  {
    place_popup(actual == WINMODE_BORDERLESS);
  }
  if (actual == WINMODE_FULLSCREEN)
    log_msg("window", "mode fullscreen, %dx%d at %d Hz, desktop was %dx%d at %d Hz (%s%s)", full_w,
            full_h, full_hz, desk_w, desk_h, desk_hz, startup ? "start-up" : "from ",
            startup ? "" : winmode_name(from));
  else
    log_msg("window", "mode %s (%s%s)", winmode_name(actual), startup ? "start-up" : "from ",
            startup ? "" : winmode_name(from));
  log_set_mode(winmode_name(actual));
  EnterCriticalSection(&cs_gdi);
  calculate_target();
  LeaveCriticalSection(&cs_gdi);
  if (failed)
  {
    log_msg("window", "fullscreen did not work, using borderless fullscreen");
    toast_show(L"Fullscreen failed, using borderless");
  }
  else if (!startup)
  {
    toast_show(toast_name(actual));
  }
  SetEvent(ev_paint);
}

//DRIFT_OCULTO=1: no window (tests without bothering), only captures

//before this the keys were a fixed switch here. now the user can remap them, see keys.h
//flow: native event -> key id -> key_down[] -> bind_pressed_mask -> keyboard_in and keyboard_xy
//window_messages later merges those with the controller state

//native key code to key id. built once. 0 means the key cannot be bound
static uint8_t vk_to_key[256];
static uint8_t key_down[TK_COUNT]; //1 = held. indexed by key id

static void build_key_table(void)
{
  for (int i = 0; i < 26; i++)
    vk_to_key['A' + i] = (uint8_t)(TK_A + i);
  for (int i = 0; i < 10; i++)
  {
    vk_to_key['0' + i] = (uint8_t)(TK_0 + i);
    vk_to_key[VK_NUMPAD0 + i] = (uint8_t)(TK_NUM0 + i);
  }
  for (int i = 0; i < 8; i++)
    vk_to_key[VK_F2 + i] = (uint8_t)(TK_F2 + i); //F2 to F9. F1 F10 F11 stay fixed
  vk_to_key[VK_F12] = TK_F12;
  vk_to_key[VK_UP] = TK_UP;
  vk_to_key[VK_DOWN] = TK_DOWN;
  vk_to_key[VK_LEFT] = TK_LEFT;
  vk_to_key[VK_RIGHT] = TK_RIGHT;
  vk_to_key[VK_SPACE] = TK_SPACE;
  vk_to_key[VK_RETURN] = TK_ENTER;
  vk_to_key[VK_BACK] = TK_BACKSPACE;
  vk_to_key[VK_TAB] = TK_TAB;
  vk_to_key[VK_INSERT] = TK_INSERT;
  vk_to_key[VK_DELETE] = TK_DEL;
  vk_to_key[VK_HOME] = TK_HOME;
  vk_to_key[VK_END] = TK_END;
  vk_to_key[VK_PRIOR] = TK_PAGEUP;
  vk_to_key[VK_NEXT] = TK_PAGEDOWN;
  vk_to_key[VK_OEM_COMMA] = TK_COMMA;
  vk_to_key[VK_OEM_PERIOD] = TK_PERIOD;
  vk_to_key[VK_OEM_2] = TK_SLASH;
  vk_to_key[VK_OEM_1] = TK_SEMICOLON;
  vk_to_key[VK_OEM_7] = TK_QUOTE;
  vk_to_key[VK_OEM_4] = TK_LBRACKET;
  vk_to_key[VK_OEM_6] = TK_RBRACKET;
  vk_to_key[VK_OEM_5] = TK_BACKSLASH;
  vk_to_key[VK_OEM_MINUS] = TK_MINUS;
  vk_to_key[VK_OEM_PLUS] = TK_EQUALS;
  vk_to_key[VK_OEM_3] = TK_LOW;
}

//key id of a WM_KEYDOWN or WM_KEYUP. Windows sends one generic code for both Shift and both Ctrl
//keys the scancode and the extended bit in lp tell which side it was
static int key_from_event(WPARAM vk, LPARAM lp)
{
  if (vk == VK_SHIFT)
    return MapVirtualKeyW((UINT)((lp >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? TK_RSHIFT
                                                                                      : TK_LSHIFT;
  if (vk == VK_CONTROL)
    return (lp & (1 << 24)) ? TK_RCTRL : TK_LCTRL;
  return vk < 256 ? vk_to_key[vk] : TK_NONE;
}

//rebuild the keyboard state of the DS buttons from the held keys. bits are 0 when pressed
static void update_keyboard(void)
{
  uint16_t pressed = bind_pressed_mask(bind_table(), key_down);
  keyboard_in = (uint16_t)(0x3FF & ~(pressed & 0x3FF));
  keyboard_xy = (uint16_t)(3 & ~(pressed >> 10));
}

//let go of everything. used on focus loss and when the menu opens so no key stays stuck
static void release_all_keys(void)
{
  memset(key_down, 0, sizeof key_down);
  update_keyboard();
}

//one key or mouse button changed
static int key_event(int key, int down)
{
  if (key == TK_NONE)
    return 0;
  key_down[key] = down ? 1 : 0;
  update_keyboard();
  return 0;
}

//for microphone.c (MIC_KEY): is the key held now. 0 when the window lost focus
int window_key_held(int key)
{
  return key > 0 && key < TK_COUNT && key_down[key];
}

static void mouse(LPARAM lp, int is_down)
{
  if (dst_w <= 0 || dst_h <= 0)
    return;
  int x, y;
  if (is_full())
  { //the touch screen is the right half
    x = ((short)LOWORD(lp) - dst_x) * 512 / dst_w - 256;
    y = ((short)HIWORD(lp) - dst_y) * 192 / dst_h;
  }
  else
  {
    x = ((short)LOWORD(lp) - dst_x) * 256 / dst_w;
    y = ((short)HIWORD(lp) - dst_y) * 384 / dst_h - 192;
  }
  if (is_down && x >= 0 && x < 256 && y >= 0 && y < 192)
  {
    g_stylus_x = x;
    g_stylus_y = y;
    g_stylus = 1;
  }
  else
  {
    g_stylus = 0;
  }
}

static void gl_shutdown(void); //defined with the OpenGL code below

//a volume key (the knob on some keyboards) changes the Windows volume. It is only written to the
//log, so a freeze can be matched to the moment of a turn. At most one line per 150 ms: a fast
//spin of the knob sends many. Window thread only. The key still does what it did before
static void note_volume_key(const char* what)
{
  static ULONGLONG last;
  ULONGLONG now = GetTickCount64();
  if (last && now - last < 150)
    return;
  last = now;
  log_msg("window", "volume key: %s", what);
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
  case WM_CLOSE:
  case WM_DESTROY:
    log_msg("window", "closing");
    save_write_pending(); //keep progress made in the last half second
    gl_shutdown();
    display_restore(); //the desktop must get its display mode back
    log_flush();
    ExitProcess(0);
  case WM_APPCOMMAND:
    //sent when Windows turns a media key into a command. Only noted: the break below hands it
    //to DefWindowProcW, which passes it on, so the shell still changes the volume
    if (GET_APPCOMMAND_LPARAM(lp) == APPCOMMAND_VOLUME_UP)
      note_volume_key("up");
    else if (GET_APPCOMMAND_LPARAM(lp) == APPCOMMAND_VOLUME_DOWN)
      note_volume_key("down");
    else if (GET_APPCOMMAND_LPARAM(lp) == APPCOMMAND_VOLUME_MUTE)
      note_volume_key("mute");
    break;
  case WM_KEYDOWN:
    if (wp == VK_VOLUME_UP)
      note_volume_key("up");
    else if (wp == VK_VOLUME_DOWN)
      note_volume_key("down");
    else if (wp == VK_VOLUME_MUTE)
      note_volume_key("mute");
    if (wp == VK_F11 || (wp == VK_ESCAPE && is_full()))
    {
      if (!(lp & (1 << 30))) //holding the key must not spin through the modes
        set_mode(wp == VK_F11 ? winmode_next(window_mode) : WINMODE_WINDOWED, 0);
      return 0;
    }
    key_event(key_from_event(wp, lp), 1);
    return 0;
  case WM_ACTIVATEAPP:
    //fullscreen owns the display, like a game on the old exclusive way: when another program
    //comes in front, the desktop gets its mode back and the game minimizes. When the game is
    //chosen again it takes the display back. Windowed and borderless need none of this
    if (window_mode == WINMODE_FULLSCREEN)
    {
      log_quiet(1.0);
      if (!wp && display_taken)
      {
        display_restore();
        ShowWindow(h, SW_MINIMIZE);
        log_msg("window", "fullscreen paused: another program is in front");
      }
      else if (wp && !display_taken)
      {
        if (display_take(0))
        {
          ShowWindow(h, SW_RESTORE);
          place_popup(0);
          log_msg("window", "fullscreen back");
        }
        else
        {
          set_mode(WINMODE_BORDERLESS, 0); //Windows refused: keep playing, borderless
        }
      }
    }
    return 0;
  case WM_KILLFOCUS:
    release_all_keys(); //a key released in another window would never send its up event here
    log_msg("window", "lost focus");
    return 0;
  case WM_SETFOCUS:
    log_msg("window", "got focus");
    return 0;
  case WM_RBUTTONDOWN:
    key_event(TK_MOUSE_RIGHT, 1);
    return 0;
  case WM_RBUTTONUP:
    key_event(TK_MOUSE_RIGHT, 0);
    return 0;
  case WM_MBUTTONDOWN:
    key_event(TK_MOUSE_MIDDLE, 1);
    return 0;
  case WM_MBUTTONUP:
    key_event(TK_MOUSE_MIDDLE, 0);
    return 0;
  case WM_XBUTTONDOWN:
    key_event(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? TK_MOUSE_X1 : TK_MOUSE_X2, 1);
    return TRUE;
  case WM_XBUTTONUP:
    key_event(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? TK_MOUSE_X1 : TK_MOUSE_X2, 0);
    return TRUE;
  case WM_SIZE:
    EnterCriticalSection(&cs_gdi);
    calculate_target();
    LeaveCriticalSection(&cs_gdi);
    SetEvent(ev_paint);
    return 0;
  case WM_ERASEBKGND:
    return 1;
  case WM_KEYUP:
    //release also for the game (it could have been pressed before the menu opened)
    //also release for the game: the key may have been pressed before the menu opened
    key_event(key_from_event(wp, lp), 0);
    return 0;
  case WM_LBUTTONDOWN:
    SetCapture(h);
    mouse(lp, 1);
    return 0;
  case WM_MOUSEMOVE:
    if (wp & MK_LBUTTON)
      mouse(lp, 1);
    return 0;
  case WM_LBUTTONUP:
    ReleaseCapture();
    mouse(lp, 0);
    return 0;
  case WM_PAINT:
  { //painted by paint_thread (here, on the game thread, nothing is waited for)
    PAINTSTRUCT ps;
    BeginPaint(h, &ps);
    EndPaint(h, &ps);
    SetEvent(ev_paint);
    return 0;
  }
  }
  return DefWindowProcW(h, msg, wp, lp);
}

static int64_t clock(void)
{
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

//the newest image whose time has come (and the painter keeps it). -1 if there is none
static int pick_frame(void)
{
  int64_t ya = clock();
  EnterCriticalSection(&cs_frames);
  int best = -1;
  for (int i = 0; i < NM; i++)
    if (frame_ts[i] && frame_ts[i] <= ya && (best < 0 || frame_ts[i] > frame_ts[best]))
      best = i;
  if (best >= 0)
    i_viewing = best;
  LeaveCriticalSection(&cs_frames);
  return best;
}

//---- OpenGL ----
static GLuint tex_game;
static int tex_w, tex_h;

//orderly OpenGL shutdown. the paint thread owns the context and sits in the driver's vsync wait,
//so calling ExitProcess under it made the driver unwind during process kill (about 5 s on close)
//the paint thread releases the context itself and the closing code waits for that, at most 3 s
static HGLRC gl_rc;
static volatile LONG gl_closing;
static volatile LONG gl_active;
static HANDLE ev_gl_done;

static void gl_shutdown(void)
{
  if (!gl_active)
    return;
  InterlockedExchange(&gl_closing, 1);
  DWORD r = WaitForSingleObject(ev_gl_done, 3000);
}

static int gl_init(HDC dc)
{
  PIXELFORMATDESCRIPTOR pfd = {
      sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
  int pf = ChoosePixelFormat(dc, &pfd);
  if (!pf || !SetPixelFormat(dc, pf, &pfd))
    return 0;
  HGLRC rc = wglCreateContext(dc);
  if (!rc || !wglMakeCurrent(dc, rc))
    return 0;
  gl_rc = rc;
  typedef BOOL(WINAPI * Interval)(int);
  Interval interval = (Interval)(void*)wglGetProcAddress("wglSwapIntervalEXT");
  if (!interval || !interval(1))
    return 0; //without sync to the monitor it is not worth it
  glGenTextures(1, &tex_game);
  return 1;
}

static void gl_rect(float x, float y, float w, float h, float u0, float v0, float u1, float v1)
{
  glBegin(GL_QUADS);
  glTexCoord2f(u0, v0);
  glVertex2f(x, y);
  glTexCoord2f(u1, v0);
  glVertex2f(x + w, y);
  glTexCoord2f(u1, v1);
  glVertex2f(x + w, y + h);
  glTexCoord2f(u0, v1);
  glVertex2f(x, y + h);
  glEnd();
}

//---- The toast with OpenGL: the text is drawn once with GDI into a small image, which becomes a
//texture and is drawn as one more rectangle over the game, with the fade as its opacity ----
static GLuint tex_toast;
static int toast_built_serial = -1, toast_built_px, toast_w, toast_h;

static void toast_build(const wchar_t* text, int px)
{
  HDC dc = CreateCompatibleDC(NULL);
  HFONT font = toast_font(px);
  HGDIOBJ old_font = SelectObject(dc, font);
  SIZE size;
  GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
  int pad = px / 2, w = size.cx + 2 * pad, h = size.cy + pad;
  BITMAPINFO info;
  memset(&info, 0, sizeof info);
  info.bmiHeader.biSize = sizeof info.bmiHeader;
  info.bmiHeader.biWidth = w;
  info.bmiHeader.biHeight = -h; //top to bottom
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  uint32_t* bits = NULL;
  HBITMAP bmp = CreateDIBSection(dc, &info, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
  if (bmp && bits)
  {
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    RECT all = {0, 0, w, h};
    HBRUSH back = CreateSolidBrush(RGB(24, 24, 24));
    FillRect(dc, &all, back);
    DeleteObject(back);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, pad, pad / 2, text, (int)wcslen(text));
    GdiFlush();
    for (int k = 0; k < w * h; k++)
      bits[k] |= 0xFF000000u; //GDI leaves the alpha empty: make every pixel solid
    if (!tex_toast)
    {
      glGenTextures(1, &tex_toast);
      glBindTexture(GL_TEXTURE_2D, tex_toast);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindTexture(GL_TEXTURE_2D, tex_toast);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, bits);
    toast_w = w;
    toast_h = h;
    SelectObject(dc, old_bmp);
  }
  if (bmp)
    DeleteObject(bmp);
  SelectObject(dc, old_font);
  DeleteObject(font);
  DeleteDC(dc);
}

static void toast_draw_gl(int cw, int ch)
{
  wchar_t text[64];
  EnterCriticalSection(&cs_gdi);
  wcscpy(text, toast_text);
  int64_t start = toast_start;
  int serial = toast_serial;
  LeaveCriticalSection(&cs_gdi);
  float alpha = toast_alpha(start);
  if (alpha <= 0.0f)
    return;
  int px = toast_px(ch);
  if (serial != toast_built_serial || px != toast_built_px)
  {
    toast_build(text, px);
    toast_built_serial = serial;
    toast_built_px = px;
  }
  if (!tex_toast || toast_w <= 0)
    return;
  glBindTexture(GL_TEXTURE_2D, tex_toast);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(1, 1, 1, alpha * 0.9f);
  float margin = (float)px / 2;
  gl_rect((float)cw - (float)toast_w - margin, margin, (float)toast_w, (float)toast_h, 0, 0, 1, 1);
  glDisable(GL_BLEND);
  glColor4f(1, 1, 1, 1);
}

static void gl_paint(HDC dc, int i, int upload)
{
  RECT r;
  GetClientRect(window, &r);
  int e = video_scale(), iw = 256 * e, ih = 384 * e;
  const uint32_t* px = e > 1 ? frames[i].hd : frames[i].low;
  glBindTexture(GL_TEXTURE_2D, tex_game);
  if (tex_w != iw || tex_h != ih)
  {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLint filter = e > 1
                       ? GL_LINEAR
                       : GL_NEAREST; //with rescale it is reduced: smooth. Without it, sharp pixels
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iw, ih, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
    tex_w = iw;
    tex_h = ih;
  }
  else if (upload)
  {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, iw, ih, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px);
  }
  glViewport(0, 0, r.right, r.bottom);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, r.right, r.bottom, 0, -1, 1);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_TEXTURE_2D);
  glDisable(GL_BLEND);
  glColor4f(1, 1, 1, 1);
  EnterCriticalSection(&cs_gdi);
  float x = (float)dst_x, y = (float)dst_y, w = (float)dst_w, h = (float)dst_h;
  int side = is_full();
  LeaveCriticalSection(&cs_gdi);
  if (side)
  { //top screen on the left, touch on the right
    gl_rect(x, y, w / 2, h, 0, 0, 1, 0.5f);
    gl_rect(x + w / 2, y, w / 2, h, 0, 0.5f, 1, 1);
  }
  else
  {
    gl_rect(x, y, w, h, 0, 0, 1, 1);
  }
  toast_draw_gl(r.right, r.bottom);
  SwapBuffers(dc); //waits for the monitor refresh
}

static DWORD WINAPI paint_thread(LPVOID p)
{
  (void)p;
  HDC dc = GetDC(window);
  int gl = gl_init(dc);
  int uploaded = -1;
  int64_t ts_uploaded = 0;
  if (gl)
    InterlockedExchange(&gl_active, 1);
  if (gl)
    log_msg("window", "OpenGL %s on %s", (const char*)glGetString(GL_VERSION),
            (const char*)glGetString(GL_RENDERER));
  else
    log_msg("window", "OpenGL with vsync is not available: using the slower GDI drawing");
  sysinfo_log_display(window);
  for (;;)
  {
    if (gl && gl_closing)
    { //close requested: let go of the context from this thread
      glFinish();
      wglMakeCurrent(NULL, NULL);
      wglDeleteContext(gl_rc);
      ReleaseDC(window, dc);
      SetEvent(ev_gl_done);
      return 0;
    }
    if (!gl)
      WaitForSingleObject(ev_paint, INFINITE);
    int i = pick_frame();
    if (i < 0)
    {
      if (gl)
        Sleep(1);
      continue;
    }
    if (gl)
    {
      int new = i != uploaded || frame_ts[i] != ts_uploaded;
      LARGE_INTEGER present_start, present_end;
      QueryPerformanceCounter(&present_start);
      gl_paint(dc, i, new);
      QueryPerformanceCounter(&present_end);
      log_note_present(present_end.QuadPart - present_start.QuadPart);
      if (new)
        InterlockedIncrement(&pictures_shown);
      uploaded = i;
      ts_uploaded = frame_ts[i];
    }
    else
    {
      image = frames[i].low;
      image_hd = frames[i].hd;
      EnterCriticalSection(&cs_gdi);
      draw_en(dc);
      LeaveCriticalSection(&cs_gdi);
    }
  }
}

//the window lives on its own thread. While the user drags the title bar Windows runs a loop inside
//the thread that owns the window. If that were the game thread, the game and its sound would stop
//until the drop
static HANDLE ev_window_up;
static DWORD WINAPI window_thread(LPVOID p)
{
  (void)p;
  WNDCLASSW wc = {0};
  wc.lpfnWndProc = proc;
  wc.hInstance = GetModuleHandle(NULL);
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.lpszClassName = L"driftds";
  RegisterClassW(&wc);
  RECT r = {0, 0, 256 * SCALE, 384 * SCALE};
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  AdjustWindowRect(&r, style, FALSE);
  window = CreateWindowExW(0, L"driftds", L"DriftDS", style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
  ShowWindow(window, SW_SHOW);
  //WINDOW_MODE in settings.ini picks the start-up mode. The old FULLSCREEN=1 still works: it
  //means borderless fullscreen, what the game did before there were three modes
  const char* size_text = getenv("DRIFT_FULLSCREEN_SIZE");
  if (size_text && *size_text && !winmode_parse_size(size_text, &want_full_w, &want_full_h))
    log_msg("window", "FULLSCREEN_SIZE \"%s\" is not like 1280x720: using the desktop size",
            size_text);
  const char* old_flag = getenv("DRIFT_FULLSCREEN");
  int start_mode = winmode_parse(getenv("DRIFT_WINDOW_MODE"), old_flag ? atoi(old_flag) : 0);
  if (start_mode != WINMODE_WINDOWED)
  {
    set_mode(start_mode, 1);
  }
  else
  {
    log_msg("window", "mode windowed (start-up)");
    log_set_mode(winmode_name(WINMODE_WINDOWED));
  }
  SetEvent(ev_window_up);
  MSG m;
  while (GetMessageW(&m, NULL, 0, 0) > 0)
  {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  return 0;
}

void window_init(void)
{
  build_key_table(); //bindings load from the environment on first use
  update_keyboard();
  InitializeCriticalSection(&cs_frames);
  InitializeCriticalSection(&cs_gdi);
  ev_paint = CreateEventW(NULL, FALSE, FALSE, NULL);
  ev_gl_done = CreateEventW(NULL, TRUE, FALSE, NULL);
  ev_window_up = CreateEventW(NULL, FALSE, FALSE, NULL);
  CloseHandle(CreateThread(NULL, 0, window_thread, NULL, 0, NULL));
  WaitForSingleObject(ev_window_up, INFINITE);
  bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
  bmi.bmiHeader.biWidth = 256;
  bmi.bmiHeader.biHeight = -384; //top to bottom
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  bmi_side = bmi;
  bmi_side.bmiHeader.biWidth = 512;
  bmi_side.bmiHeader.biHeight = -192;
  int e = video_scale();
  bmi_hd = bmi;
  bmi_hd.bmiHeader.biWidth = 256 * e;
  bmi_hd.bmiHeader.biHeight = -384 * e;
  bmi_side_hd = bmi;
  bmi_side_hd.bmiHeader.biWidth = 512 * e;
  bmi_side_hd.bmiHeader.biHeight = -192 * e;
  HANDLE h = CreateThread(NULL, 0, paint_thread, NULL, 0, NULL);
  SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL); //the game (and its music) first
  CloseHandle(h);
}

//a free image of the queue (never the one the painter is showing)
static Frame* reserve(void)
{
  EnterCriticalSection(&cs_frames);
  int i = i_write;
  if (i == i_viewing)
    i = (i + 1) % NM;
  i_write = (i + 1) % NM;
  frame_ts[i] = 0;
  i_reserved = i;
  LeaveCriticalSection(&cs_frames);
  return &frames[i];
}

static void publish(int64_t ts)
{
  EnterCriticalSection(&cs_frames);
  frame_ts[i_reserved] = ts ? ts : 1;
  LeaveCriticalSection(&cs_frames);
  SetEvent(ev_paint);
}

int64_t window_clock(void)
{
  return clock();
}

void window_show(const uint32_t* up, const uint32_t* bottom)
{
  window_show_en(up, bottom, 0);
}

//a game image. ts = when to show it (0 = now)
void window_show_en(const uint32_t* up, const uint32_t* bottom, int64_t ts)
{
  Frame* m = reserve();
  memcpy(m->low, up, 256 * 192 * 4);
  memcpy(m->low + 256 * 192, bottom, 256 * 192 * 4);
  int e = video_scale();
  if (e > 1)
  {
    size_t n = (size_t)256 * e * 192 * e;
    memcpy(m->hd, video_hd(0), n * 4);
    memcpy(m->hd + n, video_hd(1), n * 4);
  }
  last = m;
  publish(ts ? ts : clock());
}

//Gamepad: added to what is pressed on the keyboard
static void read_pad(uint16_t* in, uint16_t* xy, int* combo)
{
  *in = 0x3FF;
  *xy = 3;
  *combo = 0;
  XINPUT_STATE st;
  for (DWORD i = 0; i < 4; i++)
  {
    if (XInputGetState(i, &st) != ERROR_SUCCESS)
      continue;
    WORD b = st.Gamepad.wButtons;
    SHORT lx = st.Gamepad.sThumbLX, ly = st.Gamepad.sThumbLY;
    const SHORT zone = 12000;
    struct
    {
      int is_down, bit;
    } m[] = {
        {b & XINPUT_GAMEPAD_A, 0},
        {b & XINPUT_GAMEPAD_B, 1},
        {b & XINPUT_GAMEPAD_BACK, 2},
        {b & XINPUT_GAMEPAD_START, 3},
        {(b & XINPUT_GAMEPAD_DPAD_RIGHT) || lx > zone, 4},
        {(b & XINPUT_GAMEPAD_DPAD_LEFT) || lx < -zone, 5},
        {(b & XINPUT_GAMEPAD_DPAD_UP) || ly > zone, 6},
        {(b & XINPUT_GAMEPAD_DPAD_DOWN) || ly < -zone, 7},
        {(b & XINPUT_GAMEPAD_RIGHT_SHOULDER) || st.Gamepad.bRightTrigger > 60, 8},
        {(b & XINPUT_GAMEPAD_LEFT_SHOULDER) || st.Gamepad.bLeftTrigger > 60, 9},
    };
    for (int k = 0; k < 10; k++)
      if (m[k].is_down)
        *in &= (uint16_t)~(1u << m[k].bit);
    if (b & XINPUT_GAMEPAD_Y)
      *xy &= ~1; //by position, like the DS (and like the PlayStation ones): up = X
    if (b & XINPUT_GAMEPAD_X)
      *xy &= ~2; //left = Y
    if ((b & XINPUT_GAMEPAD_LEFT_THUMB) && (b & XINPUT_GAMEPAD_RIGHT_THUMB))
      *combo = 1;
  }
}

void window_messages(void)
{
  uint16_t pad_in = 0x3FF, pad_xy = 3;
  int combo = 0;
  read_pad(&pad_in, &pad_xy, &combo);
  g_keyinput = keyboard_in & pad_in;
  g_keyxy = (uint16_t)(keyboard_xy & pad_xy);
}

//FPS counter in the window title
int window_title_fps(int fps, int fps3d)
{
  int screen = (int)InterlockedExchange(&pictures_shown, 0);
  if (!window)
    return screen;
  wchar_t t[128];
  swprintf(t, 128, L"DriftDS  -  %d FPS  (3D: %d)  screen: %d", fps, fps3d, screen);
  SetWindowTextW(window, t);
  return screen;
}
