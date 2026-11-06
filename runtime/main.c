// Entry point of DriftDS: boots the translated Mario Kart DS code.
// Leaves memory as the DS would on power on (code of both processors, ROM header) and starts them.
// The hardware is in ds.c.
#include "compat.h" // before cpu.h: its R macro clashes with windows.h
#include "ds.h"
#include "region.h"
#include "inifile.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Addresses per region (see region.h).  EUR: the original port.  USA: mapped with
// tools/map_region.py.
static const Region REGION_EUR = {
    .label = "EUR",
    .cp_save = 0x02146C24,
    .cp_restore = 0x02146BB8,
    .reschedule = 0x0200DE50,
    .flag_resched = 0x0216FF5C,
    .current_thread = 0x0216FF54,
    .obj_mission = 0x021CA410,
    .obj_boss = 0x021CA414,
    .save = 0x023CE2C8,
    .mic = {0x0217328C, 0x0217329C, 0x021732A4, 0x021732A8, 0x02173288, 0x021732A0, 0x02161AD8},
    .dwc = 0x021B566C,
};
static const Region REGION_USA = {
    .label = "USA",
    .cp_save = 0x02146C24,
    .cp_restore = 0x02146BB8,
    .reschedule = 0x0200DE50,
    .flag_resched = 0x0216FF3C,
    .current_thread = 0x0216FF34,
    .obj_mission = 0x021CA3F0,
    .obj_boss = 0x021CA3F4,
    .save = 0, // a heap address: found at run time (name_per_frame)
    .mic = {0x0217326C, 0x0217327C, 0x02173284, 0x02173288, 0x02173268, 0x02173280, 0x02161AC8},
    .dwc = 0x021B562C,
};
const Region* g_region = &REGION_EUR;

static void dump(FILE* o, Cpu* c)
{
  fprintf(o, "  %s in function %08X (%s)\n", c->id ? "ARM7" : "ARM9", c->func & ~1u,
          (c->func & 1) ? "Thumb" : "ARM");
  for (int i = 0; i < 16; i++)
    fprintf(o, "    r%-2d %08X%s", i, c->r[i], i % 4 == 3 ? "\n" : "");
  if (c->id)
  {
    fprintf(o, "    CPSR %08X  stack:", cpu_get_cpsr(c));
    for (int i = 0; i < 24; i++)
      fprintf(o, " %08X", rd32_7(c->r[13] + 4 * i));
    fprintf(o, "\n");
  }
  if (!c->id)
  {
    extern void cpu_dump_jumps(FILE * o);
    cpu_dump_jumps(o);
    // current thread of the SDK (OSi_CurrentThread) and its priority
    uint32_t t = rd32_9(rd32_9(g_region->current_thread));
    fprintf(o, "    current game thread: %08X\n", t);
    fprintf(o, "    CPSR %08X  stack:", cpu_get_cpsr(c));
    for (int i = 0; i < 24; i++)
      fprintf(o, " %08X", rd32_9(c->r[13] + 4 * i));
    fprintf(o, "\n");
  }
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS* e)
{
  DWORD cod = e->ExceptionRecord->ExceptionCode;
  if (cod != EXCEPTION_ACCESS_VIOLATION && cod != EXCEPTION_STACK_OVERFLOW &&
      cod != EXCEPTION_ILLEGAL_INSTRUCTION && cod != EXCEPTION_INT_DIVIDE_BY_ZERO)
    return EXCEPTION_CONTINUE_SEARCH;
  // in which module (the game exe, the graphics card driver...) and from where it got there
  void* dir = e->ExceptionRecord->ExceptionAddress;
  HMODULE mod = NULL;
  char label[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCSTR)dir, &mod))
    GetModuleFileNameA(mod, label, MAX_PATH);
  char txt[512];
  int k = snprintf(txt, sizeof txt, "[game] fault %08lX at PC (%s+%llX)\n",
                   e->ExceptionRecord->ExceptionCode,
                   strrchr(label, '\\') ? strrchr(label, '\\') + 1 : label,
                   (unsigned long long)((char*)dir - (char*)mod));
