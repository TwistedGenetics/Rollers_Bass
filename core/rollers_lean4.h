#pragma once
/*
 * Rollers - lean 4-voice renderer for the microKORG 2
 *
 * The microKORG 2 runs oscillators polyphonically (up to 8 voices) on a
 * Cortex-A7, and the time an oscillator unit gets per buffer is tight and
 * undocumented: the full scalar engine (rollers.h) run once per voice is too
 * heavy and crackles. This renderer follows the cost profile of Korg's own
 * microkorg2/vox example:
 *
 *   - voices are rendered four at a time, one per SIMD lane (GCC vector
 *     extensions: NEON on the synth, SSE in the desktop tests)
 *   - all control maths (pitch drop, hoover dip, detune, drift, PWM, filter
 *     coefficients, drive / crunch settings) runs once per buffer per voice;
 *     the per-sample loop only steps linear ramps
 *   - saws are DPW (differentiated parabolic waveform, as in vox): branch-
 *     free, a few operations each, instead of polyBLEP
 *   - shapers are cubic soft clips (no divides), sines are parabolic
 *
 * Same controls, modes and signal flow as rollers.h (sub stays clean, the
 * upper band is high-passed, driven and crunched); small differences in
 * waveform detail. See test/render.cc for the checks.
 */

#include <stdint.h>
#include <string.h>
#include "rollers.h"

namespace {

typedef float r4f __attribute__((vector_size(16)));
typedef int r4i __attribute__((vector_size(16)));  // type of a float-vector comparison
typedef uint32_t r4u __attribute__((vector_size(16)));

#define R4_INLINE inline __attribute__((always_inline))

namespace r4 {

static R4_INLINE r4f splat(float v) { return (r4f){v, v, v, v}; }
static R4_INLINE r4f sel(r4i m, r4f a, r4f b) { return m ? a : b; }
static R4_INLINE r4f absv(r4f x) {
  r4u b;
  memcpy(&b, &x, 16);
  b &= 0x7FFFFFFFu;
  memcpy(&x, &b, 16);
  return x;
}
static R4_INLINE r4f minv(r4f a, r4f b) { return sel(a < b, a, b); }
static R4_INLINE r4f maxv(r4f a, r4f b) { return sel(a > b, a, b); }
static R4_INLINE r4f clamp(r4f x, float lo, float hi) { return minv(maxv(x, splat(lo)), splat(hi)); }
// single wrap into [0, 1) for -1 < t < 2
static R4_INLINE r4f wrap1(r4f t) {
  t = sel(t >= 1.f, t - 1.f, t);
  return sel(t < 0.f, t + 1.f, t);
}
// cubic soft clip: unity slope at 0, flat at +-1.5 (-> +-1)
static R4_INLINE r4f softclip(r4f x) {
  x = clamp(x, -1.5f, 1.5f);
  return x - 0.148148148f * x * x * x;
}
static inline float softclip1(float x) {
  x = x < -1.5f ? -1.5f : (x > 1.5f ? 1.5f : x);
  return x - 0.148148148f * x * x * x;
}
// sin(2 pi t) for t in [0, 1): refined parabola (~0.1 % error)
static R4_INLINE r4f sin2pi(r4f t) {
  const r4f y = 2.f * t - 1.f;
  const r4f s0 = 4.f * y - 4.f * y * absv(y);
  const r4f s = 0.225f * (s0 * absv(s0) - s0) + s0;
  return -s;
}
// triangle, 0 at t = 0, +1 at t = 0.25 (same phase as the sine)
static R4_INLINE r4f tri(r4f t) {
  const r4f x = wrap1(t + 0.25f);
  return 1.f - 4.f * absv(x - 0.5f);
}
// transparent below 0.8, soft knee above, never exceeds 1
static R4_INLINE r4f limit(r4f x) {
  const r4f a = absv(x);
  const r4f knee = 0.8f + 0.2f * softclip((a - 0.8f) * 3.f);
  const r4f l = sel(x < 0.f, -knee, knee);
  return sel(a <= 0.8f, x, l);
}
// truncate toward zero, per lane (float -> int -> float)
static R4_INLINE r4f trunc(r4f x) {
  r4f r;
  for (int j = 0; j < 4; ++j) r[j] = (float)(int32_t)x[j];
  return r;
}

}  // namespace r4

class RollersGroup4 {
 public:
  static constexpr int kMaxSaws = Rollers::kMaxVoices;

