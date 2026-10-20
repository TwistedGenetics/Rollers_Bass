// Desktop test harness for Rollers.
// Renders the DSP without any Korg runtime, checks for NaN / clipping / DC,
// aliasing, tuning, zipper noise, level jumps and that the sub stays clean,
// then writes WAV demos to test/out/.
//
// Build & run:  ./test/run.sh

#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "rollers.h"

static const int kSR = 48000;
static const int kBlock = 64;
static int failures = 0;

static float noteW0(float note) { return 440.f * std::pow(2.f, (note - 69.f) / 12.f) / kSR; }
static float noteHz(float note) { return 440.f * std::pow(2.f, (note - 69.f) / 12.f); }

static void writeWav(const std::string &path, const std::vector<float> &x) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t n = x.size(), data = n * 2, riff = 36 + data;
  const uint16_t pcm = 1, ch = 1, bits = 16, align = 2;
  const uint32_t sr = kSR, br = kSR * 2, fmt = 16;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
  fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
  fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
  for (float v : x) {
    int16_t s = (int16_t)std::lrint(std::fmax(-1.f, std::fmin(1.f, v)) * 32767.f);
    fwrite(&s, 2, 1, f);
  }
  fclose(f);
}

static void result(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void result(bool ok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char buf[256];
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  printf("%-62s %s\n", buf, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

struct Stats { float peak = 0, dc = 0, rms = 0; bool bad = false; };

static Stats stats(const std::vector<float> &x, size_t from = 0) {
  Stats s; double sum = 0, sq = 0;
  for (size_t i = from; i < x.size(); ++i) {
    const float v = x[i];
    if (!std::isfinite(v)) s.bad = true;
    s.peak = std::fmax(s.peak, std::fabs(v));
    sum += v; sq += double(v) * v;
  }
  const double n = double(x.size() - from);
  s.dc = sum / n;
  s.rms = std::sqrt(sq / n);
  return s;
}

static void check(const char *name, const std::vector<float> &x) {
  Stats s = stats(x);
  const bool ok = !s.bad && s.peak <= 1.0f && std::fabs(s.dc) < 0.05f && s.peak > 0.01f;
  result(ok, "%-34s peak %.3f  dc %+.4f", name, s.peak, s.dc);
}

// A note held for `seconds`, `automate(t)` called once per block (t = 0..1).
template <typename F>
static std::vector<float> render(Rollers &r, float note, float seconds, F automate, float lfo_hz = 0.f, int block = kBlock) {
  std::vector<float> out(size_t(seconds * kSR));
  r.setPitch(noteW0(note));
  r.noteOn();
  for (size_t i = 0; i < out.size(); i += block) {
    const float t = float(i) / kSR;
    automate(float(i) / out.size());
    r.setShapeLfo(lfo_hz > 0.f ? std::sin(6.2831853f * lfo_hz * t) : 0.f);
    r.process(&out[i], std::min<size_t>(block, out.size() - i));
  }
  return out;
}
static std::vector<float> render(Rollers &r, float note, float seconds) {
  return render(r, note, seconds, [](float) {});
}

// Magnitude of one frequency in a window of x (Hann-windowed single-bin DFT)
static double toneMag(const std::vector<float> &x, size_t off, size_t N, double hz) {
  double re = 0, im = 0;
  const double w = 2 * M_PI * hz / kSR;
  for (size_t n = 0; n < N; ++n) {
    const double win = 0.5 - 0.5 * std::cos(2 * M_PI * n / N);
    re += x[off + n] * win * std::cos(w * n);
    im -= x[off + n] * win * std::sin(w * n);
  }
  return std::sqrt(re * re + im * im) * 4.0 / N;
}

// Energy in DFT bins that are not near a harmonic of f0 (aliasing measure)
static float inharmonicRatio(const std::vector<float> &x, float f0) {
  const int N = 8192;
  const int off = x.size() > (size_t)N ? (x.size() - N) / 2 : 0;
  double harm = 0, other = 0;
  for (int k = 1; k < N / 2; ++k) {
    double re = 0, im = 0;
    const double w = 2 * M_PI * k / N;
    for (int n = 0; n < N; ++n) {
      const double win = 0.5 - 0.5 * std::cos(2 * M_PI * n / N);
      re += x[off + n] * win * std::cos(w * n);
      im -= x[off + n] * win * std::sin(w * n);
    }
    const double p = re * re + im * im;
    const double f = double(k) * kSR / N;
    const double h = f / f0;
    if (std::fabs(h - std::round(h)) * f0 < 3.0 * kSR / N) harm += p; else other += p;
  }
  return float(other / (harm + other + 1e-20));
}

static int zeroCrossHz(const std::vector<float> &x, size_t from) {
  int zc = 0;
  for (size_t i = from + 1; i < x.size(); ++i) zc += (x[i - 1] < 0.f && x[i] >= 0.f);
  return int(std::lround(zc * double(kSR) / double(x.size() - from)));
}

static float maxStep(const std::vector<float> &x) {
  float m = 0;
  for (size_t i = 1; i < x.size(); ++i) m = std::fmax(m, std::fabs(x[i] - x[i - 1]));
  return m;
}

static void setAll(Rollers &r, int blend, int detune, int shape, int drop, int dropt, int voices, int drift,
                   int drive, int crunch, int mode) {
  r.setParam(Rollers::kBlend, blend);
  r.setParam(Rollers::kDetune, detune);
  r.setParam(Rollers::kSubShape, shape);
  r.setParam(Rollers::kDrop, drop);
  r.setParam(Rollers::kDropTime, dropt);
  r.setParam(Rollers::kVoices, voices);
  r.setParam(Rollers::kDrift, drift);
  r.setParam(Rollers::kDrive, drive);
  r.setParam(Rollers::kCrunch, crunch);
  r.setParam(Rollers::kMode, mode);
}

// Starter settings documented in the README (keep in sync)
struct Preset { const char *name; int blend, detune, shape, drop, dropt, voices, drift, drive, crunch, mode; };
static const Preset kPresets[] = {
    {"rolling_sub",   0,    0, Rollers::kSine,      25, 25, 2,  0,  0,  0, Rollers::kModeSub},
    {"classic_reese", 560, 450, Rollers::kSine,      0, 30, 3, 35, 30,  0, Rollers::kModeSubReese},
    {"dark_reese",    350, 650, Rollers::kSine,      0, 30, 4, 55, 60,  0, Rollers::kModeReese},
    {"hoover_stab",   700, 520, Rollers::kSine,     15, 15, 4, 40, 20,  0, Rollers::kModeHoover},
    {"crunchy_94",    600, 400, Rollers::kFolded,   10, 20, 3, 30, 45, 55, Rollers::kModeSubReese},
    {"808_glide",     150, 300, Rollers::kTriangle, 70, 70, 2, 20, 20,  0, Rollers::kModeSubReese},
};

int main() {
  Rollers r;
  if (system("mkdir -p test/out") != 0) return 1;

  // 1. Defaults (as the unit loads) at a few notes
  for (float note : {28.f, 40.f, 52.f}) {
    r.init();
    auto x = render(r, note, 1.f);
    char n[64]; snprintf(n, sizeof n, "defaults note %d", int(note));
    check(n, x);
  }

  // 2. Stress: every mode x sub shape x voice count, everything at max, low to very high notes
  for (int m = 0; m < Rollers::kNumModes; ++m)
    for (int sh = 0; sh < Rollers::kNumShapes; ++sh)
      for (int nv = 2; nv <= 4; ++nv)
        for (float note : {12.f, 36.f, 60.f, 96.f, 127.f}) {
          r.init();
          setAll(r, 1023, 1023, sh, 100, 100, nv, 100, 100, 100, m);
          auto x = render(r, note, 0.3f, [](float) {}, 3.f);
          Stats s = stats(x);
          if (s.bad || s.peak > 1.f || std::fabs(s.dc) > 0.05f || s.peak < 0.01f) {
            char n[96]; snprintf(n, sizeof n, "stress %s %s v%d note %d", Rollers::modeName(m), Rollers::shapeName(sh), nv, int(note));
            check(n, x);
          }
        }
  result(true, "stress: modes x shapes x voices x notes (failures listed above)");

  // 3. Same stress with everything at zero
  for (int m = 0; m < Rollers::kNumModes; ++m) {
    r.init();
    setAll(r, 0, 0, 0, 0, 0, 2, 0, 0, 0, m);
    auto x = render(r, 36, 0.5f);
    Stats s = stats(x);
    const bool ok = !s.bad && s.peak <= 1.f && std::fabs(s.dc) < 0.05f;
    result(ok, "all-zero %-24s peak %.3f", Rollers::modeName(m), s.peak);
  }

  // 4. Odd block sizes (1, 7, 64) and zero pitch must not misbehave
  for (int bs : {1, 7, 64}) {
    r.init();
    setAll(r, 600, 500, 2, 50, 30, 4, 60, 50, 40, Rollers::kModeHoover);
    auto x = render(r, 40, 0.5f, [](float) {}, 0.f, bs);
    char n[64]; snprintf(n, sizeof n, "block size %d", bs);
    check(n, x);
  }
  {
    r.init();
    std::vector<float> x(4800);
    r.setPitch(0.f); r.noteOn(); r.process(x.data(), x.size());
    Stats s = stats(x);
    result(!s.bad && s.peak <= 1.f, "zero pitch stays finite");
  }

  // 5. Tuning: SUB sine, no drop: A1 and A4 by zero-crossing count
  for (float note : {33.f, 69.f}) {
    r.init();
    setAll(r, 0, 0, Rollers::kSine, 0, 0, 2, 0, 0, 0, Rollers::kModeSub);
    auto x = render(r, note, 2.f);
    const int hz = zeroCrossHz(x, 0);
    const int want = int(std::lround(noteHz(note)));
    result(std::abs(hz - want) <= 1, "tuning note %d: %d Hz (want %d)", int(note), hz, want);
  }

  // 6. Pitch drop settles back onto the note
  {
    r.init();
    setAll(r, 0, 0, Rollers::kSine, 100, 40, 2, 0, 0, 0, Rollers::kModeSub);
    auto x = render(r, 45, 3.f);
    const int hz = zeroCrossHz(x, size_t(1.5 * kSR));
    result(std::abs(hz - 110) <= 1, "drop 2 oct settles: %d Hz (want 110)", hz);
    // ...and actually starts high: first 10 ms should be well above 110 Hz
    std::vector<float> head(x.begin(), x.begin() + 480);
    const int start = zeroCrossHz(head, 0);
    result(start > 250, "drop starts high: ~%d Hz in first 10 ms", start);
    writeWav("test/out/drop_808.wav", x);
  }

  // 6b. Legato retrigger: a note-on mid-cycle must not make a step bigger
  //     than the sub's own largest sample-to-sample change
  {
    r.init();
    setAll(r, 0, 0, Rollers::kSine, 0, 0, 2, 0, 0, 0, Rollers::kModeSub);
    r.setPitch(noteW0(33));
    r.noteOn();
    std::vector<float> x(48000);
    for (size_t i = 0; i < x.size(); i += kBlock) {
      if (i == 64 * 101) r.noteOn();  // lands mid-cycle
      r.process(&x[i], kBlock);
    }
    float ref = 0;
    for (size_t i = 1; i < 64 * 100; ++i) ref = std::fmax(ref, std::fabs(x[i] - x[i - 1]));
    const float d = maxStep(x);
    result(d <= ref * 1.5f, "legato retrigger max step %.4f (steady %.4f)", d, ref);
  }

  // 6c. Switching MODE / VOICES mid-note fades instead of jumping
  {
    float ref = 0;
    for (int m = 0; m < Rollers::kNumModes; ++m) {
      r.init(); setAll(r, 512, 450, 0, 0, 0, 3, 0, 25, 0, m);
      ref = std::fmax(ref, maxStep(render(r, 33, 0.3f)));
    }
    r.init(); setAll(r, 512, 450, 0, 0, 0, 3, 0, 25, 0, Rollers::kModeSub);
    auto x = render(r, 33, 2.f, [&](float p) {
      const int step = int(p * 8);
      r.setParam(Rollers::kMode, step % Rollers::kNumModes);
      r.setParam(Rollers::kVoices, 2 + step % 3);
    });
    const float d = maxStep(x);
    result(d <= ref * 1.15f, "mode/voices switching max step %.4f (static %.4f)", d, ref);
  }

  // 7. Aliasing: Reese saws at high notes (no drive/crunch), triangle sub
  for (float note : {84.f, 96.f}) {
    r.init();
    setAll(r, 1023, 0, 0, 0, 0, 2, 0, 0, 0, Rollers::kModeReese);
    auto x = render(r, note, 0.5f);
    const float ir = inharmonicRatio(x, noteHz(note));
    result(ir < 0.01f, "alias reese saw note %d: inharmonic %.5f", int(note), ir);
  }
  for (float note : {84.f, 96.f}) {
    r.init();
    setAll(r, 0, 0, Rollers::kTriangle, 0, 0, 2, 0, 0, 0, Rollers::kModeSub);
    auto x = render(r, note, 0.5f);
    const float ir = inharmonicRatio(x, noteHz(note));
    result(ir < 0.002f, "alias triangle sub note %d: inharmonic %.5f", int(note), ir);
  }
  for (float note : {84.f, 96.f}) {
    r.init();
    setAll(r, 0, 0, Rollers::kFolded, 0, 0, 2, 0, 0, 0, Rollers::kModeSub);
    auto x = render(r, note, 0.5f);
    const float ir = inharmonicRatio(x, noteHz(note));
    result(ir < 0.002f, "alias folded sub note %d: inharmonic %.5f", int(note), ir);
  }

  // 8. Sub stays clean: in SUB+REESE, drive + crunch must not change the
  //    fundamental (owned by the sub) by more than 1.5 dB
  {
    auto fund = [&](int drive, int crunch) {
      r.init();
      setAll(r, 512, 450, Rollers::kSine, 0, 0, 3, 0, drive, crunch, Rollers::kModeSubReese);
      auto x = render(r, 33, 1.f);
      return toneMag(x, kSR / 2, 16384, noteHz(33));
    };
    const double clean = fund(0, 0), dirty = fund(100, 100);
    const double db = 20 * std::log10(dirty / clean);
    result(std::fabs(db) < 1.5, "sub fundamental clean %.3f / dirty %.3f (%+.2f dB)", clean, dirty, db);
  }

  // 9. Zipper: sweeping a control never steps harder than either end does statically
  {
    struct Sweep { const char *name; int id, lo, hi, mode; };
    const Sweep sweeps[] = {
        {"blend", Rollers::kBlend, 0, 1023, Rollers::kModeSubReese},
        {"tone (reese)", Rollers::kBlend, 0, 1023, Rollers::kModeReese},
        {"drive", Rollers::kDrive, 0, 100, Rollers::kModeSubReese},
        {"detune", Rollers::kDetune, 0, 1023, Rollers::kModeSubReese},
    };
    for (const Sweep &s : sweeps) {
      float ref = 0;
      for (int v : {s.lo, s.hi}) {
        r.init(); setAll(r, 512, 450, 0, 0, 0, 3, 0, 25, 0, s.mode); r.setParam(s.id, v);
        ref = std::fmax(ref, maxStep(render(r, 33, 0.5f)));
      }
      r.init(); setAll(r, 512, 450, 0, 0, 0, 3, 0, 25, 0, s.mode);
      auto x = render(r, 33, 1.f, [&](float p) { r.setParam(s.id, int(s.lo + (s.hi - s.lo) * p)); });
      const float d = maxStep(x);
      result(d <= ref * 1.15f, "zipper %-14s max step %.4f (static %.4f)", s.name, d, ref);
    }
  }

  // 10. Level consistency: voice count and drive shouldn't jump the level much
  {
    auto rmsOf = [&](int voices, int drive, int mode) {
      r.init(); setAll(r, 700, 450, 0, 0, 0, voices, 30, drive, 0, mode);
      return stats(render(r, 36, 1.f), kSR / 4).rms;
    };
    for (int mode : {Rollers::kModeReese, Rollers::kModeSubReese, Rollers::kModeHoover}) {
      float lo = 1e9f, hi = 0;
      for (int nv = 2; nv <= 4; ++nv) { float v = rmsOf(nv, 25, mode); lo = std::fmin(lo, v); hi = std::fmax(hi, v); }
      const float db = 20 * std::log10(hi / lo);
      result(db < 3.f, "level vs voices %-14s spread %.2f dB", Rollers::modeName(mode), db);
      const float d0 = rmsOf(3, 0, mode), d1 = rmsOf(3, 100, mode);
      const float dd = 20 * std::log10(d1 / d0);
      result(std::fabs(dd) < 3.f, "level vs drive  %-14s 0->100%%: %+.2f dB", Rollers::modeName(mode), dd);
    }
    float lo = 1e9f, hi = 0;
    for (int m = 0; m < Rollers::kNumModes; ++m) {
      r.init(); setAll(r, 512, 450, 0, 0, 0, 3, 30, 25, 0, m);
      float v = stats(render(r, 36, 1.f), kSR / 4).rms;
      lo = std::fmin(lo, v); hi = std::fmax(hi, v);
    }
    const float db = 20 * std::log10(hi / lo);
    result(db < 6.f, "level across modes spread %.2f dB", db);
  }

  // 11. Starter settings: render a short jungle-ish riff for each
  const float riff[] = {33, 33, 36, 31, 33, 33, 38, 36};
  for (const Preset &p : kPresets) {
    r.init(0xC0FFEEu);
    setAll(r, p.blend, p.detune, p.shape, p.drop, p.dropt, p.voices, p.drift, p.drive, p.crunch, p.mode);
    std::vector<float> x;
    const int step = int(kSR * 60.0 / 170.0 / 2.0);  // 8th notes at 170 BPM
    for (float n : riff) {
      std::vector<float> seg(step * 2);
      r.setPitch(noteW0(n));
      r.noteOn();
      for (size_t i = 0; i < seg.size(); i += kBlock) r.process(&seg[i], std::min<size_t>(kBlock, seg.size() - i));
      x.insert(x.end(), seg.begin(), seg.end());
    }
    char n[64]; snprintf(n, sizeof n, "preset %s", p.name);
    check(n, x);
    writeWav(std::string("test/out/preset_") + p.name + ".wav", x);
  }

  // 12. Speed (desktop only: a relative number, not a hardware measurement)
  {
    r.init();
    setAll(r, 600, 600, Rollers::kFolded, 50, 50, 4, 80, 80, 80, Rollers::kModeHoover);
    std::vector<float> buf(kBlock);
    r.setPitch(noteW0(36)); r.noteOn();
    const int blocks = 48000 * 10 / kBlock;
    auto t0 = std::chrono::steady_clock::now();
    float acc = 0;
    for (int b = 0; b < blocks; ++b) { r.process(buf.data(), kBlock); acc += buf[0]; }
    auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (blocks * kBlock);
    printf("worst case (HOOVER, 4 voices, drop+drive+crunch): %.1f ns/sample on this machine (%.2f%% of real time)%s\n",
           ns, ns / (1e9 / kSR) * 100.0, acc > 1e9 ? "" : "");
  }

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