#ifdef _M_X64
  { // the stack of the thread that failed: return addresses that fall inside the game exe
    char *base = (char*)GetModuleHandle(NULL), **stack = (char**)e->ContextRecord->Rsp;
    k += snprintf(txt + k, sizeof txt - (size_t)k, "    calls in the game exe:");
    for (int i = 0, n = 0; i < 4096 && n < 12 && k < (int)sizeof txt - 24; i++)
      if (stack[i] > base && stack[i] < base + 0x8000000)
      {
        k += snprintf(txt + k, sizeof txt - (size_t)k, " +%llX",
                      (unsigned long long)(stack[i] - base));
        n++;
      }
    snprintf(txt + k, sizeof txt - (size_t)k, "\n");
  }
#endif
  FILE* lf = log_crash_begin("fault in the game program");
  fprintf(stderr, "\n%s", txt);
  if (lf)
    fprintf(lf, "ARM9 at %08X  ARM7 at %08X\n%s\n", g_cpu9.r[15], g_cpu7.r[15], txt);
  dump(stderr, &g_cpu9);
  dump(stderr, &g_cpu7);
  if (lf)
  {
    dump(lf, &g_cpu9);
    dump(lf, &g_cpu7);
    fflush(lf);
  }
  fflush(stderr);
  ExitProcess(9);
}

