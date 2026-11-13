//runs the game's own blow check (A_0201EC30, Balloon Battle) on microphone signals, so the signal
//of the blow key can be tested without playing. where it fits: microphone.c makes the blow of the
//MIC_KEY key (blow_wave). The game does not just look for loud sound, it measures the length of the
//waves, so the signal has to be tested against the game's own code. what it does: loads
//extracted/arm9.bin into the game memory, sets up the game's microphone variables (1024 byte
//buffer, as A_0210F130 does) and calls the translated check. Usage:  tests/test_blow.exe checks
//blow_wave from microphone.c (exit code 1 on failure)
//        tests/test_blow.exe search   random search for another wave
//Needs:  the game objects, so run "sh build.sh compile" first, then (from the project folder):
//  find build/obj_gen build/obj_rt -name '*.o' > build/blow_list.txt
//  clang -O1 -w -Iruntime tests/test_blow.c @build/blow_list.txt -lwinmm -lgdi32 -lopengl32
//  -luser32
//        -lxinput -Wl,--allow-multiple-definition -Wl,--stack,268435456 -o tests/test_blow.exe
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/cpu.h"

void A_0201EC30(Cpu* c);
int16_t blow_wave(int i); //microphone.c
int blow_samples(void);
int blow_max(void);

#define BUF 0x02210000u
#define S_ADDR 0x02200000u
#define T_ADDR 0x02200100u
#define MODE_ADDR 0x02161AD8u
#define N 1024 //the buffer of the game (A_0210F130 writes 0x400)

static void w32(uint32_t a, uint32_t v)
{
  memcpy(g_main + (a & 0x3FFFFF), &v, 4);
}

static void load_arm9(void)
{
  FILE* f = fopen("extracted/arm9.bin", "rb");
  if (!f)
  {
    puts("extracted/arm9.bin missing (run from the project folder)");
    exit(2);
  }
  fread(g_main, 1, 0x3FFFFF, f);
  fclose(f);
}

//the level the game gives for a buffer whose newest sample is the last one. 0 = no blow
static int check(const int8_t* samples, int mode)
{
  memcpy(g_main + (BUF & 0x3FFFFF), samples, N);
  w32(0x02173270, S_ADDR);
  w32(0x02173274, T_ADDR);
  w32(T_ADDR, 1);
  w32(S_ADDR, 1);
  w32(S_ADDR + 4, BUF);
  w32(S_ADDR + 8, N);
  w32(0x027FFF90, BUF + N); //the position after the newest sample
  g_main[MODE_ADDR & 0x3FFFFF] = (uint8_t)mode;
  Cpu c;
  memset(&c, 0, sizeof c);
  c.cycles = 1 << 30;
  c.r[13] = 0x02300000;
  A_0201EC30(&c);
  return (int)c.r[0];
}

static int8_t sig[N];

static int failures;
static void expect(int ok, const char* what)
{
  if (!ok)
  {
    printf("FAIL: %s\n", what);
    failures++;
  }
}

//a wave that repeats exactly every base samples: three sine waves at whole multiples of 1/base, so
//every window of the press looks like one of base shifted copies. Searches the base, the three
//multiples, the loudness and the weights
static void search(void)
{
  srand(3);
  int best = 0;
  for (int tries = 0; tries < 20000; tries++)
  {
    int base = 40 + rand() % 360;
    int h[3] = {1 + rand() % 6, 1 + rand() % 14, 1 + rand() % 24};
    double amp[3] = {1, (rand() % 100) / 100.0, (rand() % 100) / 100.0};
    double gain = 2 + rand() % 6;
    int score = 0, cells = 0;
    for (int off = 0; off < base; off += 3)
    {
      for (int i = 0; i < N; i++)
      {
        double v = 0;
        for (int k = 0; k < 3; k++)
          v += amp[k] * sin(6.2831853 * h[k] * (i + off) / base + k);
        v *= gain * 40;
        sig[i] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : (int)v);
      }
      for (int mode = 0; mode < 3; mode++, cells++)
        score += check(sig, mode) > 0;
    }
    if (score * 1000 / cells > best)
    {
      best = score * 1000 / cells;
      printf("%d.%d percent: base %d multiples %d %d %d amps 1 %.2f %.2f gain %.0f\n", best / 10,
             best % 10, base, h[0], h[1], h[2], amp[1], amp[2], gain);
    }
  }
}

