//PC microphone for the DS microphone (Balloon Battle: blowing inflates the balloon).
//It opens only when the game starts recording and closes when it stops. Records at 8000 samples
//per second, 16 bits. ds.c (mic_update) asks for samples at the pace of the game. Without a
//microphone, or with DRIFT_MIC=0 (MICRO=0 in settings.ini, launcher option), it gives silence.
#include <windows.h>
#include <math.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "keys.h"
#include "log.h"

#define NBUF 6
#define SAMPLES 400 //per buffer: 50 ms
static int16_t ring[8192];
static int read, write; //ring of what was recorded
static HWAVEIN entry_point;
static WAVEHDR hdr[NBUF];
static int16_t raw_data[NBUF][SAMPLES];

static int active = -1, gain = -1; //from settings.ini (DRIFT_MIC and DRIFT_MIC_GAIN)
static int blow_key = -1; //key id of MIC_KEY (DRIFT_MIC_KEY), TK_NONE for none
static int recording; //the game is recording (mic_pc_open without mic_pc_close)

int window_key_held(int key); //window.c: 1 while the key is held and the game window has focus

static int allowed(void)
{
  if (active < 0)
  {
    const char* m = getenv("DRIFT_MIC");
    active = !m || atoi(m) != 0;
  }
  return active;
}

void mic_pc_close(void);

//MIC_KEY (DRIFT_MIC_KEY in settings.ini): every press of this key gives one short blow, so
//Balloon Battle can be played without a microphone by pressing the key again and again. Holding the
//key gives one blow only: the key has to be released and pressed again
static int key_wanted(void)
{
  if (blow_key < 0)
  {
    const char* k = getenv("DRIFT_MIC_KEY");
    blow_key = k && *k ? key_from_name(k) : TK_NONE;
  }
  return blow_key != TK_NONE;
}

void mic_pc_open(void)
{
  recording = 1;
  log_msg("mic", "the game started recording (PC microphone %s, blow key %s)",
          allowed() ? "on" : "off", key_wanted() ? getenv("DRIFT_MIC_KEY") : "none");
  if (entry_point || !allowed())
    return;
  WAVEFORMATEX f = {WAVE_FORMAT_PCM, 1, 8000, 16000, 2, 16, 0};
  //DRIFT_MIC_DEVICE (MIC_DEVICE in settings.ini): the microphone name
  //as Windows gives it (waveInGetDevCaps, 31 letters at most). If it is not found, the default
  UINT disp = WAVE_MAPPER;
  const char* label = getenv("DRIFT_MIC_DEVICE");
  if (label && *label)
  {
    wchar_t searched[64];
    MultiByteToWideChar(CP_UTF8, 0, label, -1, searched, 64);
    for (UINT i = 0; i < waveInGetNumDevs(); i++)
    {
      WAVEINCAPSW caps;
      if (waveInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR &&
          !wcscmp(caps.szPname, searched))
      {
        disp = i;
        break;
      }
    }
  }
  MMRESULT r = waveInOpen(&entry_point, disp, &f, 0, 0, CALLBACK_NULL);
  if (r != MMSYSERR_NOERROR)
  {
    entry_point = NULL;
    log_msg("mic", "could not open the microphone (error %u): blowing will not work", (unsigned)r);
    return;
  }
  log_msg("mic", "microphone open");
  for (int i = 0; i < NBUF; i++)
  {
    memset(&hdr[i], 0, sizeof hdr[i]);
    hdr[i].lpData = (LPSTR)raw_data[i];
    hdr[i].dwBufferLength = sizeof raw_data[i];
    waveInPrepareHeader(entry_point, &hdr[i], sizeof hdr[i]);
    waveInAddBuffer(entry_point, &hdr[i], sizeof hdr[i]);
  }
  read = write = 0;
  waveInStart(entry_point);
}

void mic_pc_close(void)
{
  recording = 0;
  if (!entry_point)
    return;
  waveInReset(entry_point);
  for (int i = 0; i < NBUF; i++)
    waveInUnprepareHeader(entry_point, &hdr[i], sizeof hdr[i]);
  waveInClose(entry_point);
  entry_point = NULL;
}

//moves to the ring the buffers Windows has already filled and puts them to record again
static void collect(void)
{
  for (int i = 0; i < NBUF; i++)
  {
    if (!(hdr[i].dwFlags & WHDR_DONE))
      continue;
    int n = (int)hdr[i].dwBytesRecorded / 2;
    for (int k = 0; k < n; k++)
    {
      ring[write] = raw_data[i][k];
      write = (write + 1) % 8192;
      if (write == read)
        read = (read + 1) % 8192; //full: the oldest is lost
    }
    hdr[i].dwFlags &= ~WHDR_DONE;
    waveInAddBuffer(entry_point, &hdr[i], sizeof hdr[i]);
  }
}

