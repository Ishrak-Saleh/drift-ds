//checks the microphone signal chain of runtime/microphone.c with made up samples: a silent room, a
//steady offset, a real blow, and the blow made by the MIC_KEY key.
//build and run:  clang -O1 -w -I runtime tests/test_mic.c runtime/keys.c -lwinmm -o
//tests/test_mic.exe
#include "../runtime/microphone.c"

void log_msg(const char* tag, const char* fmt, ...)
{
}

int blow_samples(void);
int blow_max(void);
static int held;
int window_key_held(int key)
{
  return held && key == TK_SPACE;
}

static int failures;
static void check(int ok, const char* what)
{
  if (!ok)
  {
    printf("FAIL: %s\n", what);
    failures++;
  }
}

//the share of samples (percent) that the game counts as "at the top" (125 of 127 in 8 bits)
static int top_percent(int offset, int loud, int g, int n)
{
  gain = g;
  dc = 0;
  stat_n = stat_peak = stat_top = 0;
  int top = 0, counted = 0;
  uint32_t x = 12345;
  for (int i = 0; i < n; i++)
  {
    x = x * 1664525u + 1013904223u;
    int raw = offset + ((int)(x >> 16) % (2 * loud + 1)) - loud;
    int16_t s = amplify(raw);
    if (i >= 2000) //the offset tracker needs about 2000 samples to settle
    {
      counted++;
      if (s >= 32000 || s <= -32000)
        top++;
    }
  }
  return top * 100 / counted;
}

int main(void)
{
  check(top_percent(0, 550, 8, 8000) == 0, "a silent room (peaks 550, gain 8) is not a blow");
  check(top_percent(3000, 550, 8, 8000) == 0, "a steady offset of 3000 is not a blow");
  check(top_percent(0, 9000, 8, 8000) > 40, "a real blow (peaks 9000, gain 8) is");
  check(top_percent(0, 2500, 16, 8000) > 15, "a weak blow (peaks 2500) is heard at gain 16");
  check(top_percent(0, 2500, 1, 8000) == 0, "the same weak blow is not heard at gain 1");

  //the key: one press gives one short blow, holding gives no more
  recording = 1;
  blow_key = TK_SPACE;
  held = 0;
  check(mic_pc_show() == 0, "key not held: silence");
  held = 1;
  int top = 0, loud = 0;
  for (int i = 0; i < 8000; i++)
  {
    int16_t s = mic_pc_show();
    top += (s >= 32000 || s <= -32000);
    loud += s != 0;
  }
  check(loud >= blow_samples() - 60 && loud <= blow_samples(),
        "key held for a second: one blow only");
  check(top > blow_samples() / 4,
        "the blow is loud: a quarter or more of its samples are at the top");
  held = 0;
  mic_pc_show();
  held = 1;
  check(mic_pc_show() != 0, "released and pressed again: a new blow");
  //mashing: a press every `gap` samples (a press = key down for 800 samples, then up). 4 presses a
  //second is a gap of 2000 samples and must keep the blow going; 2 a second must not
  int gaps[2] = {2000, 4000};
  for (int g = 0; g < 2; g++)
  {
    blow_left = 0;
    held = 0;
    mic_pc_show();
    int on = 0, total = 24000; //3 seconds
    for (int i = 0; i < total; i++)
    {
      held = (i % gaps[g]) < 800;
      on += mic_pc_show() != 0;
    }
    if (g == 0)
      check(on * 100 >= total * 97, "4 presses a second keep the blow going (97 percent of 3 s)");
    else
      check(on * 100 <= total * 60, "2 presses a second let the blow stop between the presses");
  }
  //pressing many times at once never stores more than the cap
  blow_left = 0;
  for (int k = 0; k < 20; k++)
  {
    held = 0;
    mic_pc_show();
    held = 1;
    mic_pc_show();
  }
  check(blow_left <= blow_max(), "stored blow never goes over the cap");
  held = 0;
  recording = 0;
  blow_left = 0;
  check(mic_pc_show() == 0, "not recording: silence");

  //delay: a ring that has built up one second is cut to 100 ms, a small one is left alone
  entry_point = (HWAVEIN)1; //only so the ring is read, no real device is used
  gain = 1;
  for (int i = 0; i < 8192; i++)
    ring[i] = (int16_t)(i % 100);
  read = 0;
  write = 5000; //625 ms behind
  mic_pc_show();
  int lag = (write - read + 8192) % 8192;
  check(lag <= 400, "a delay of 625 ms is cut to 50 ms");
  read = 0;
  write = 500; //62 ms, under the limit
  mic_pc_show();
  check(read == 1, "a delay under 100 ms is left alone");
  entry_point = NULL;
  printf(failures ? "%d checks failed\n" : "all checks passed\n", failures);
  return failures != 0;
}
