/*
 *  File: unit.cc
 *
 *  Rollers - logue SDK callbacks for the microKORG 2 (polyphonic)
 *
 *  One Rollers instance per voice, all sharing the same parameters. Voice
 *  events trigger the per-voice pitch drop; the virtual patch can modulate
 *  Blend, Detune, Drive and Crunch per voice. Buffer layout, mod data access
 *  and voice events follow Korg's microkorg2/waves and vox examples.
 */

#include "unit.h"
#include "runtime.h"
#include "unit_osc.h"
#include "utils/io_ops.h"
#include "utils/mk2_utils.h"
#include "rollers.h"

namespace {

enum { kDestBlend = 0, kDestDetune, kDestDrive, kDestCrunch, kNumDest };
enum { kNumUnitParams = 13 };

// Per-voice output trim: voices are summed by the synth (Korg's waves uses 0.3)
constexpr float kVoiceTrim = 0.35f;

Rollers s_voices[kMk2MaxVoices];
float s_mod[kNumDest][kMk2MaxVoices];
int32_t s_cached[kNumUnitParams];
unit_runtime_desc_t s_desc;

float s_tmp[kMk2BufferSize];

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
  for (int v = 0; v < kMk2MaxVoices; ++v) {
    s_voices[v].init(0x9E3779B9u * (uint32_t)(v + 1));  // different drift per voice
    for (int d = 0; d < kNumDest; ++d) s_mod[d][v] = 0.f;
  }
  for (int id = 0; id < kNumUnitParams; ++id) {
    s_cached[id] = static_cast<int32_t>(unit_header.params[id].init);
    if (id < Rollers::kNumParams)
      for (int v = 0; v < kMk2MaxVoices; ++v) s_voices[v].setParam(id, s_cached[id]);
  }
  return k_unit_err_none;
}

__attribute__((used)) void unit_teardown() {}

__attribute__((used)) void unit_reset() {
  for (int v = 0; v < kMk2MaxVoices; ++v) s_voices[v].reset();
}

__attribute__((used)) void unit_resume() {}

__attribute__((used)) void unit_suspend() {}

__attribute__((used)) void unit_render(const float *in, float *out, uint32_t frames) {
  (void)in;
  const unit_runtime_osc_context_t *ctx = context();
  const int n = voiceCount(ctx);

  for (int v = 0; v < n; ++v) {
    // Every voice renders every block (the synth's amp envelope does the
    // gating), and voice v writes lane v%4 of its group of four, as in Korg's
    // waves example and VectorFM.
    const uint32_t lane = (uint32_t)(v & 3);
    const uint32_t offset = GetBufferOffset(ctx, v, frames);

    Rollers &voice = s_voices[v];
    // ctx->pitch is a fractional MIDI note number per voice
    float note = ctx->pitch[v];
    if (!finiteBits(note) || note < 0.f || note > 140.f) note = 60.f;
    const uint8_t nn = static_cast<uint8_t>(note);
    const float frac = note - static_cast<float>(nn);
    voice.setPitch(osc_w0f_for_note(nn, static_cast<uint8_t>(frac * 255.f)));
    voice.setMod(s_mod[kDestBlend][v], s_mod[kDestDetune][v], s_mod[kDestDrive][v], s_mod[kDestCrunch][v]);

    uint32_t done = 0;
    while (done < frames) {
      const uint32_t len = (frames - done) < (uint32_t)kMk2BufferSize ? (frames - done) : (uint32_t)kMk2BufferSize;
      voice.process(s_tmp, len);
      for (uint32_t i = 0; i < len; ++i)
        write_oscillator_output_x1(out, s_tmp[i] * kVoiceTrim, offset, ctx->outputStride, done + i, lane);
      done += len;
    }
  }
}

__attribute__((used)) void unit_set_param_value(uint8_t id, int32_t value) {
  if (id >= kNumUnitParams) return;
  value = clipminmaxi32(unit_header.params[id].min, value, unit_header.params[id].max);
  s_cached[id] = value;
  if (id < Rollers::kNumParams)
    for (int v = 0; v < kMk2MaxVoices; ++v) s_voices[v].setParam(id, value);
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
      return Rollers::modeName(value);
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

// Note events per voice: fire the pitch drop / hoover dip on each new note.
// Used only as a trigger; rendering never depends on them.
__attribute__((used)) void unit_osc_voice_event(uint8_t event, uint8_t voice, uint8_t note, uint8_t velocity) {
  (void)note;
  (void)velocity;
  if (voice >= kMk2MaxVoices) return;
  if (event == k_voice_event_allocation || event == k_voice_event_steal) s_voices[voice].noteOn();
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
