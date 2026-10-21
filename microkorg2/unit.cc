/*
 *  File: unit.cc
 *
 *  Rollers - logue SDK callbacks for the microKORG 2 (polyphonic)
 *
 *  Voices are rendered four at a time, one per NEON lane, by the lean
 *  renderer in core/rollers_lean4.h (the full scalar engine run once per
 *  voice was too heavy for the microKORG 2 and crackled). Idle voices are
 *  skipped: a group of four with no sounding voice costs (almost) nothing.
 *
 *  Voice events fire the per-voice pitch drop and mark voices as sounding.
 *  Until the first voice event arrives every voice renders, so nothing goes
 *  silent if the host never sends events. The virtual patch can modulate
 *  Blend, Detune, Drive and Crunch per voice. Buffer layout, mod data access
 *  and voice events follow Korg's microkorg2/waves and vox examples.
 *
 *  Built with -DROLLERS_LOADMETER (tools/mk2-meter), the MODE display also
 *  shows the CPU load of this unit, e.g. "S+RS 37%".
 */

#include "unit.h"
#include "runtime.h"
#include "unit_osc.h"
#include "utils/io_ops.h"
#include "utils/mk2_utils.h"
#include "rollers_lean4.h"
#ifdef ROLLERS_LOADMETER
#include <stdio.h>
#include <time.h>
#endif

namespace {

enum { kDestBlend = 0, kDestDetune, kDestDrive, kDestCrunch, kNumDest };
enum { kNumUnitParams = 13, kNumGroups = kMk2MaxVoices / 4 };

// Per-voice output trim: voices are summed by the synth (Korg's waves uses 0.3)
constexpr float kVoiceTrim = 0.35f;

RollersGroup4 s_group[kNumGroups];
float s_mod[kNumDest][kMk2MaxVoices];
int32_t s_cached[kNumUnitParams];
unit_runtime_desc_t s_desc;
r4f s_out4[kMk2BufferSize];

// Voice activity from unit_osc_voice_event; idle voices output silence.
bool s_active[kMk2MaxVoices];
bool s_events_seen = false;

#ifdef ROLLERS_LOADMETER
// Time spent in unit_render as a share of the buffer's real-time length.
// Peak over ~0.5 s. 100 % = no time left (crackles).
float s_load_peak = 0.f, s_load_show = 0.f;
uint32_t s_load_frames = 0;
char s_mode_str[16];
inline double nowNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
#endif

inline const unit_runtime_osc_context_t *context() {
  return static_cast<const unit_runtime_osc_context_t *>(s_desc.hooks.runtime_context);
}

inline int voiceCount(const unit_runtime_osc_context_t *ctx) {
  const int n = ctx->voiceLimit;
  return n < 0 ? 0 : (n > kMk2MaxVoices ? kMk2MaxVoices : n);
}

// NaN/inf check on the bits: the Makefile builds with -ffast-math, which lets
// the compiler drop float NaN comparisons
inline bool finiteBits(float x) {
  union { float f; uint32_t u; } b;
  b.f = x;
  return (b.u & 0x7F800000u) != 0x7F800000u;
}

void writeName(char *dst, const char *name) {
  int i = 0;
  for (; name[i] && i < 8; ++i) dst[i] = name[i];
  dst[i] = 0;
}

}  // namespace

__attribute__((used)) int8_t unit_init(const unit_runtime_desc_t *desc) {
  if (!desc) return k_unit_err_undef;
  if (desc->target != unit_header.target) return k_unit_err_target;
  if (!UNIT_API_IS_COMPAT(desc->api)) return k_unit_err_api_version;
  if (desc->samplerate != 48000) return k_unit_err_samplerate;

  s_desc = *desc;
  s_events_seen = false;
  for (int k = 0; k < kNumGroups; ++k) s_group[k].init(0x9E3779B9u * (uint32_t)(k + 1));
  for (int v = 0; v < kMk2MaxVoices; ++v) {
    s_active[v] = false;
    for (int d = 0; d < kNumDest; ++d) s_mod[d][v] = 0.f;
  }
  for (int id = 0; id < kNumUnitParams; ++id) {
    s_cached[id] = static_cast<int32_t>(unit_header.params[id].init);
    if (id < Rollers::kNumParams)
      for (int k = 0; k < kNumGroups; ++k) s_group[k].setParam(id, s_cached[id]);
  }
  return k_unit_err_none;
}

__attribute__((used)) void unit_teardown() {}

__attribute__((used)) void unit_reset() {
  for (int k = 0; k < kNumGroups; ++k) s_group[k].resetState();
}

__attribute__((used)) void unit_resume() {}

__attribute__((used)) void unit_suspend() {}