  void init(uint32_t seed) {
    rng_ = seed ? seed : 0x1234567u;
    p_blend_ = 0.55f;
    p_detune_ = 0.4f;
    p_drop_ = 0.2f;
    p_drop_time_ = 0.3f;
    p_drift_ = 0.3f;
    p_drive_ = 0.25f;
    p_crunch_ = 0.f;
    shape_ = Rollers::kSine;
    nv_ = want_nv_ = 3;
    mode_ = want_mode_ = Rollers::kModeSubReese;
    fade_in_ = false;
    first_ = true;
    for (int j = 0; j < 4; ++j) {
      w0_[j] = 55.f * rmath::kInvSR;
      drop_env_[j] = hoov_env_[j] = 0.f;
      mod_blend_[j] = mod_detune_[j] = mod_drive_[j] = mod_crunch_[j] = 0.f;
      reseed_[j] = true;
      for (int k = 0; k < kMaxSaws; ++k) {
        ph_[k][j] = rand01();
        pw_ph_[j][k] = rand01();
        drift_t_[j][k] = rand01();
        drift_a_[j][k] = rand11();
        drift_b_[j][k] = rand11();
        ratio_z_[j][k] = 1.f;
        pw_z_[j][k] = 0.5f;
      }
    }
    resetState();
  }

  // Clear filter / sample-and-hold state (all lanes)
  void resetState() {
    const r4f z = r4::splat(0.f);
    sub_ph_ = z;
    hp1_ = hp2_ = dcx_ = dcy_ = lp_ = hval_ = z;
    hacc_ = r4::splat(1.f);
    for (int k = 0; k < kMaxSaws; ++k) z_[k] = z2_[k] = z;
    for (int j = 0; j < 4; ++j) reseed_[j] = true;
  }

  void setParam(int id, int32_t value) {
    switch (id) {
      case Rollers::kBlend: p_blend_ = value * (1.f / 1023.f); break;
      case Rollers::kDetune: p_detune_ = value * (1.f / 1023.f); break;
      case Rollers::kSubShape: shape_ = rmath::clampi(value, 0, Rollers::kNumShapes - 1); break;
      case Rollers::kDrop: p_drop_ = value * 0.01f; break;
      case Rollers::kDropTime: p_drop_time_ = value * 0.01f; break;
      case Rollers::kVoices: want_nv_ = rmath::clampi(value, Rollers::kMinVoices, Rollers::kMaxVoices); break;
      case Rollers::kDrift: p_drift_ = value * 0.01f; break;
      case Rollers::kDrive: p_drive_ = value * 0.01f; break;
      case Rollers::kCrunch: p_crunch_ = value * 0.01f; break;
      case Rollers::kMode: want_mode_ = rmath::clampi(value, 0, Rollers::kNumModes - 1); break;
      default: break;
    }
  }

  // lane: 0..3. w0 in cycles per sample.
  void setPitch(int lane, float w0) { w0_[lane] = w0; }
  void setMod(int lane, float blend, float detune, float drive, float crunch) {
    mod_blend_[lane] = blend;
    mod_detune_[lane] = detune;
    mod_drive_[lane] = drive;
    mod_crunch_[lane] = crunch;
  }

