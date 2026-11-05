// The Nintendo sound driver (in the translated ARM7) programs the channels
// by writing at 0x04000400 + 16 * channel:
//+0 CNT  volume (0-6), divider (8-9), pan (16-22), PSG duty (24-26),
// repeat (27-28: 0 manual, 1 loop, 2 once), format (29-30:
// PCM8, PCM16, ADPCM, PSG/noise), running (31)
//+4 SAD  address of the samples    +8 TMR  frequency (16.76 MHz / (0x10000 - TMR))
//+A PNT  loop start (words)  +C LEN  length after the loop (words)
// and at 0x04000500 the master volume.
#include "compat.h" // before cpu.h: its R macro clashes with windows.h
#include <mmsystem.h>
#include "ds.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>

#define FREQ 32768
#define SAMPLES 512 // per buffer (stereo, 16 bits)
#define NBUF 6
#define CLOCK_SOUND (33513982.0 / 2)

extern uint8_t* ds_io7(void);
volatile uint32_t g_snd_gen[16]; // changes each time the game starts a channel

typedef struct
{
  uint32_t gen;
  int active;
  double pos; // in samples
  int16_t adpcm, adpcm_loop;
  int idx, idx_loop, loop_stored;
  uint32_t last; // last decoded ADPCM sample (index)
  uint16_t lfsr;
  int16_t current;
} Channel;

static Channel channel[16];
static volatile int volume = 100, mute; // from the menu (0-100)

