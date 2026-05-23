#include "apm.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <rnnoise.h>
#include <speex/speex_echo.h>
}

namespace voice {

namespace {
constexpr float kSampleRate = 48000.0f;
constexpr float kPi         = 3.14159265358979323846f;
// AEC filter tail in samples. 100ms @ 48kHz covers typical WASAPI
// playback latency (~10–50ms) with margin for room-acoustics reverb.
// Longer tails cost CPU; this is a good speech tradeoff.
constexpr int   kAecFrameSize    = 960;     // 20ms @ 48kHz
constexpr int   kAecFilterLength = 48000 / 10;  // 100ms

inline float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float DbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

inline float LinearToDb(float lin) {
    if (lin <= 1e-9f) return -180.0f;
    return 20.0f * std::log10(lin);
}
}  // namespace

Apm::Apm() {
    DesignHpf(80.0f, kSampleRate);
    // RNNoise denoiser. cpuimage fork bakes the trained weights into
    // rnn_data.c, so rnnoise_create() takes no model argument.
    rnn_state_ = rnnoise_create();
    // Speex DSP echo canceller. 20ms frames, 100ms adaptive-filter tail.
    SpeexEchoState* es = speex_echo_state_init(kAecFrameSize, kAecFilterLength);
    if (es) {
        int sr = 48000;
        speex_echo_ctl(es, SPEEX_ECHO_SET_SAMPLING_RATE, &sr);
        aec_state_ = es;
    }
}

Apm::~Apm() {
    if (rnn_state_) {
        rnnoise_destroy(static_cast<DenoiseState*>(rnn_state_));
        rnn_state_ = nullptr;
    }
    if (aec_state_) {
        speex_echo_state_destroy(static_cast<SpeexEchoState*>(aec_state_));
        aec_state_ = nullptr;
    }
}

void Apm::Configure(const ApmConfig& cfg) {
    cfg_ = cfg;
}

void Apm::Reset() {
    hpf_z1_ = hpf_z2_ = 0.0f;
    noise_floor_ = 1e-4f;
    ns_envelope_ = 1.0f;
    agc_gain_    = 1.0f;
    // Re-create the RNNoise state to flush its internal adaptive
    // statistics — happens on device change so we don't carry over
    // the old room's noise profile.
    if (rnn_state_) {
        rnnoise_destroy(static_cast<DenoiseState*>(rnn_state_));
    }
    rnn_state_ = rnnoise_create();
    // Speex AEC: clear the adaptive filter so we start clean.
    if (aec_state_) {
        speex_echo_state_reset(static_cast<SpeexEchoState*>(aec_state_));
    }
}

// 2nd-order Butterworth high-pass at `cutoff_hz`. Coefficients per the
// standard biquad-cookbook derivation. We pick a moderate Q (0.7071)
// so the response is flat in the passband with no resonant peak.
void Apm::DesignHpf(float cutoff_hz, float sample_rate) {
    float w0 = 2.0f * kPi * cutoff_hz / sample_rate;
    float cos_w0 = std::cos(w0);
    float sin_w0 = std::sin(w0);
    float Q = 0.7071f;
    float alpha = sin_w0 / (2.0f * Q);

    float b0 =  (1.0f + cos_w0) / 2.0f;
    float b1 = -(1.0f + cos_w0);
    float b2 =  (1.0f + cos_w0) / 2.0f;
    float a0 =  1.0f + alpha;
    float a1 = -2.0f * cos_w0;
    float a2 =  1.0f - alpha;

    hpf_b0_ = b0 / a0;
    hpf_b1_ = b1 / a0;
    hpf_b2_ = b2 / a0;
    hpf_a1_ = a1 / a0;
    hpf_a2_ = a2 / a0;
}

float Apm::ProcessFrame(int16_t* pcm, uint32_t samples,
                        const int16_t* aec_ref) {
    if (samples == 0) return 0.0f;

    // ---- AEC (Speex DSP MDF) ----
    // Cancels the far-end playback signal out of the mic before any
    // other DSP touches it. Requires the ref frame to be roughly time-
    // aligned (within the filter tail, ~100ms); Speex's adaptive
    // filter handles the residual skew.
    if (cfg_.aec_enabled && aec_state_ != nullptr && aec_ref != nullptr
            && samples == (uint32_t)kAecFrameSize) {
        int16_t out_buf[kAecFrameSize];
        speex_echo_cancellation(static_cast<SpeexEchoState*>(aec_state_),
                                pcm, aec_ref, out_buf);
        std::memcpy(pcm, out_buf, samples * sizeof(int16_t));
    }

    // ---- HPF (transposed direct-form II) ----
    if (cfg_.hpf_enabled) {
        for (uint32_t i = 0; i < samples; ++i) {
            float x = pcm[i] * (1.0f / 32768.0f);
            float y = hpf_b0_ * x + hpf_z1_;
            hpf_z1_ = hpf_b1_ * x - hpf_a1_ * y + hpf_z2_;
            hpf_z2_ = hpf_b2_ * x - hpf_a2_ * y;
            float scaled = y * 32768.0f;
            if      (scaled >  32767.0f) scaled =  32767.0f;
            else if (scaled < -32768.0f) scaled = -32768.0f;
            pcm[i] = (int16_t)scaled;
        }
    }

    // ---- Compute frame RMS ----
    double acc = 0.0;
    for (uint32_t i = 0; i < samples; ++i) {
        float s = pcm[i] * (1.0f / 32768.0f);
        acc += s * s;
    }
    float frame_rms = (float)std::sqrt(acc / samples);
    float frame_db  = LinearToDb(frame_rms);

    // ---- Noise suppressor ----
    // Primary path: RNNoise (xiph/rnnoise) — neural network NS.
    // Operates on 480-sample chunks at 48kHz; our frame is 960 (20ms),
    // so we invoke it twice. Input/output is float, scaled to the
    // standard int16 amplitude range [-32768, 32767]. The returned
    // VAD probability is ignored for now — could feed a future
    // transmit-gate.
    //
    // Fallback path (rnn_state_ == nullptr): legacy RMS gate that
    // ships shipped before the RNN integration. Kept so the DLL still
    // ships sound suppression even if rnnoise_create failed (OOM,
    // model load error).
    if (cfg_.ns_enabled && rnn_state_ != nullptr && samples == 960) {
        DenoiseState* st = static_cast<DenoiseState*>(rnn_state_);
        float chunk[480];
        for (int half = 0; half < 2; ++half) {
            int16_t* p = pcm + half * 480;
            for (int i = 0; i < 480; ++i) chunk[i] = (float)p[i];
            rnnoise_process_frame(st, chunk, chunk);
            for (int i = 0; i < 480; ++i) {
                float v = chunk[i];
                if      (v >  32767.0f) v =  32767.0f;
                else if (v < -32768.0f) v = -32768.0f;
                p[i] = (int16_t)v;
            }
        }
        // Recompute RMS post-denoise so AGC reacts to the cleaned signal.
        double acc2 = 0.0;
        for (uint32_t i = 0; i < samples; ++i) {
            float s = pcm[i] * (1.0f / 32768.0f);
            acc2 += s * s;
        }
        frame_rms = (float)std::sqrt(acc2 / samples);
    } else if (cfg_.ns_enabled) {
        // Legacy RMS gate (only reached if rnnoise init failed).
        float attack  = 0.99f;
        float release = 0.20f;
        if (frame_rms < noise_floor_) {
            noise_floor_ = release * noise_floor_ + (1.0f - release) * frame_rms;
        } else {
            noise_floor_ = attack  * noise_floor_ + (1.0f - attack)  * frame_rms;
        }
        float ns_thresh = std::max(DbToLinear(cfg_.ns_threshold_db),
                                   noise_floor_ * 2.0f);
        float target = (frame_rms < ns_thresh) ? cfg_.ns_attenuation : 1.0f;
        float env_attack = 0.5f;
        ns_envelope_ = env_attack * ns_envelope_ + (1.0f - env_attack) * target;
        if (ns_envelope_ < cfg_.ns_attenuation) ns_envelope_ = cfg_.ns_attenuation;
        if (ns_envelope_ < 0.999f) {
            for (uint32_t i = 0; i < samples; ++i) {
                pcm[i] = (int16_t)(pcm[i] * ns_envelope_);
            }
            frame_rms *= ns_envelope_;
        }
    }

    // ---- AGC ----
    if (cfg_.agc_enabled && frame_rms > 1e-4f) {
        float target_lin = DbToLinear(cfg_.agc_target_dbfs);
        float desired    = target_lin / frame_rms;
        desired = Clamp(desired, cfg_.agc_min_gain, cfg_.agc_max_gain);

        // Slow attack (gain rises slowly to avoid pumping), faster
        // release (gain drops quickly so loud bursts don't keep us
        // attenuated). Speech sounds natural under these constants.
        float attack  = 0.05f;
        float release = 0.30f;
        float a = (desired > agc_gain_) ? attack : release;
        agc_gain_ = (1.0f - a) * agc_gain_ + a * desired;

        if (std::abs(agc_gain_ - 1.0f) > 1e-3f) {
            for (uint32_t i = 0; i < samples; ++i) {
                float v = pcm[i] * agc_gain_;
                if      (v >  32767.0f) v =  32767.0f;
                else if (v < -32768.0f) v = -32768.0f;
                pcm[i] = (int16_t)v;
            }
            frame_rms *= agc_gain_;
        }
    }

    return frame_rms;
}

}  // namespace voice
