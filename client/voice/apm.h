// apm.h — Audio processing module for the capture pipeline.
//
// Stage order matches the WebRTC APM reference: AEC → HPF → NS → AGC.
// Each stage is independently toggleable from voice.ini so users can
// experiment.
//
// HPF — Biquad high-pass at 80 Hz. Removes pop/wind/rumble that
//       Opus encodes inefficiently and that drowns out speech.
//
// NS  — Simple noise gate with hysteresis. Tracks the long-running
//       noise floor (RMS) and attenuates frames below an adaptive
//       threshold using a soft envelope. Not as effective as RNNoise
//       for non-stationary noise (keyboards), but no model dependency
//       and adds zero perceptible latency. Designed so the API can
//       be swapped with RNNoise later — just replace NoiseSuppressor.
//
// AGC — Auto-gain target a configurable RMS dBFS. Slow attack, faster
//       release. Avoids the "everyone has to scream to be heard" or
//       "loud talker clips" experience.
//
// All stages operate on 20 ms / 960-sample mono frames at 48 kHz —
// the same shape as the rest of the pipeline.

#pragma once

#include <cstdint>

namespace voice {

struct ApmConfig {
    bool  aec_enabled = true;
    bool  hpf_enabled = true;
    bool  ns_enabled  = true;
    bool  agc_enabled = true;

    // Noise suppressor knobs.
    float ns_threshold_db = -45.0f;   // RMS floor below which we treat as noise
    float ns_attenuation  = 0.20f;    // 0..1 — how much to scale down noise (0=mute)

    // AGC knobs.
    float agc_target_dbfs = -18.0f;   // target RMS level
    float agc_max_gain    = 6.0f;     // ~+15 dB ceiling
    float agc_min_gain    = 0.25f;    // ~-12 dB floor
};

class Apm {
public:
    Apm();
    ~Apm();
    Apm(const Apm&) = delete;
    Apm& operator=(const Apm&) = delete;

    void Configure(const ApmConfig& cfg);

    // Processes 960 int16 samples in-place (20ms at 48kHz mono).
    // `aec_ref` (optional) is the time-aligned far-end playback signal
    // — pass nullptr to skip AEC for this frame. Returns the linear
    // RMS of the post-processed frame (handy for the UI level meter).
    float ProcessFrame(int16_t* pcm, uint32_t samples,
                       const int16_t* aec_ref = nullptr);

    // Resets internal state (e.g., on capture-device change).
    void Reset();

private:
    ApmConfig cfg_{};

    // Biquad HPF state.
    float hpf_b0_{}, hpf_b1_{}, hpf_b2_{};
    float hpf_a1_{}, hpf_a2_{};
    float hpf_z1_{}, hpf_z2_{};

    // RNNoise denoiser (opaque pointer to rnnoise's DenoiseState).
    // Falls back to the legacy RMS noise gate when nullptr.
    void* rnn_state_ = nullptr;

    // Speex DSP echo canceller (opaque pointer to SpeexEchoState).
    // AEC is skipped when nullptr or when ProcessFrame's aec_ref arg
    // is null.
    void* aec_state_ = nullptr;

    // Legacy RMS-gate fallback state (only used if RNNoise init fails).
    float noise_floor_{1e-4f};
    float ns_envelope_{1.0f};

    // AGC state — current applied gain (we adjust slowly).
    float agc_gain_{1.0f};

    void DesignHpf(float cutoff_hz, float sample_rate);
};

}  // namespace voice
