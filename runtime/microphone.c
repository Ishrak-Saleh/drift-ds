// PC microphone for the DS microphone (Balloon Battle: blowing inflates the balloon).
// It opens only when the game starts recording and closes when it stops. Records at 8000 samples
// per second, 16 bits. ds.c (mic_update) asks for samples at the pace of the game. Without a
// microphone, or with DRIFT_MIC=0 (MICRO=0 in settings.ini, launcher option), it gives silence.
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

#define NBUF 6
#define SAMPLES 400 // per buffer: 50 ms
static int16_t ring[8192];
static int read, write; // ring of what was recorded
static HWAVEIN entry_point;
static WAVEHDR hdr[NBUF];
static int16_t raw_data[NBUF][SAMPLES];

static int active = -1, gain = -1; // from settings.ini (DRIFT_MIC and DRIFT_MIC_GAIN)

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

void mic_pc_open(void)
{
  if (entry_point || !allowed())
    return;
  WAVEFORMATEX f = {WAVE_FORMAT_PCM, 1, 8000, 16000, 2, 16, 0};
  // DRIFT_MIC_DEVICE (MIC_DEVICE in settings.ini): the microphone name
  // as Windows gives it (waveInGetDevCaps, 31 letters at most). If it is not found, the default
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
  if (!entry_point)
    return;
  waveInReset(entry_point);
  for (int i = 0; i < NBUF; i++)
    waveInUnprepareHeader(entry_point, &hdr[i], sizeof hdr[i]);
  waveInClose(entry_point);
  entry_point = NULL;
}

// moves to the ring the buffers Windows has already filled and puts them to record again
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
        read = (read + 1) % 8192; // full: the oldest is lost
    }
    hdr[i].dwFlags &= ~WHDR_DONE;
    waveInAddBuffer(entry_point, &hdr[i], sizeof hdr[i]);
  }
}

// The game (0201EC40) decides you are blowing if many samples hit the top (|s| >= 125 of 127) and
// by the shape of the wave: the DS microphone saturates when blown. The PC one records much weaker
// (a Razer Seiren gives peaks of ~550 in a silent room), so it is amplified and clipped as the DS
// would. DRIFT_MIC_GAIN (MIC_GAIN in settings.ini): the multiplier. 8 if not set.
static int16_t amplify(int s)
{
  if (gain < 0)
  {
    const char* g = getenv("DRIFT_MIC_GAIN");
    gain = g && atoi(g) > 0 ? atoi(g) : 8;
  }
  s *= gain;
  return (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

// the next sample (silence if there is no microphone or nothing has arrived yet)
int16_t mic_pc_show(void)
{
  if (!entry_point)
    return 0;
  if (read == write)
    collect();
  if (read == write)
    return 0;
  int16_t s = ring[read];
  read = (read + 1) % 8192;
  return amplify(s);
}