__attribute__((used)) void unit_render(const float *in, float *out, uint32_t frames) {
  (void)in;
#ifdef ROLLERS_LOADMETER
  const double t_start = nowNs();
#endif
  const unit_runtime_osc_context_t *ctx = context();
  const int n = voiceCount(ctx);

  for (int base = 0; base < n; base += 4) {
    RollersGroup4 &grp = s_group[base >> 2];
    const int lanes = (n - base) < 4 ? (n - base) : 4;
    const uint32_t offset = GetBufferOffset(ctx, base, frames);
    bool live[4] = {false, false, false, false};
    bool any = false;
    for (int j = 0; j < lanes; ++j) {
      const int v = base + j;
      live[j] = !s_events_seen || s_active[v];
      if (!live[j]) continue;
      any = true;
      // ctx->pitch is a fractional MIDI note number per voice
      float note = ctx->pitch[v];
      if (!finiteBits(note) || note < 0.f || note > 140.f) note = 60.f;
      const uint8_t nn = static_cast<uint8_t>(note);
      const float frac = note - static_cast<float>(nn);
      grp.setPitch(j, osc_w0f_for_note(nn, static_cast<uint8_t>(frac * 255.f)));
      grp.setMod(j, s_mod[kDestBlend][v], s_mod[kDestDetune][v], s_mod[kDestDrive][v], s_mod[kDestCrunch][v]);
    }

    // Voice v writes lane v%4 of its group of four (Korg's waves, VectorFM)
    if (!any) {
      for (uint32_t i = 0; i < frames; ++i)
        for (int j = 0; j < lanes; ++j) write_oscillator_output_x1(out, 0.f, offset, ctx->outputStride, i, (uint32_t)j);
      continue;
    }
    uint32_t done = 0;
    while (done < frames) {
      const uint32_t len = (frames - done) < (uint32_t)kMk2BufferSize ? (frames - done) : (uint32_t)kMk2BufferSize;
      grp.render(live, s_out4, len);
      for (uint32_t i = 0; i < len; ++i)
        for (int j = 0; j < lanes; ++j)
          write_oscillator_output_x1(out, s_out4[i][j] * kVoiceTrim, offset, ctx->outputStride, done + i, (uint32_t)j);
      done += len;
    }
  }

#ifdef ROLLERS_LOADMETER
  const double used = nowNs() - t_start;
  const float load = (float)(used / ((double)frames * (1e9 / 48000.0)));
  if (load > s_load_peak) s_load_peak = load;
  s_load_frames += frames;
  if (s_load_frames >= 24000) {
    s_load_show = s_load_peak;
    s_load_peak = 0.f;
    s_load_frames = 0;
  }
#endif
}

__attribute__((used)) void unit_set_param_value(uint8_t id, int32_t value) {
  if (id >= kNumUnitParams) return;
  value = clipminmaxi32(unit_header.params[id].min, value, unit_header.params[id].max);
  s_cached[id] = value;
  if (id < Rollers::kNumParams)
    for (int k = 0; k < kNumGroups; ++k) s_group[k].setParam(id, value);
}

__attribute__((used)) int32_t unit_get_param_value(uint8_t id) {
  if (id >= kNumUnitParams) return 0;
  return s_cached[id];
}

__attribute__((used)) const char *unit_get_param_str_value(uint8_t id, int32_t value) {
  switch (id) {
    case Rollers::kSubShape:
      return Rollers::shapeName(value);
    case Rollers::kMode:
#ifdef ROLLERS_LOADMETER
    {
      const char *m = value == Rollers::kModeSub ? "SUB" : value == Rollers::kModeReese ? "RS" : value == Rollers::kModeSubReese ? "S+RS" : "HOOV";
      snprintf(s_mode_str, sizeof s_mode_str, "%s %d%%", m, (int)(s_load_show * 100.f + 0.5f));
      return s_mode_str;
    }
#else
      return Rollers::modeName(value);
#endif
    default:
      return nullptr;
  }
}

__attribute__((used)) const uint8_t *unit_get_param_bmp_value(uint8_t id, int32_t value) {
  (void)id;
  (void)value;
  return nullptr;
}

__attribute__((used)) void unit_set_tempo(uint32_t tempo) { (void)tempo; }

// Note events per voice: fire the pitch drop / hoover dip, track which voices sound
__attribute__((used)) void unit_osc_voice_event(uint8_t event, uint8_t voice, uint8_t note, uint8_t velocity) {
  (void)note;
  (void)velocity;
  if (voice >= kMk2MaxVoices) return;
  s_events_seen = true;
  RollersGroup4 &grp = s_group[voice >> 2];
  const int lane = voice & 3;
  switch (event) {
    case k_voice_event_allocation:
      grp.noteOn(lane, !s_active[voice]);  // fresh voice: restart the sub cleanly
      s_active[voice] = true;
      break;
    case k_voice_event_steal:
      grp.noteOn(lane, false);  // keep phase: no click
      s_active[voice] = true;
      break;
    case k_voice_event_release:
      break;  // keep rendering through the amp release
    case k_voice_event_deallocation:
      s_active[voice] = false;
      break;
    default:
      break;
  }
}

// Virtual patch: modulation data and destination names
__attribute__((used)) void unit_platform_exclusive(uint8_t messageId, void *data, uint32_t dataSize) {
  (void)dataSize;
  switch (messageId) {
    case kMk2PlatformExclusiveModData: {
      const unit_runtime_osc_context_t *ctx = context();
      const int n = voiceCount(ctx);
      const int32_t *index = GetModIndex(data);
      const float *depth = GetModDepth(data);
      for (int d = 0; d < kNumDest; ++d)
        for (int v = 0; v < kMk2MaxVoices; ++v) s_mod[d][v] = 0.f;
      for (int src = 0; src < kNumMk2ModSrc; ++src) {
        const int32_t dest = index[src];
        if (dest < 0 || dest >= kNumDest) continue;
        const float *mod = GetModSourceData(data, src, ctx->voiceLimit, 0);
        for (int v = 0; v < n; ++v) s_mod[dest][v] += mod[v] * depth[src];
      }
      break;
    }
    case kMk2PlatformExclusiveModDestName: {
      // data[0] holds the destination index; the name is written from data[1]
      char *name = GetModDestNameData(data);
      switch (name[0]) {
        case kDestBlend: writeName(name + 1, "Blend"); break;
        case kDestDetune: writeName(name + 1, "Detune"); break;
        case kDestDrive: writeName(name + 1, "Drive"); break;
        case kDestCrunch: writeName(name + 1, "Crunch"); break;
        default: break;
      }
      break;
    }
    default:
      break;
  }
}
