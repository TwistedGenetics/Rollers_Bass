/*
 *  File: unit.cc
 *
 *  Rollers - logue SDK v2 callbacks for the NTS-1 mkII (one mono voice)
 */

#include "unit_osc.h"
#include "utils/int_math.h"
#include "rollers.h"

static Rollers s_synth;
static int32_t s_cached[Rollers::kNumParams];
static const unit_runtime_osc_context_t *s_context;

#define q31_to_f32_c 4.65661287307739e-010f
#define q31_to_f32(q) ((float)(q) * q31_to_f32_c)

__unit_callback int8_t unit_init(const unit_runtime_desc_t *desc) {
  if (!desc) return k_unit_err_undef;
  if (desc->target != unit_header.target) return k_unit_err_target;
  if (!UNIT_API_IS_COMPAT(desc->api)) return k_unit_err_api_version;
  if (desc->samplerate != 48000) return k_unit_err_samplerate;
  if (desc->input_channels != 2 || desc->output_channels != 1) return k_unit_err_geometry;

  s_context = static_cast<const unit_runtime_osc_context_t *>(desc->hooks.runtime_context);

  s_synth.init();
  for (int id = 0; id < Rollers::kNumParams; ++id) {
    s_cached[id] = static_cast<int32_t>(unit_header.params[id].init);
    s_synth.setParam(id, s_cached[id]);
  }
  return k_unit_err_none;
}

__unit_callback void unit_teardown() {}

__unit_callback void unit_reset() { s_synth.reset(); }

__unit_callback void unit_resume() {}

__unit_callback void unit_suspend() {}

__unit_callback void unit_render(const float *in, float *out, uint32_t frames) {
  (void)in;
  s_synth.setPitch(osc_w0f_for_note(s_context->pitch >> 8, s_context->pitch & 0xFF));
  // The shape LFO modulates BLEND (Knob A), like the shape knob on built-in oscillators
  s_synth.setShapeLfo(q31_to_f32(s_context->shape_lfo));
  s_synth.process(out, frames);
}

__unit_callback void unit_set_param_value(uint8_t id, int32_t value) {
  if (id >= Rollers::kNumParams) return;
  value = clipminmaxi32(unit_header.params[id].min, value, unit_header.params[id].max);
  s_cached[id] = value;
  s_synth.setParam(id, value);
}

__unit_callback int32_t unit_get_param_value(uint8_t id) {
  if (id >= Rollers::kNumParams) return 0;
  return s_cached[id];
}

__unit_callback const char *unit_get_param_str_value(uint8_t id, int32_t value) {
  switch (id) {
    case Rollers::kSubShape:
      return Rollers::shapeName(value);
    case Rollers::kMode:
      return Rollers::modeName(value);
    default:
      return nullptr;
  }
}

__unit_callback void unit_note_on(uint8_t note, uint8_t velo) {
  (void)note;
  (void)velo;
  s_synth.noteOn();  // fires the pitch drop / hoover dip
}

__unit_callback void unit_note_off(uint8_t note) { (void)note; }

__unit_callback void unit_all_note_off() {}

__unit_callback void unit_set_tempo(uint32_t tempo) { (void)tempo; }

__unit_callback void unit_tempo_4ppqn_tick(uint32_t counter) { (void)counter; }

__unit_callback void unit_pitch_bend(uint16_t bend) { (void)bend; }

__unit_callback void unit_channel_pressure(uint8_t press) { (void)press; }

__unit_callback void unit_aftertouch(uint8_t note, uint8_t press) {
  (void)note;
  (void)press;
}
