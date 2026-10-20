/*
 *  File: header.c
 *
 *  Rollers - NTS-1 mkII oscillator unit header
 */

#include "unit_osc.h"

const __unit_header unit_header_t unit_header = {
    .header_size = sizeof(unit_header_t),
    .target = UNIT_TARGET_PLATFORM | k_unit_module_osc,
    .api = UNIT_API_VERSION,
    .dev_id = 0x5477476EU,  // 'TwGn' (TwistedGenetics), same as VectorFM / Formant Mutator
    .unit_id = 0x3U,        // 1 = VectorFM, 2 = Formant Mutator, 3 = Rollers
    .version = 0x00010000U,  // 1.0.0
    .name = "Rollers",
    .num_params = 10,
    .params = {
        // min, max, center, default, type, frac, frac mode, reserved, name
        // Knob A / Knob B
        {0, 1023, 0, 560, k_unit_param_type_none, 0, 0, 0, {"BLEND"}},
        {0, 1023, 0, 410, k_unit_param_type_none, 0, 0, 0, {"DETUNE"}},
        // Edit 1-8
        {0, 2, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"SUB SHAPE"}},
        {0, 100, 0, 20, k_unit_param_type_percent, 0, 0, 0, {"PITCH DROP"}},
        {0, 100, 0, 30, k_unit_param_type_percent, 0, 0, 0, {"DROP TIME"}},
        {2, 4, 0, 3, k_unit_param_type_none, 0, 0, 0, {"VOICES"}},
        {0, 100, 0, 30, k_unit_param_type_percent, 0, 0, 0, {"DRIFT RATE"}},
        {0, 100, 0, 25, k_unit_param_type_percent, 0, 0, 0, {"DRIVE"}},
        {0, 100, 0, 0, k_unit_param_type_percent, 0, 0, 0, {"CRUNCH"}},
        {0, 3, 0, 2, k_unit_param_type_strings, 0, 0, 0, {"MODE"}}},
};