  // New note on a lane. fresh = the voice was silent before (allocation):
  // restart the sub at zero phase. A stolen / legato voice keeps its phase so
  // the retrigger doesn't click; the pitch drop restarts either way.
  void noteOn(int lane, bool fresh) {
    if (fresh) {
      sub_ph_[lane] = 0.f;
      hp1_[lane] = hp2_[lane] = dcx_[lane] = dcy_[lane] = lp_[lane] = 0.f;
      reseed_[lane] = true;
    }
    drop_env_[lane] = 1.f;
    hoov_env_[lane] = 1.f;
  }

  // Render n (<= 64) samples for four lanes; out[s][j] = lane j.
  // Lanes with live[j] == false output silence and skip the control work.
  void render(const bool live[4], r4f *out, uint32_t n) {
    using namespace rmath;
    if (n == 0) return;
    const float inv_n = 1.f / (float)n;
    const float fn = (float)n;

    if (first_) {
      mode_ = want_mode_;
      nv_ = want_nv_;
    }
    // Pending MODE / VOICES change: fade this buffer out, switch afterwards
    const bool switching = want_mode_ != mode_ || want_nv_ != nv_;
    float fade = 1.f, d_fade = 0.f;
    if (switching) {
      d_fade = -inv_n;
    } else if (fade_in_) {
      fade = 0.f;
      d_fade = inv_n;
      fade_in_ = false;
    }

    const int mode = mode_;
    const int nv = nv_;
    const bool has_sub = mode != Rollers::kModeReese;
    const bool has_upper = mode != Rollers::kModeSub;
    const bool hoover = mode == Rollers::kModeHoover;
    const bool split = has_sub && has_upper;
    const bool body = mode == Rollers::kModeSub;

    // ---- shared (per buffer) ----
    const float drop_oct = p_drop_ * 2.f;
    const float tau = 0.005f * pow2(p_drop_time_ * 7.3f);
    const float drop_kn = pow2(-1.442695f * fn * kInvSR / tau);   // env decay over this buffer
    const float hoov_kn = pow2(-1.442695f * fn * kInvSR / 0.07f);  // 70 ms
    const float drift = p_drift_;
    const float drift_hz = drift > 0.f ? 0.05f * pow2(drift * 6.3f) : 0.f;
    const float pwm_hz = 0.25f + drift_hz * 0.5f;
    const float upper_norm = hoover ? (nv == 2 ? 0.42f : (nv == 3 ? 0.34f : 0.29f))
                                    : (nv == 2 ? 0.62f : (nv == 3 ? 0.52f : 0.45f));

    // ---- per lane (per buffer) -> start values + per-sample increments ----
    r4f wsub, dwsub;
    r4f dt[kMaxSaws], ddt[kMaxSaws], kk[kMaxSaws], dkk[kMaxSaws], pw[kMaxSaws], dpw[kMaxSaws];
    r4f hpA1, hpA2, hpA3, tone;
    r4f blend, dblend, bodyk;
    r4f dg, ddg, da, dda, db, ddb;  // drive: up = a*up + b*softclip(g*up)
    r4f sg, dsg, ug, dug;           // SUB+RS / HOOVER mix gains
    r4f crate, cq, ciq, cwet, dcwet;
    r4f gain;
    bool any_drive = false, any_crunch = false;
    const r4f zero = r4::splat(0.f);
    wsub = dwsub = hpA1 = hpA2 = hpA3 = tone = blend = dblend = bodyk = zero;
    dg = ddg = da = dda = db = ddb = zero;
    sg = dsg = ug = dug = zero;
    crate = cq = ciq = cwet = dcwet = gain = zero;
    for (int k = 0; k < kMaxSaws; ++k) dt[k] = ddt[k] = kk[k] = dkk[k] = pw[k] = dpw[k] = zero;

    for (int j = 0; j < 4; ++j) {
      if (!live[j]) {
        // keep the ramps' stored values at rest; silence this lane
        blend[j] = blend_z_[j];
        continue;
      }
      gain[j] = 1.f;
      const float w0 = clampf(w0_[j], 0.f, 0.45f);

      // pitch drop (whole osc) and hoover dip, start -> end of this buffer
      const float eS = drop_env_[j];
      float eE = eS * drop_kn;
      if (eE < 1e-4f) eE = 0.f;
      drop_env_[j] = eE;
      const float pmS = (drop_oct > 0.f && eS > 0.f) ? pow2(drop_oct * eS) : 1.f;
      const float pmE = (drop_oct > 0.f && eE > 0.f) ? pow2(drop_oct * eE) : 1.f;
      const float wS = clampf(w0 * pmS, 0.f, 0.45f);
      const float wE = clampf(w0 * pmE, 0.f, 0.45f);
      wsub[j] = wS;
      dwsub[j] = (wE - wS) * inv_n;

      float huS = 1.f, huE = 1.f;
      if (hoover) {
        const float hS = hoov_env_[j];
        float hE = hS * hoov_kn;
        if (hE < 1e-4f) hE = 0.f;
        hoov_env_[j] = hE;
        huS = hS > 0.f ? pow2(-7.f / 12.f * hS) : 1.f;
        huE = hE > 0.f ? pow2(-7.f / 12.f * hE) : 1.f;
      }

      if (has_upper) {
        const float d = clampf(p_detune_ + mod_detune_[j], 0.f, 1.f);
        const float det_cents = 0.5f + 45.f * d * d;
        const float drift_cents = 2.f + 0.5f * det_cents;
        for (int k = 0; k < nv; ++k) {
          float c = det_cents * spread(nv, k);
          if (drift_hz > 0.f) {
            drift_t_[j][k] += drift_hz * (0.8f + 0.13f * k) * fn * kInvSR;
            if (drift_t_[j][k] >= 1.f) {
              drift_t_[j][k] -= (float)(int32_t)drift_t_[j][k];
              drift_a_[j][k] = drift_b_[j][k];
              drift_b_[j][k] = rand11();
            }
            const float t = drift_t_[j][k];
            const float s = t * t * (3.f - 2.f * t);
            c += drift_cents * (drift_a_[j][k] + s * (drift_b_[j][k] - drift_a_[j][k]));
          }
          const float rt = pow2(c * (1.f / 1200.f));
          const float rs = first_ ? rt : ratio_z_[j][k];
          ratio_z_[j][k] = rt;
          const float a = clampf(wS * huS * rs, 1e-5f, 0.45f);
          const float b = clampf(wE * huE * rt, 1e-5f, 0.45f);
          dt[k][j] = a;
          ddt[k][j] = (b - a) * inv_n;
          // DPW gain 1/(4 dt)
          const float ka = 0.25f / a, kb = 0.25f / b;
          kk[k][j] = ka;
          dkk[k][j] = (kb - ka) * inv_n;
          if (hoover) {
            const float pwS = pw_z_[j][k];
            pw_ph_[j][k] += pwm_hz * (1.f + 0.31f * k) * fn * kInvSR;
            pw_ph_[j][k] -= (float)(int32_t)pw_ph_[j][k];
            const float pwE = 0.5f + 0.38f * sin2pi(pw_ph_[j][k]);
            pw_z_[j][k] = pwE;
            pw[k][j] = pwS;
            dpw[k][j] = (pwE - pwS) * inv_n;
          }
        }
        if (reseed_[j]) {
          // DPW state = parabola one sample back, so the first sample is right
          for (int k = 0; k < kMaxSaws; ++k) {
            float p = ph_[k][j] - dt[k][j];
            if (p < 0.f) p += 1.f;
            const float a = 2.f * p - 1.f;
            z_[k][j] = a * a;
            float p2 = p + pw[k][j];
            if (p2 >= 1.f) p2 -= 1.f;
            const float b = 2.f * p2 - 1.f;
            z2_[k][j] = b * b;
          }
          reseed_[j] = false;
        }

        if (split) {
          // high-pass just above the (dropping) note: the sub owns the fundamental
          const float wd = 0.5f * (wS + wE);
          const float fc = clampf(wd * 1.4f, 20.f * kInvSR, 1000.f * kInvSR);
          const float x = 3.1415927f * fc;
          const float g = x * (1.f + x * x * (0.33333334f + x * x * 0.13333334f));
          const float a1 = 1.f / (1.f + g * (g + 1.4142135f));
          hpA1[j] = a1;
          hpA2[j] = g * a1;
          hpA3[j] = g * g * a1;
        }

        // drive, start -> end
        const float drS = drive_z_[j];
        const float drE = clampf(p_drive_ + mod_drive_[j], 0.f, 1.f);
        drive_z_[j] = drE;
        if (drS > 0.f || drE > 0.f) any_drive = true;
        const float gS = 1.f + drS * 9.f, gE = 1.f + drE * 9.f;
        dg[j] = gS;
        ddg[j] = (gE - gS) * inv_n;
        // (dry + wet * (sat * norm - dry)) * makeup, folded into two gains
        float aS, bS, aE, bE;
        driveGains(drS, gS, aS, bS);
        driveGains(drE, gE, aE, bE);
        da[j] = aS;
        dda[j] = (aE - aS) * inv_n;
        db[j] = bS;
        ddb[j] = (bE - bS) * inv_n;

        // crunch: rate and depth step per buffer, wet ramps
        const float crS = crunch_z_[j];
        const float crE = clampf(p_crunch_ + mod_crunch_[j], 0.f, 1.f);
        crunch_z_[j] = crE;
        if (crS > 0.001f || crE > 0.001f) any_crunch = true;
        crate[j] = pow2(-crE * 3.6f);
        cq[j] = pow2(15.f - crE * 11.f);
        ciq[j] = 1.f / cq[j];
        const float cwS = clampf(crS * 20.f, 0.f, 1.f), cwE = clampf(crE * 20.f, 0.f, 1.f);
        cwet[j] = cwS;
        dcwet[j] = (cwE - cwS) * inv_n;
      }

      // blend (and the REESE tone / SUB body that Knob A drives)
      const float bS = first_ ? clampf(p_blend_ + mod_blend_[j], 0.f, 1.f) : blend_z_[j];
      const float bE = clampf(p_blend_ + mod_blend_[j], 0.f, 1.f);
      blend_z_[j] = bE;
      blend[j] = bS;
      dblend[j] = (bE - bS) * inv_n;
      // mix gains: sub fades out over the top half of Knob A, upper fades in over the bottom half
      const float sgS = clampf(2.f * (1.f - bS), 0.f, 1.f) * 0.48f, sgE = clampf(2.f * (1.f - bE), 0.f, 1.f) * 0.48f;
      const float ugS = clampf(2.f * bS, 0.f, 1.f) * 0.6f, ugE = clampf(2.f * bE, 0.f, 1.f) * 0.6f;
      sg[j] = sgS;
      dsg[j] = (sgE - sgS) * inv_n;
      ug[j] = ugS;
      dug[j] = (ugE - ugS) * inv_n;
      if (mode == Rollers::kModeReese) {
        const float fc = 150.f * pow2(bE * 7.f);
        tone[j] = clampf(1.f - pow2(-9.0647f * fc * kInvSR), 0.f, 1.f);
      }
      if (body) bodyk[j] = 1.f / (1.f + 0.35f * bE);
    }
    first_ = false;

    // ---- per sample, in passes (keeps each pass inside the NEON registers) ----
    r4f *const sb = sub_buf_;
    r4f *const ub = up_buf_;

    // pass 1: SUB
    if (has_sub) {
      r4f sph = sub_ph_;
      r4f bl = blend;
      const int shape = shape_;
      for (uint32_t s = 0; s < n; ++s) {
        r4f sub;
        if (shape == Rollers::kTriangle) {
          sub = r4::tri(sph);
        } else if (shape == Rollers::kFolded) {
          const r4f a = 0.3125f * r4::sin2pi(sph);  // light fold: sin(2pi * 0.3125 sin)
          sub = r4::sin2pi(r4::sel(a < 0.f, a + 1.f, a));
        } else {
          sub = r4::sin2pi(sph);
        }
        if (body) {
          bl += dblend;
          const r4f s2 = r4::sin2pi(r4::wrap1(2.f * sph));
          sub = (sub + 0.5f * bl * s2) * bodyk;
        }
        sb[s] = sub;
        sph += wsub;
        sph = r4::sel(sph >= 1.f, sph - 1.f, sph);
        wsub += dwsub;
      }
      sub_ph_ = sph;
    }

    // pass 2: SAWS, one at a time, summed into the upper buffer
    if (has_upper) {
      for (int k = 0; k < nv; ++k) {
        const bool first_saw = k == 0;
        r4f p = ph_[k], zz = z_[k], d = dt[k], dd = ddt[k], g = kk[k], dg2 = dkk[k];
        if (hoover) {
          r4f zz2 = z2_[k], q = pw[k], dq = dpw[k];
          for (uint32_t s = 0; s < n; ++s) {
            const r4f a = 2.f * p - 1.f;
            const r4f pa = a * a;
            const r4f sw = (pa - zz) * g;
            zz = pa;
            // PWM: 1.5 saw - 0.5 saw(t + pw)
            const r4f t2 = p + q;
            const r4f b = 2.f * r4::sel(t2 >= 1.f, t2 - 1.f, t2) - 1.f;
            const r4f pb = b * b;
            const r4f sw2 = (pb - zz2) * g;
            zz2 = pb;
            const r4f v = 1.5f * sw - 0.5f * sw2;
            ub[s] = first_saw ? v : ub[s] + v;
            q += dq;
            p += d;
            p = r4::sel(p >= 1.f, p - 1.f, p);
            d += dd;
            g += dg2;
          }
          z2_[k] = zz2;
        } else {
          for (uint32_t s = 0; s < n; ++s) {
            // DPW saw: d/dt of (2t-1)^2, scaled by 1/(4 dt)
            const r4f a = 2.f * p - 1.f;
            const r4f pa = a * a;
            const r4f v = (pa - zz) * g;
            ub[s] = first_saw ? v : ub[s] + v;
            zz = pa;
            p += d;
            p = r4::sel(p >= 1.f, p - 1.f, p);
            d += dd;
            g += dg2;
          }
        }
        ph_[k] = p;
        z_[k] = zz;
      }

      // pass 3: split filter / tone, drive, crunch, DC blocker
      const float un = upper_norm;
      r4f hp1 = hp1_, hp2 = hp2_, lp = lp_;
      for (uint32_t s = 0; s < n; ++s) {
        r4f up = ub[s] * un;
        if (split) {
          // 2-pole SVF high-pass (Simper), Q = 0.707
          const r4f v3 = up - hp2;
          const r4f v1 = hpA1 * hp1 + hpA2 * v3;
          const r4f v2 = hp2 + hpA2 * hp1 + hpA3 * v3;
          hp1 = 2.f * v1 - hp1;
          hp2 = 2.f * v2 - hp2;
          up = up - 1.4142135f * v1 - v2;
        } else {
          lp += tone * (up - lp);  // REESE: Knob A is a tone control
          up = lp;
        }
        ub[s] = up;
      }
      hp1_ = flush(hp1);
      hp2_ = flush(hp2);
      lp_ = flush(lp);

      if (any_drive) {
        for (uint32_t s = 0; s < n; ++s) {
          const r4f up = ub[s];
          ub[s] = da * up + db * r4::softclip(dg * up);
          dg += ddg;
          da += dda;
          db += ddb;
        }
      }
      if (any_crunch) {
        r4f hacc = hacc_, hval = hval_;
        for (uint32_t s = 0; s < n; ++s) {
          const r4f up = ub[s];
          hacc += crate;
          const r4i m = hacc >= 1.f;
          hacc = r4::sel(m, hacc - 1.f, hacc);
          hval = r4::sel(m, r4::trunc(up * cq) * ciq, hval);
          ub[s] = up + cwet * (hval - up);
          cwet += dcwet;
        }
        hacc_ = hacc;
        hval_ = hval;
      }

      // DC blocker (~10 Hz), last: drive on the high-passed stack makes DC
      r4f dcx = dcx_, dcy = dcy_;
      for (uint32_t s = 0; s < n; ++s) {
        const r4f up = ub[s];
        const r4f y = up - dcx + 0.9987f * dcy;
        dcx = up;
        dcy = y;
        ub[s] = y;
      }
      dcx_ = flush(dcx);
      dcy_ = flush(dcy);
    }

    // pass 4: MIX + limiter
    for (uint32_t s = 0; s < n; ++s) {
      r4f y;
      if (mode == Rollers::kModeSub) {
        y = sb[s] * 0.85f;
      } else if (mode == Rollers::kModeReese) {
        y = ub[s] * 0.9f;
      } else {
        y = sg * sb[s] + ug * ub[s];
        sg += dsg;
        ug += dug;
      }
      fade += d_fade;
      // the mix leaves headroom, so a hard clamp only catches rare overs
      out[s] = r4::clamp(y, -1.f, 1.f) * (gain * fade);
    }

    if (switching) {
      mode_ = want_mode_;
      nv_ = want_nv_;
      hp1_ = hp2_ = dcx_ = dcy_ = lp_ = zero;
      for (int j = 0; j < 4; ++j) reseed_[j] = true;
      fade_in_ = true;
    }
  }

