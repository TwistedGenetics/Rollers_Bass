/*
 *  File: header.c
 *
 *  Rollers - microKORG 2 oscillator unit header
 *  Parameters keep the same order as the NTS-1 mkII version.
 *  Names: max 8 characters on the microKORG 2.
 */

#include "unit.h"
#include "runtime.h"

const __unit_header unit_header_t unit_header = {
    .header_size = sizeof(unit_header_t),
    .target = UNIT_TARGET_PLATFORM | k_unit_module_osc,
    .api = UNIT_API_VERSION,
    .dev_id = 0x5477476EU,  // 'TwGn' (TwistedGenetics) - same as the NTS-1 mkII build
    .unit_id = 0x3U,        // 1 = VectorFM, 2 = Formant Mutator, 3 = Rollers
    .version = 0x00010000U,  // 1.0.0
    .name = "Rollers",
    .num_presets = 0,
    .num_params = 13,
    .params = {
        // min, max, center, default, type, frac, frac mode, reserved, name

        // Page 1
        {0, 1023, 0, 560, k_unit_param_type_none, 0, 0, 0, {"Blend"}},
        {0, 1023, 0, 410, k_unit_param_type_none, 0, 0, 0, {"Detune"}},
        {0, 2, 0, 0, k_unit_param_type_strings, 0, 0, 0, {"SubShape"}},

        // Page 2
        {0, 100, 0, 20, k_unit_param_type_percent, 0, 0, 0, {"Drop"}},
        {0, 100, 0, 30, k_unit_param_type_percent, 0, 0, 0, {"DropTime"}},
        {2, 4, 0, 3, k_unit_param_type_none, 0, 0, 0, {"Voices"}},
        {0, 100, 0, 30, k_unit_param_type_percent, 0, 0, 0, {"Drift"}},
        {0, 100, 0, 25, k_unit_param_type_percent, 0, 0, 0, {"Drive"}},

        // Page 3
        {0, 100, 0, 0, k_unit_param_type_percent, 0, 0, 0, {"Crunch"}},
        {0, 3, 0, 2, k_unit_param_type_strings, 0, 0, 0, {"Mode"}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}},
        {0, 0, 0, 0, k_unit_param_type_none, 0, 0, 0, {""}}}};