//the game (0201EC40) decides you are blowing if many samples hit the top (|s| >= 125 of 127) and
//by the shape of the wave: the DS microphone saturates when blown. The PC one records much weaker
//(a Razer Seiren gives peaks of ~550 in a silent room), so it is amplified and clipped as the DS
//would. DRIFT_MIC_GAIN (MIC_GAIN in settings.ini): the multiplier, 1 to 16. 8 if not set.
//A steady offset of the microphone is removed first, or it would use up part of the range and
//count as a blow by itself.
static int dc; //running average of the raw samples, x256
static int stat_n, stat_peak, stat_top;

static int16_t amplify(int s)
{
  if (gain < 0)
  {
    const char* g = getenv("DRIFT_MIC_GAIN");
    gain = g && atoi(g) > 0 ? (atoi(g) > 16 ? 16 : atoi(g)) : 8; //1 to 16, as settings.ini says
  }
  dc += ((s * 256) - dc) >> 8;
  s -= dc >> 8;
  int a = s < 0 ? -s : s;
  s *= gain;
  int top = s > 32767 || s < -32768 || s >= 32000 || s <= -32000;
  //one line per second with what the microphone gave: the peak before the gain, and how many
  //samples reached the top after it. it shows if the sensitivity is too low or too high
  stat_peak = a > stat_peak ? a : stat_peak;
  stat_top += top;
  if (++stat_n >= 8000)
  {
    log_msg("mic", "last second: peak %d, x%d gain, %d%% of the samples at the top", stat_peak,
            gain, stat_top * 100 / stat_n);
    stat_n = stat_peak = stat_top = 0;
  }
  return (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

//the blow of one key press. The game (0201EC30) does not just look for loud sound: it takes the
//newest 128 samples of its 1024 byte buffer, measures how long the wave stays above and below
//zero, and compares that with fixed ranges. Random noise has far too short waves and is never
//taken for a blow. This steady wave (three sine waves, hard clipped) is taken at every position
//of the 128 sample window and in the three modes of the game: tests/test_blow.c runs the game's
//own check on it
//every press adds 250 ms (2000 samples at 8000 per second) of blow to what is left, up to 500 ms.
//The blow runs out in real time, so pressing 4 times a second keeps it going and fewer presses
//let it stop. The game needs about 100 ms of blow in its buffer before it notices, so the first
//press is noticed a little after it was made
#define PRESS_SAMPLES 2000
#define BLOW_MAX 4000
int blow_samples(void)
{
  return PRESS_SAMPLES;
}
int blow_max(void)
{
  return BLOW_MAX;
}
static int blow_left, key_was_down, blow_pos; //blow_pos: where the wave is, it never restarts

int16_t blow_wave(int i)
{
  //repeats exactly every 313 samples, so every window of the game looks like one of 313 shifted
  //copies, all of which were tested
  static const int multiple[3] = {2, 1, 22};
  static const double amp[3] = {1, 0.37, 0.84};
  double v = 0;
  for (int k = 0; k < 3; k++)
    v += amp[k] * sin(6.2831853 * multiple[k] * (i % 313) / 313 + k);
  v *= 4 * 40 * 256; //gain 4 on the 8 bit range of the game, in the 16 bits used here
  return (int16_t)(v > 32512 ? 32512 : v < -32512 ? -32512 : (int)v); //at most 127 of 127 in 8 bits
}

static int16_t keyboard_blow(void)
{
  return blow_wave(blow_pos++);
}

//the next sample (silence if there is no microphone or nothing has arrived yet)
int16_t mic_pc_show(void)
{
  if (recording && key_wanted())
  {
    int down = window_key_held(blow_key);
    if (down && !key_was_down)
      blow_left =
          blow_left + PRESS_SAMPLES > BLOW_MAX ? BLOW_MAX : blow_left + PRESS_SAMPLES; //a new press
    key_was_down = down;
    if (blow_left > 0)
    {
      blow_left--;
      if (entry_point && read != write)
        read = (read + 1) % 8192; //the real sample is dropped so the ring does not fill up
      return keyboard_blow();
    }
  }
  if (!entry_point)
    return 0;
  if (read == write)
    collect();
  if (read == write)
    return 0;
  //no delay: the ring is only emptied at the pace of the game and the clocks of the microphone
  //and the game differ a little, so it could fill up to a second. at most 100 ms are kept (the
  //oldest is thrown away)
  int lag = (write - read + 8192) % 8192;
  if (lag > 800)
    read = (write - 400 + 8192) % 8192;
  int16_t s = ring[read];
  read = (read + 1) % 8192;
  return amplify(s);
}