static void* load(const char* file_path, size_t* n)
{
  FILE* f = fopen(file_path, "rb");
  if (!f)
  {
    perror(file_path);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  *n = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  void* p = malloc(*n);
  fread(p, 1, *n, f);
  fclose(f);
  return p;
}

// Player name (NAME in settings.ini). The loaded save is at 0x023CE2C8
//(starts with "NKSY") and the name goes after it, in UTF-16, up to 10 letters. The game copies it
// from the firmware on creating the save. The user's was created with an empty name and the game
// does not let it be changed. It is written on every frame if it does not match. On saving, the
// game already leaves it in the save.
static int name_ready;

void name_per_frame(void)
{
  static uint16_t label[10];
  int ready = name_ready;
  if (!ready)
  {
    ready = 1;
    const char* n = getenv("DRIFT_NAME");
    wchar_t w[11] = {0};
    if (n && *n)
      MultiByteToWideChar(CP_UTF8, 0, n, -1, w, 11);
    w[10] = 0;
    for (int i = 0; i < 10; i++)
      label[i] = (uint16_t)w[i];
    if (!label[0])
      ready = 2; // without a name: nothing is touched
    name_ready = ready;
  }
  if (ready == 2)
    return;
  static uint32_t
      dir; // where the save is: the one of the region, or the one found by searching (USA)
  if (!dir)
  {
    dir = g_region->save;
    if (!dir)
    {
      // The save is in the heap, after the code and the overlays (the text "NKSY" is also in the
      // code: it is only looked for above 0x02200000). It is searched every half second until it
      // appears.
      static unsigned frames;
      if (frames++ % 30)
        return;
      for (uint32_t a = 0x02200000; a + 0x20 <= 0x023FE000; a += 4)
        if (!memcmp(g_main + (a & 0x3FFFFF), "NKSY", 4))
        {
          dir = a;
          break;
        }
      if (!dir)
        return;
    }
  }
  uint8_t* p = g_main + (dir & 0x3FFFFF);
  if (memcmp(p, "NKSY", 4))
    return; // the save is not loaded yet
  if (memcmp(p + 4, label, sizeof label))
    memcpy(p + 4, label, sizeof label);
}

// settings.ini (written by the launcher): lines KEY=value that become the usual variables
//(SCALE -> DRIFT_SCALE, FULLSCREEN -> DRIFT_FULLSCREEN, NAME -> DRIFT_NAME).
// A line that starts with # is a comment, and so is the end of a line after " #" (see inifile.h).
// What is already set in the environment wins.
static void read_settings(void)
{
  FILE* f = fopen("settings.ini", "r");
  if (!f)
    return;
  char line[256];
  while (fgets(line, sizeof line, f))
  {
    line[strcspn(line, "\r\n")] = 0;
    char* same = strchr(line, '=');
    if (!same || line[0] == '#' || line[0] == ';' || line[0] == '[')
      continue;
    *same = 0;
    char var[64];
    snprintf(var, sizeof var, "DRIFT_%s", line);
    ini_clean_value(same + 1); // a note after the value (" # ...") is not part of it
    if (!getenv(var))
      _putenv_s(var, same + 1);
  }
  fclose(f);
}

//(so choosing the ROM in the launcher is enough. Before, extracted/ from tools/rom_reader.py was
// needed)
static uint32_t le32(const uint8_t* p)
{
  return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

//"Backwards LZ" of the SDK (MI_UncompressBackward), same as blz_decompress in tools/rom_reader.py:
// the last 8 bytes say how much it grows and which part is compressed. It is read from the end to
// the start.
static uint8_t* blz(const uint8_t* buf, size_t len, size_t* outbuf)
{
  uint32_t grows = le32(buf + len - 4);
  uint8_t* out = malloc(len + grows);
  memcpy(out, buf, len);
  *outbuf = len + grows;
  if (!grows)
    return out;
  uint32_t hdr = le32(buf + len - 8);
  size_t hdr_len = hdr >> 24, cod_len = hdr & 0xFFFFFF;
  size_t raw = len - cod_len; // what comes before goes uncompressed
  size_t src = len - hdr_len, dst = len + grows;
  while (dst > raw && src > raw)
  {
    uint8_t flags = buf[--src];
    for (int k = 0; k < 8 && dst > raw && src > raw; k++, flags <<= 1)
    {
      if (flags & 0x80)
      {
        uint8_t b1 = buf[src - 1], b2 = buf[src - 2];
        src -= 2;
        int span = (b1 >> 4) + 3, shift = (((b1 & 15) << 8) | b2) + 3;
        for (int i = 0; i < span && dst > 0; i++, dst--)
          out[dst - 1] = out[dst - 1 + shift];
      }
      else
      {
        out[--dst] = buf[--src];
      }
    }
  }
  return out;
}

static void load_code(const uint8_t* rom, size_t n)
{
  uint32_t a9o = le32(rom + 0x20), a9ram = le32(rom + 0x28), a9t = le32(rom + 0x2C);
  uint32_t a7o = le32(rom + 0x30), a7ram = le32(rom + 0x38), a7t = le32(rom + 0x3C);
  if ((size_t)a9o + a9t > n || (size_t)a7o + a7t > n)
  {
    fprintf(stderr, "incomplete ROM\n");
    exit(1);
  }
  const uint8_t* raw = rom + a9o;
  uint8_t* arm9 = malloc(a9t);
  memcpy(arm9, raw, a9t);
  size_t t9 = a9t;
  static const uint8_t magic[8] = {0x21, 0x06, 0xC0, 0xDE, 0xDE, 0xC0, 0x06, 0x21};
  for (uint32_t i = 0x1C; i + 8 <= a9t; i += 4)
  {
    if (memcmp(raw + i, magic, 8))
      continue;
    uint32_t mp = i - 0x1C, fin = le32(raw + mp + 0x14); // compressed_static_end
    if (fin)
    {
      size_t cut = fin - a9ram, td;
      uint8_t* dec = blz(raw, cut, &td);
      free(arm9);
      t9 = td + (a9t - cut);
      arm9 = malloc(t9);
      memcpy(arm9, dec, td);
      memcpy(arm9 + td, raw + cut, a9t - cut);
      free(dec);
      memset(arm9 + mp + 0x14, 0, 4); // already decompressed: so the boot does not try it again
    }
    break;
  }
  memcpy(g_main + (a9ram & 0x3FFFFF), arm9, t9);
  memcpy(g_main + (a7ram & 0x3FFFFF), rom + a7o, a7t);
  free(arm9);
}

// The game folder holds rom/, settings.ini and save.sav, one level above the build folder.
// A double click on the exe inside the build folder starts Windows there, and the ROM is not found.
static void go_to_game_folder(void)
{
  wchar_t file_path[MAX_PATH];
  DWORD n = GetModuleFileNameW(NULL, file_path, MAX_PATH);
  if (!n || n >= MAX_PATH)
    return;
  wchar_t* slash = wcsrchr(file_path, L'\\');
  if (!slash)
    return;
  *slash = 0; // folder of the .exe
  slash = wcsrchr(file_path, L'\\');
  if (slash && !_wcsicmp(slash + 1, L"build"))
    *slash = 0;
  SetCurrentDirectoryW(file_path);
}

// NO_POWER_THROTTLE=1 in settings.ini: tell Windows not to slow this program down on its own.
// Windows can move a program to slow power-saving cores ("efficiency mode") and, since Windows 11,
// can ignore the 1 ms timer the game asks for. Both can cause short pauses. Only this process is
// affected, no admin rights are needed, and it ends when the game closes. Default 0: do nothing.
static void apply_power_setting(void)
{
  const char* want = getenv("DRIFT_NO_POWER_THROTTLE");
  if (!want || atoi(want) != 1)
  {
    log_msg("power", "power throttling left to Windows");
    return;
  }
  PROCESS_POWER_THROTTLING_STATE state = {0};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask =
      PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  state.StateMask = 0; // control these two, and ask for them to be off
  // looked up at run time: the build targets an older Windows version, and an older Windows
  // without this call then just gets the error line below
  typedef BOOL(WINAPI * SetProcessInformationFn)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
  SetProcessInformationFn set_info = (SetProcessInformationFn)(void*)GetProcAddress(
      GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
  if (!set_info)
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
  if (set_info && set_info(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof state))
    log_msg("power", "power throttling switched off for this program");
  else
    log_msg("power", "could not switch off power throttling (error %lu)", GetLastError());
}

static int boot(void)
{
  go_to_game_folder();
  read_settings();
  log_init();
  apply_power_setting();
  size_t n;
  FILE* has = fopen("rom/game.nds", "rb");
  if (!has)
  {
    MessageBoxW(
        NULL, L"Game not found (rom\\game.nds).\nPut your Mario Kart DS ROM there and start again.",
        L"DriftDS", MB_ICONWARNING);
    return 1;
  }
  fclose(has);
  uint8_t* rom = load("rom/game.nds", &n);
  // The region (the game code is in the ROM header): European (AMCP) or American (AMCE)
  if (n < 0x200 || (memcmp(rom + 12, "AMCP", 4) && memcmp(rom + 12, "AMCE", 4)))
  {
    fprintf(stderr, "rom/game.nds is not European (AMCP) or American (AMCE) Mario Kart DS\n");
    return 1;
  }
  g_region = memcmp(rom + 12, "AMCE", 4) ? &REGION_EUR : &REGION_USA;
  log_msg("rom", "%s (%s), %zu KB", g_region == &REGION_USA ? "AMCE" : "AMCP", g_region->label,
          n >> 10);
  load_code(rom, n); // ARM9 en 0x02000000, ARM7 en 0x02380000
  memcpy(g_main + 0x3FFE00, rom, 0x170); // the header ends up at 0x027FFE00
  ds_set_rom(rom, n);

  // Operating system of the game (NitroSDK): addresses taken from the disassembly
  g_so[0] = (SoConfig){
      .cp_save = g_region->cp_save,
      .cp_restore = g_region->cp_restore,
      .reschedule = g_region->reschedule,
      .queue_irq = 0x027E0000,
      .flag_resched = g_region->flag_resched,
      .off_state = 0x64,
      .off_link = 0x78,
      .off_sig = 0x80,
  };
  g_so[1] = (SoConfig){
      .reschedule = 0x037F8F48,
      .queue_irq = 0x0380766C,
      .flag_resched = 0x038076FC,
      .off_state = 0x48,
      .off_link = 0x5C,
      .off_sig = 0x64,
  };

  AddVectoredExceptionHandler(1, on_crash);
  ds_start(0x02000800, 0x02380000);
  return 0;
}

int main(void)
{
  return boot();
} // the 256 MB stack is set by the linker (/STACK)
