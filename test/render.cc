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
#include "rollers_lean4.h"

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

// ---------------- microKORG 2 lean renderer ----------------

static void setAllG(RollersGroup4 &g, int blend, int detune, int shape, int drop, int dropt, int voices, int drift,
                    int drive, int crunch, int mode) {
  g.setParam(Rollers::kBlend, blend);
  g.setParam(Rollers::kDetune, detune);
  g.setParam(Rollers::kSubShape, shape);
  g.setParam(Rollers::kDrop, drop);
  g.setParam(Rollers::kDropTime, dropt);
  g.setParam(Rollers::kVoices, voices);
  g.setParam(Rollers::kDrift, drift);
  g.setParam(Rollers::kDrive, drive);
  g.setParam(Rollers::kCrunch, crunch);
  g.setParam(Rollers::kMode, mode);
}

// Four lanes at notes n0..n3 (all live), returns lane `lane`. automate(t) per buffer.
template <typename F>
static std::vector<float> renderG(RollersGroup4 &g, const float notes[4], float seconds, int lane, F automate, int block = 64) {
  std::vector<float> out(size_t(seconds * kSR));
  const bool live[4] = {true, true, true, true};
  for (int j = 0; j < 4; ++j) { g.setPitch(j, noteW0(notes[j])); g.noteOn(j, true); }
  r4f buf[64];
  for (size_t i = 0; i < out.size(); i += block) {
    automate(float(i) / out.size());
    const uint32_t n = (uint32_t)std::min<size_t>(block, out.size() - i);
    g.render(live, buf, n);
    for (uint32_t s = 0; s < n; ++s) out[i + s] = buf[s][lane];
  }
  return out;
}
static std::vector<float> renderG(RollersGroup4 &g, float note, float seconds) {
  const float notes[4] = {note, note + 7, note + 12, note - 5};
  return renderG(g, notes, seconds, 0, [](float) {});
}