 private:
  static inline void driveGains(float drive, float g, float &a, float &b) {
    const float wet = rmath::clampf(drive * 5.f, 0.f, 1.f);
    const float norm = 1.f / r4::softclip1(g);
    const float makeup = 1.f + wet * (1.f / (1.f + 0.6f * drive) - 1.f);
    a = (1.f - wet) * makeup;
    b = wet * norm * makeup;
  }

  static inline float spread(int n, int v) {
    if (n == 2) return v == 0 ? -1.f : 1.f;
    if (n == 3) return (float)(v - 1);
    return -1.f + (float)v * (2.f / 3.f);
  }
  static R4_INLINE r4f flush(r4f x) { return r4::sel(r4::absv(x) < 1e-20f, r4::splat(0.f), x); }

  inline uint32_t rnd() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
  }
  inline float rand01() { return (float)(rnd() >> 8) * (1.f / 16777216.f); }
  inline float rand11() { return rand01() * 2.f - 1.f; }

  // shared parameters
  float p_blend_, p_detune_, p_drop_, p_drop_time_, p_drift_, p_drive_, p_crunch_;
  int shape_, nv_, want_nv_, mode_, want_mode_;
  bool fade_in_, first_;
  uint32_t rng_;

  // per-lane control state
  float w0_[4];
  float drop_env_[4], hoov_env_[4];
  float mod_blend_[4], mod_detune_[4], mod_drive_[4], mod_crunch_[4];
  float blend_z_[4] = {0.55f, 0.55f, 0.55f, 0.55f};
  float drive_z_[4] = {0.f, 0.f, 0.f, 0.f};
  float crunch_z_[4] = {0.f, 0.f, 0.f, 0.f};
  float drift_t_[4][kMaxSaws], drift_a_[4][kMaxSaws], drift_b_[4][kMaxSaws];
  float ratio_z_[4][kMaxSaws];
  float pw_ph_[4][kMaxSaws], pw_z_[4][kMaxSaws];
  bool reseed_[4];

  // per-sample state, lane-vectorised
  r4f sub_ph_;
  r4f ph_[kMaxSaws], z_[kMaxSaws], z2_[kMaxSaws];
  r4f hp1_, hp2_, dcx_, dcy_, lp_, hacc_, hval_;
  r4f sub_buf_[64], up_buf_[64];
};

}  // namespace
