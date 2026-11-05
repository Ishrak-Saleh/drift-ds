// Machine details for the log: see sysinfo.h.
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
  while (*name == ' ') // the registry value often has leading spaces
    name++;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  log_msg("system", "%s, %lu logical cores", name, (unsigned long)si.dwNumberOfProcessors);

  // GetVersionEx lies on current Windows; RtlGetVersion tells the truth
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

  // On battery or in battery saver Windows slows the CPU: the first thing to rule out for stutter.
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
  // A 60 Hz monitor cannot show more than 60 pictures a second, and a window on a second monitor
  // can have a different refresh rate than the main one.
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
  uint64_t total = filetime_value(kernel) + filetime_value(user); // kernel time includes idle
  uint64_t idle_time = filetime_value(idle);
  int percent = -1;
  if (total_before && total > total_before)
    percent = (int)(100 - (idle_time - idle_before) * 100 / (total - total_before));
  total_before = total;
  idle_before = idle_time;
  return percent;
}
