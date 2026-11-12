//Machine details for the log: see sysinfo.h.
#include "compat.h"
#include "sysinfo.h"
#include "log.h"
#include <string.h>

void sysinfo_log_machine(void)
{
  char cpu[128] = "unknown CPU";
  DWORD size = sizeof cpu;
  RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
               "ProcessorNameString", RRF_RT_REG_SZ, NULL, cpu, &size);
  const char* name = cpu;
  while (*name == ' ') //the registry value often has leading spaces
    name++;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  log_msg("system", "%s, %lu logical cores", name, (unsigned long)si.dwNumberOfProcessors);

  //GetVersionEx lies on current Windows; RtlGetVersion tells the truth
  typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
  OSVERSIONINFOW os;
  memset(&os, 0, sizeof os);
  os.dwOSVersionInfoSize = sizeof os;
  RtlGetVersionFn rtl =
      (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
  MEMORYSTATUSEX mem;
  memset(&mem, 0, sizeof mem);
  mem.dwLength = sizeof mem;
  GlobalMemoryStatusEx(&mem);
  if (rtl && rtl(&os) == 0)
    log_msg("system", "Windows %s (build %lu), RAM %llu MB (%llu MB free)",
            os.dwBuildNumber >= 22000 ? "11" : "10", (unsigned long)os.dwBuildNumber,
            (unsigned long long)(mem.ullTotalPhys >> 20),
            (unsigned long long)(mem.ullAvailPhys >> 20));

  //on battery or in battery saver Windows slows the CPU: the first thing to rule out for stutter.
  SYSTEM_POWER_STATUS power;
  if (GetSystemPowerStatus(&power))
  {
    if (power.ACLineStatus == 1)
      log_msg("power", "plugged in");
    else if (power.ACLineStatus == 0)
      log_msg("power", "ON BATTERY (%d%%)%s: Windows may slow the CPU",
              power.BatteryLifePercent == 255 ? -1 : power.BatteryLifePercent,
              (power.SystemStatusFlag & 1) ? ", battery saver is ON" : "");
  }
}

void sysinfo_log_display(void* hwnd)
{
  //a 60 Hz monitor cannot show more than 60 pictures a second, and a window on a second monitor
  //can have a different refresh rate than the main one.
  MONITORINFOEXW mi;
  memset(&mi, 0, sizeof mi);
  mi.cbSize = sizeof mi;
  DEVMODEW dm;
  memset(&dm, 0, sizeof dm);
  dm.dmSize = sizeof dm;
  if (GetMonitorInfoW(MonitorFromWindow((HWND)hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO*)&mi) &&
      EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
    log_msg("display", "%lux%lu at %lu Hz (the window's monitor), %d monitor(s) connected",
            (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight,
            (unsigned long)dm.dmDisplayFrequency, GetSystemMetrics(SM_CMONITORS));
}

int sysinfo_pick_mode(void* hwnd, int want_w, int want_h, int* width, int* height, int* hertz,
                      wchar_t device[32], int* desktop_w, int* desktop_h, int* desktop_hz)
{
  MONITORINFOEXW mi;
  memset(&mi, 0, sizeof mi);
  mi.cbSize = sizeof mi;
  DEVMODEW now;
  memset(&now, 0, sizeof now);
  now.dmSize = sizeof now;
  if (!GetMonitorInfoW(MonitorFromWindow((HWND)hwnd, MONITOR_DEFAULTTONEAREST),
                       (MONITORINFO*)&mi) ||
      !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &now))
    return 0;
  *desktop_w = (int)now.dmPelsWidth;
  *desktop_h = (int)now.dmPelsHeight;
  *desktop_hz = (int)now.dmDisplayFrequency;
  wcsncpy(device, mi.szDevice, 31);
  device[31] = 0;

  int w = want_w > 0 ? want_w : *desktop_w, h = want_h > 0 ? want_h : *desktop_h;
  //Score of a refresh rate for this size: the desktop's own rate wins, then the highest below it,
  //then the highest above it. 0 and 1 Hz mean "hardware default" and are skipped
  long best_score = -1;
  int best_hz = 0;
  for (DWORD i = 0;; i++)
  {
    DEVMODEW m;
    memset(&m, 0, sizeof m);
    m.dmSize = sizeof m;
    if (!EnumDisplaySettingsW(mi.szDevice, i, &m))
      break;
    int hz = (int)m.dmDisplayFrequency;
    if ((int)m.dmPelsWidth != w || (int)m.dmPelsHeight != h || m.dmBitsPerPel != now.dmBitsPerPel ||
        (m.dmDisplayFlags & DM_INTERLACED) || hz <= 1)
      continue;
    long score = hz == *desktop_hz ? 1000000 : hz < *desktop_hz ? 100000 + hz : 10000 + hz;
    if (score > best_score)
    {
      best_score = score;
      best_hz = hz;
    }
  }
  if (best_score < 0)
  {
    if (w != *desktop_w || h != *desktop_h)
      return 0; //the size is not offered by this monitor
    best_hz = *desktop_hz; //the desktop's own mode is always usable
  }
  *width = w;
  *height = h;
  *hertz = best_hz;
  return 1;
}

int sysinfo_free_edge(int left, int top, int right, int bottom)
{
  int desk_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  int desk_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int desk_right = desk_left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  int desk_bottom = desk_top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (bottom >= desk_bottom)
    return 0;
  if (right >= desk_right)
    return 1;
  if (top <= desk_top)
    return 2;
  if (left <= desk_left)
    return 3;
  return -1;
}

static uint64_t filetime_value(FILETIME ft)
{
  return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

int sysinfo_pc_cpu_percent(void)
{
  static uint64_t total_before, idle_before;
  FILETIME idle, kernel, user;
  if (!GetSystemTimes(&idle, &kernel, &user))
    return -1;
  uint64_t total = filetime_value(kernel) + filetime_value(user); //kernel time includes idle
  uint64_t idle_time = filetime_value(idle);
  int percent = -1;
  if (total_before && total > total_before)
    percent = (int)(100 - (idle_time - idle_before) * 100 / (total - total_before));
  total_before = total;
  idle_before = idle_time;
  return percent;
}