static const int adpcm_idx[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static const uint16_t adpcm_step[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static uint32_t r32(const uint8_t* io, uint32_t o)
{
  uint32_t v;
  memcpy(&v, io + o, 4);
  return v;
}
static uint16_t r16(const uint8_t* io, uint32_t o)
{
  uint16_t v;
  memcpy(&v, io + o, 2);
  return v;
}

static void adpcm_step1(Channel* c, uint8_t n)
{
  int step = adpcm_step[c->idx];
  int dif = step / 8;
  if (n & 1)
    dif += step / 4;
  if (n & 2)
    dif += step / 2;
  if (n & 4)
    dif += step;
  int s = c->adpcm + ((n & 8) ? -dif : dif);
  if (s > 0x7FFF)
    s = 0x7FFF;
  if (s < -0x7FFF)
    s = -0x7FFF;
  c->adpcm = (int16_t)s;
  c->idx += adpcm_idx[n & 7];
  if (c->idx < 0)
    c->idx = 0;
  if (c->idx > 88)
    c->idx = 88;
}

static void reset(Channel* c, uint32_t sad)
{
  c->active = 1;
  c->pos = 0;
  c->loop_stored = 0;
  c->last = 0;
  c->lfsr = 0x7FFF;
  uint32_t h = rd32_7(sad); // ADPCM: header with the first sample and the index
  c->adpcm = (int16_t)(h & 0xFFFF);
  c->idx = (int)((h >> 16) & 0x7F);
  if (c->idx > 88)
    c->idx = 88;
}

// One sample of channel n (and advances)
static int16_t show(int n, const uint8_t* io, double step)
{
  Channel* c = &channel[n];
  uint32_t o = 0x400 + 16 * n;
  uint32_t cnt = r32(io, o), sad = r32(io, o + 4) & 0x07FFFFFC;
  uint32_t pnt = r16(io, o + 0xA), len = r32(io, o + 0xC) & 0x3FFFFF;
  int format = (cnt >> 29) & 3, rep = (cnt >> 27) & 3;
  uint32_t total, loop;
  switch (format)
  {
  case 0:
    total = (pnt + len) * 4;
    loop = pnt * 4;
    break;
  case 1:
    total = (pnt + len) * 2;
    loop = pnt * 2;
    break;
  case 2:
    total = (pnt + len) * 8;
    loop = pnt * 8;
    if (total >= 8)
      total -= 8;
    loop = loop >= 8 ? loop - 8 : 0;
    break;
  default:
    total = 0xFFFFFFFF;
    loop = 0;
    break;
  }
  uint32_t p = (uint32_t)c->pos;
  if (p >= total)
  {
    if (rep == 2 || total == 0)
    { // once: it ended
      c->active = 0;
      uint8_t* w = ds_io7();
      w[o + 3] &= 0x7F;
      return 0;
    }
    c->pos = loop + (c->pos - total);
    p = (uint32_t)c->pos;
    if (format == 2 && c->loop_stored)
    {
      c->adpcm = c->adpcm_loop;
      c->idx = c->idx_loop;
      c->last = loop;
    }
  }
  int16_t s = 0;
  switch (format)
  {
  case 0:
    s = (int16_t)((int8_t)rd8_7(sad + p) << 8);
    break;
  case 1:
    s = (int16_t)rd16_7(sad + p * 2);
    break;
  case 2:
    while (c->last <= p)
    {
      if (c->last == loop && !c->loop_stored)
      {
        c->adpcm_loop = c->adpcm;
        c->idx_loop = c->idx;
        c->loop_stored = 1;
      }
      uint8_t b = (uint8_t)rd8_7(sad + 4 + c->last / 2);
      adpcm_step1(c, (c->last & 1) ? b >> 4 : b & 15);
      c->last++;
    }
    s = c->adpcm;
    break;
  default:
    if (n >= 8 && n <= 13)
    { // onda cuadrada
      int cycle = (cnt >> 24) & 7;
      s = ((p & 7) <= (uint32_t)cycle) ? 0x7FFF : -0x7FFF;
    }
    else if (n >= 14)
    { // ruido
      uint32_t before = (uint32_t)(c->pos - step);
      for (uint32_t k = before; k < p; k++)
      {
        int bit = c->lfsr & 1;
        c->lfsr >>= 1;
        if (bit)
          c->lfsr ^= 0x6000;
      }
      s = (c->lfsr & 1) ? -0x7FFF : 0x7FFF;
    }
    break;
  }
  c->pos += step;
  return s;
}

static void mix(int16_t* outbuf, int n_samples)
{
  const uint8_t* io = ds_io7();
  uint16_t soundcnt = r16(io, 0x500);
  int master = soundcnt & 0x7F;
  for (int i = 0; i < n_samples; i++)
  {
    int32_t left_ch = 0, right = 0;
    if (soundcnt & 0x8000)
    {
      for (int n = 0; n < 16; n++)
      {
        uint32_t o = 0x400 + 16 * n;
        uint32_t cnt = r32(io, o);
        Channel* c = &channel[n];
        if (c->gen != g_snd_gen[n])
        {
          c->gen = g_snd_gen[n];
          reset(c, r32(io, o + 4) & 0x07FFFFFC);
        }
        if (!(cnt & 0x80000000) || !c->active)
          continue;
        uint16_t tmr = r16(io, o + 8);
        double step = CLOCK_SOUND / (double)(0x10000 - tmr) / FREQ;
        int32_t s = show(n, io, step);
        int vol = cnt & 0x7F, div = (cnt >> 8) & 3, pan = (cnt >> 16) & 0x7F;
        static const int shift[4] = {0, 1, 2, 4};
        s = (s * vol / 128) >> shift[div];
        left_ch += s * (128 - pan) / 128;
        right += s * pan / 128;
      }
    }
    int vol = mute ? 0 : volume;
    left_ch = left_ch * master / 128 / 4 * vol / 100;
    right = right * master / 128 / 4 * vol / 100;
    if (left_ch > 32767)
      left_ch = 32767;
    if (left_ch < -32768)
      left_ch = -32768;
    if (right > 32767)
      right = 32767;
    if (right < -32768)
      right = -32768;
    outbuf[i * 2] = (int16_t)left_ch;
    outbuf[i * 2 + 1] = (int16_t)right;
  }
}

static DWORD WINAPI thread_sound(LPVOID p)
{
  (void)p;
  HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
  WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 2, FREQ, FREQ * 4, 4, 16, 0};
  HWAVEOUT wo;
  if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
  {
    log_msg("sound", "could not open the audio output: the game will be silent");
    return 0;
  }
  log_msg("sound", "audio output open, %d Hz, %d buffers of %d samples", FREQ, NBUF, SAMPLES);
  static int16_t raw_data[NBUF][SAMPLES * 2];
  static WAVEHDR hdr[NBUF];
  for (int b = 0; b < NBUF; b++)
  {
    hdr[b].lpData = (LPSTR)raw_data[b];
    hdr[b].dwBufferLength = sizeof raw_data[b];
    waveOutPrepareHeader(wo, &hdr[b], sizeof hdr[b]);
    mix(raw_data[b], SAMPLES);
    waveOutWrite(wo, &hdr[b], sizeof hdr[b]);
  }
  for (;;)
  {
    WaitForSingleObject(event, 100);
    for (int b = 0; b < NBUF; b++)
    {
      if (!(hdr[b].dwFlags & WHDR_DONE))
        continue;
      mix(raw_data[b], SAMPLES);
      waveOutWrite(wo, &hdr[b], sizeof hdr[b]);
    }
  }
}

void sound_init(void)
{
  const char* vol = getenv("DRIFT_VOLUME"); // settings.ini: volume 0-100 and mute
  if (vol)
  {
    int v = atoi(vol);
    volume = v < 0 ? 0 : v > 100 ? 100 : v;
  }
  const char* mute_cfg = getenv("DRIFT_MUTE");
  mute = mute_cfg && atoi(mute_cfg);
  HANDLE h = CreateThread(NULL, 0, thread_sound, NULL, 0, NULL);
  SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
}