static void bursts(void)
{
  //how long must one press be? for a blow of len samples (silence before and after), the number of
  //moments, counted every 4 samples while the buffer slides over it, at which the game says blow
  for (int len = 512; len <= 3000; len += 256)
  {
    int taken = 0, total = 0, first = -1, last = -1;
    for (int end = 1; end <= len + N; end += 4, total++)
    {
      for (int i = 0; i < N; i++)
      {
        int at = end - N + i;
        sig[i] = (at < 0 || at >= len) ? 0 : (int8_t)(blow_wave(at) >> 8);
      }
      int hit = 0;
      for (int mode = 0; mode < 3; mode++)
        hit += check(sig, mode) > 0;
      if (hit == 3)
      {
        taken++;
        if (first < 0)
          first = end;
        last = end;
      }
    }
    printf("blow of %4d samples (%3d ms): accepted at %3d moments (%d ms), from sample %d to %d\n",
           len, len / 8, taken, taken * 4 / 8, first, last);
  }
}

int main(int argc, char** argv)
{
  load_arm9();
  if (argc > 1 && !strcmp(argv[1], "bursts"))
  {
    bursts();
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "search"))
  {
    search();
    return 0;
  }

  //silence and the old noise of the key are not a blow
  memset(sig, 0, N);
  for (int mode = 0; mode < 3; mode++)
    expect(check(sig, mode) == 0, "silence is not a blow");
  uint32_t x = 12345;
  for (int i = 0; i < N; i++)
  {
    x = x * 1664525u + 1013904223u;
    int v = ((int)(x >> 24) - 128) * 2;
    sig[i] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
  }
  for (int mode = 0; mode < 3; mode++)
    expect(check(sig, mode) == 0, "white noise (the first blow of the key) is not a blow");

  //a wave that fills the whole buffer is a blow at every one of its 313 positions
  int cells = 0, taken = 0;
  for (int mode = 0; mode < 3; mode++)
    for (int off = 0; off < 313; off++, cells++)
    {
      for (int i = 0; i < N; i++)
        sig[i] = (int8_t)(blow_wave(off + i) >> 8);
      taken += check(sig, mode) > 0;
    }
  printf("full buffer: %d of %d positions are a blow\n", taken, cells);
  expect(taken * 100 >= cells * 99,
         "a full buffer of blow_wave is a blow (99 percent of positions)");

  //one key press (silence before and after): the game must notice it for a while
  int len = blow_samples(), moments = 0;
  for (int end = 1; end <= len + N; end += 4)
  {
    for (int i = 0; i < N; i++)
    {
      int at = end - N + i;
      sig[i] = (at < 0 || at >= len) ? 0 : (int8_t)(blow_wave(at) >> 8);
    }
    int all = 1;
    for (int mode = 0; mode < 3; mode++)
      all &= check(sig, mode) > 0;
    moments += all;
  }
  printf("one key press of %d samples: noticed at %d moments (%d ms, counted every 4 samples)\n",
         len, moments, moments * 4 / 8);
  expect(moments * 4 >= 400, "one key press is noticed for at least 50 ms (400 samples)");

  //mashing 4 times a second (a press every 2000 samples, each adds blow_samples() to what is left,
  //at most blow_max()): once the game has noticed the first blow, every later moment while the
  //mashing goes on must be a blow. the timeline is made here the way microphone.c makes it
  static int8_t line[20000];
  int end = 0, pos = 0, presses = 6;
  for (int t = 0; t < 20000; t++)
  {
    if (t % 2000 == 0 && t / 2000 < presses)
    {
      end = (end > t ? end : t) + blow_samples();
      end = end > t + blow_max() ? t + blow_max() : end;
    }
    line[t] = t < end ? (int8_t)(blow_wave(pos++) >> 8) : 0;
  }
  int mashing_end = (presses - 1) * 2000, bad = 0, checked = 0;
  for (int at_end = 1100; at_end <= mashing_end; at_end += 4, checked++)
  {
    int all = 1;
    for (int mode = 0; mode < 3; mode++)
    {
      for (int i = 0; i < N; i++)
        sig[i] = line[at_end - N + i];
      all &= check(sig, mode) > 0;
    }
    bad += !all;
  }
  printf("mashing 4 times a second: %d of %d moments are not a blow after the first notice\n", bad,
         checked);
  expect(bad * 100 <= checked * 3, "4 presses a second: at most 3 percent of the moments are lost");
  printf(failures ? "%d checks failed\n" : "all checks passed\n", failures);
  return failures != 0;
}