static void testLean() {
  printf("\n-- microKORG 2 lean renderer --\n");
  RollersGroup4 g;

  // stress: every mode x shape x voices, all max, wide note range, all 4 lanes
  for (int m = 0; m < Rollers::kNumModes; ++m)
    for (int sh = 0; sh < Rollers::kNumShapes; ++sh)
      for (int nv = 2; nv <= 4; ++nv)
        for (float note : {12.f, 36.f, 60.f, 96.f, 120.f}) {
          for (int lane = 0; lane < 4; ++lane) {
            g.init(0xABCDu);
            setAllG(g, 1023, 1023, sh, 100, 100, nv, 100, 100, 100, m);
            const float notes[4] = {note, note + 5, note - 7, note + 0.5f};
            auto x = renderG(g, notes, 0.3f, lane, [](float) {}, 64);
            Stats s = stats(x);
            if (s.bad || s.peak > 1.f || std::fabs(s.dc) > 0.05f || s.peak < 0.01f) {
              char n[96]; snprintf(n, sizeof n, "lean stress %s %s v%d note %d lane %d", Rollers::modeName(m), Rollers::shapeName(sh), nv, int(note), lane);
              check(n, x);
            }
          }
        }
  result(true, "lean stress: modes x shapes x voices x notes x lanes");

  for (int bs : {1, 7, 64}) {
    g.init(1);
    setAllG(g, 600, 500, 2, 50, 30, 4, 60, 50, 40, Rollers::kModeHoover);
    const float notes[4] = {40, 47, 52, 35};
    auto x = renderG(g, notes, 0.5f, 1, [](float) {}, bs);
    char n[64]; snprintf(n, sizeof n, "lean block size %d", bs);
    check(n, x);
  }

  // tuning
  for (float note : {33.f, 69.f}) {
    g.init(1);
    setAllG(g, 0, 0, Rollers::kSine, 0, 0, 2, 0, 0, 0, Rollers::kModeSub);
    auto x = renderG(g, note, 2.f);
    const int hz = zeroCrossHz(x, 0), want = int(std::lround(noteHz(note)));
    result(std::abs(hz - want) <= 1, "lean tuning note %d: %d Hz (want %d)", int(note), hz, want);
  }
  // drop settles and starts high
  {
    g.init(1);
    setAllG(g, 0, 0, Rollers::kSine, 100, 40, 2, 0, 0, 0, Rollers::kModeSub);
    auto x = renderG(g, 45, 3.f);
    const int hz = zeroCrossHz(x, size_t(1.5 * kSR));
    result(std::abs(hz - 110) <= 1, "lean drop settles: %d Hz (want 110)", hz);
    std::vector<float> head(x.begin(), x.begin() + 480);
    result(zeroCrossHz(head, 0) > 250, "lean drop starts high: ~%d Hz", zeroCrossHz(head, 0));
  }
  // aliasing: DPW saws (less rejection than polyBLEP, fine for bass)
  for (float note : {60.f, 72.f, 84.f}) {
    g.init(1);
    setAllG(g, 1023, 0, 0, 0, 0, 2, 0, 0, 0, Rollers::kModeReese);
    auto x = renderG(g, note, 0.5f);
    const float ir = inharmonicRatio(x, noteHz(note));
    result(ir < (note < 80 ? 0.01f : 0.03f), "lean alias reese saw note %d: inharmonic %.5f", int(note), ir);
  }
  // sub stays clean
  {
    auto fund = [&](int drive, int crunch) {
      g.init(1);
      setAllG(g, 512, 450, Rollers::kSine, 0, 0, 3, 0, drive, crunch, Rollers::kModeSubReese);
      auto x = renderG(g, 33, 1.f);
      return toneMag(x, kSR / 2, 16384, noteHz(33));
    };
    const double clean = fund(0, 0), dirty = fund(100, 100);
    const double db = 20 * std::log10(dirty / clean);
    result(std::fabs(db) < 1.5, "lean sub fundamental %+.2f dB with drive+crunch at max", db);
  }
  // zipper sweeps
  {
    struct Sweep { const char *name; int id, lo, hi, mode; };
    const Sweep sweeps[] = {
        {"blend", Rollers::kBlend, 0, 1023, Rollers::kModeSubReese},
        {"tone (reese)", Rollers::kBlend, 0, 1023, Rollers::kModeReese},
        {"drive", Rollers::kDrive, 0, 100, Rollers::kModeSubReese},
        {"detune", Rollers::kDetune, 0, 1023, Rollers::kModeSubReese},
        {"crunch", Rollers::kCrunch, 0, 100, Rollers::kModeSubReese},
    };
    for (const Sweep &s : sweeps) {
      float ref = 0;
      for (int v : {s.lo, s.hi}) {
        g.init(1); setAllG(g, 512, 450, 0, 0, 0, 3, 0, 25, 0, s.mode); g.setParam(s.id, v);
        ref = std::fmax(ref, maxStep(renderG(g, 33, 0.5f)));
      }
      g.init(1); setAllG(g, 512, 450, 0, 0, 0, 3, 0, 25, 0, s.mode);
      const float notes[4] = {33, 40, 45, 28};
      auto x = renderG(g, notes, 1.f, 0, [&](float p) { g.setParam(s.id, int(s.lo + (s.hi - s.lo) * p)); });
      const float d = maxStep(x);
      result(d <= ref * 1.2f, "lean zipper %-14s max step %.4f (static %.4f)", s.name, d, ref);
    }
  }
  // mode / voices switching
  {
    float ref = 0;
    for (int m = 0; m < Rollers::kNumModes; ++m) {
      g.init(1); setAllG(g, 512, 450, 0, 0, 0, 3, 0, 25, 0, m);
      ref = std::fmax(ref, maxStep(renderG(g, 33, 0.3f)));
    }
    g.init(1); setAllG(g, 512, 450, 0, 0, 0, 3, 0, 25, 0, Rollers::kModeSub);
    const float notes[4] = {33, 40, 45, 28};
    auto x = renderG(g, notes, 2.f, 0, [&](float p) {
      const int step = int(p * 8);
      g.setParam(Rollers::kMode, step % Rollers::kNumModes);
      g.setParam(Rollers::kVoices, 2 + step % 3);
    });
    const float d = maxStep(x);
    result(d <= ref * 1.15f, "lean mode/voices switching max step %.4f (static %.4f)", d, ref);
  }
  // retrigger: steal (keeps phase) and fresh allocation on a silent lane must not click
  {
    g.init(1);
    setAllG(g, 512, 450, Rollers::kSine, 30, 30, 3, 30, 30, 0, Rollers::kModeSubReese);
    const bool live[4] = {true, true, true, true};
    for (int j = 0; j < 4; ++j) { g.setPitch(j, noteW0(33)); g.noteOn(j, true); }
    std::vector<float> x(48000);
    r4f buf[64];
    for (size_t i = 0; i < x.size(); i += 64) {
      if (i == 64 * 300) { g.setPitch(0, noteW0(36)); g.noteOn(0, false); }  // steal / legato
      g.render(live, buf, 64);
      for (int s = 0; s < 64; ++s) x[i + s] = buf[s][0];
    }
    float ref = 0;
    for (size_t i = 64 * 150; i < 64 * 299; ++i) ref = std::fmax(ref, std::fabs(x[i] - x[i - 1]));
    float d = 0;
    for (size_t i = 64 * 300; i < 64 * 304; ++i) d = std::fmax(d, std::fabs(x[i] - x[i - 1]));
    result(d <= ref * 1.6f, "lean steal retrigger step %.4f (steady %.4f)", d, ref);
  }
  // silent lanes output exactly zero and don't disturb live lanes
  {
    g.init(1);
    setAllG(g, 512, 450, 0, 0, 0, 3, 30, 25, 0, Rollers::kModeHoover);
    const bool live[4] = {true, false, true, false};
    for (int j = 0; j < 4; ++j) { g.setPitch(j, noteW0(33 + j)); g.noteOn(j, true); }
    r4f buf[64];
    float silent = 0, loud = 0;
    bool bad = false;
    for (int b = 0; b < 400; ++b) {
      g.render(live, buf, 64);
      for (int s = 0; s < 64; ++s) {
        silent = std::fmax(silent, std::fabs(buf[s][1]) + std::fabs(buf[s][3]));
        loud = std::fmax(loud, std::fabs(buf[s][0]));
        for (int j = 0; j < 4; ++j) bad |= !std::isfinite(buf[s][j]);
      }
    }
    result(silent == 0.f && loud > 0.1f && !bad, "lean idle lanes silent (%.3g), live lanes play (%.3f)", silent, loud);
  }
  // level close to the NTS-1 engine for each starter setting
  for (const Preset &p : kPresets) {
    Rollers r; r.init(7);
    setAll(r, p.blend, p.detune, p.shape, p.drop, p.dropt, p.voices, p.drift, p.drive, p.crunch, p.mode);
    const float a = stats(render(r, 36, 1.f), kSR / 4).rms;
    g.init(7);
    setAllG(g, p.blend, p.detune, p.shape, p.drop, p.dropt, p.voices, p.drift, p.drive, p.crunch, p.mode);
    const float b = stats(renderG(g, 36, 1.f), kSR / 4).rms;
    const float db = 20 * std::log10(b / a);
    result(std::fabs(db) < 2.f, "lean vs NTS-1 level %-14s %+.2f dB", p.name, db);
  }
  // demo riffs from the lean renderer
  {
    const float riff[] = {33, 33, 36, 31, 33, 33, 38, 36};
    for (const Preset &p : kPresets) {
      g.init(0xC0FFEEu);
      setAllG(g, p.blend, p.detune, p.shape, p.drop, p.dropt, p.voices, p.drift, p.drive, p.crunch, p.mode);
      std::vector<float> x;
      const int step = int(kSR * 60.0 / 170.0 / 2.0);
      const bool live[4] = {true, false, false, false};
      r4f buf[64];
      for (float n : riff) {
        g.setPitch(0, noteW0(n));
        g.noteOn(0, false);
        for (int i = 0; i < step * 2; i += 64) {
          g.render(live, buf, 64);
          for (int s = 0; s < 64; ++s) x.push_back(buf[s][0]);
        }
      }
      char n[64]; snprintf(n, sizeof n, "lean preset %s", p.name);
      check(n, x);
      writeWav(std::string("test/out/mk2_preset_") + p.name + ".wav", x);
    }
  }
  // cost: 8 voices (2 groups) worst case vs 8 scalar engines
  {
    const int blocks = 48000 * 10 / 64;
    std::vector<float> buf(64);
    Rollers rs[8];
    for (int v = 0; v < 8; ++v) {
      rs[v].init(v + 1);
      setAll(rs[v], 600, 600, Rollers::kFolded, 50, 50, 4, 80, 80, 80, Rollers::kModeHoover);
      rs[v].setPitch(noteW0(36 + v)); rs[v].noteOn();
    }
    auto t0 = std::chrono::steady_clock::now();
    float acc = 0;
    for (int b = 0; b < blocks; ++b) for (int v = 0; v < 8; ++v) { rs[v].process(buf.data(), 64); acc += buf[0]; }
    auto t1 = std::chrono::steady_clock::now();
    RollersGroup4 gg[2];
    const bool live[4] = {true, true, true, true};
    for (int k = 0; k < 2; ++k) {
      gg[k].init(k + 1);
      setAllG(gg[k], 600, 600, Rollers::kFolded, 50, 50, 4, 80, 80, 80, Rollers::kModeHoover);
      for (int j = 0; j < 4; ++j) { gg[k].setPitch(j, noteW0(36 + j)); gg[k].noteOn(j, true); }
    }
    r4f ob[64];
    auto t2 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) for (int k = 0; k < 2; ++k) { gg[k].render(live, ob, 64); acc += ob[0][0]; }
    auto t3 = std::chrono::steady_clock::now();
    const double sc = std::chrono::duration<double>(t1 - t0).count(), ln = std::chrono::duration<double>(t3 - t2).count();
    printf("8 voices worst case: scalar %.1f ns/sample, lean %.1f ns/sample (%.1fx faster)%s\n",
           sc * 1e9 / (blocks * 64.0), ln * 1e9 / (blocks * 64.0), sc / ln, acc > 1e9 ? "" : "");
  }
}

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

  testLean();

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
