// audio_io.h — thin C++ wrapper around miniaudio for capture and
// playback. Hides the miniaudio types so callers only see std/cstdint.
//
// Capture: WASAPI input device, 48kHz mono, fixed 20ms frames
// (960 samples). On each frame the on_frame_cb fires on the
// miniaudio callback thread; keep work minimal there (just push to
// a ring buffer or encode + send).
//
// Playback: WASAPI output device, 48kHz stereo (so positional pan
// works). The mixer is fed by Enqueue() per source; the audio
// thread pulls and mixes with per-source 3D attenuation +
// equal-power panning.

#pragma once

#include <cstdint>
#include <functional>

namespace voice {

// ---- Capture ---------------------------------------------------------

using CaptureCallback = std::function<void(const int16_t* mono_pcm,
                                           uint32_t samples)>;

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    // device_name = "" → default input device. Returns false on init failure.
    bool Start(const char* device_name, CaptureCallback cb);
    void Stop();
    bool IsRunning() const;

private:
    struct Impl;
    Impl* impl_;
};

// ---- Playback / 3D mixer --------------------------------------------

class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();

    // device_name = "" → default output device.
    bool Start(const char* device_name);
    void Stop();
    bool IsRunning() const;

    // Configuration for the distance attenuation curve (per source).
    // Linear from full volume at <= min_dist to 0 at >= max_dist.
    void SetAttenuation(float min_dist, float max_dist);

    // Enqueue a decoded PCM frame from a remote speaker. Thread-safe.
    // src_id identifies the speaker (=session_id from server);
    // listener-relative coordinates passed (we already computed delta
    // before calling).
    //
    // delta_x/y/z are the speaker's position MINUS the listener's,
    // in the L2 unit (cm). distance_attenuation is precomputed by
    // the caller from the same delta (avoids recomputation).
    void Enqueue(uint32_t src_id,
                 const int16_t* mono_pcm, uint32_t samples,
                 float delta_x, float delta_y, float delta_z,
                 float volume);

    // Drop a source (e.g. session disconnected). Frees its decoder
    // and mixer slot.
    void DropSource(uint32_t src_id);

    // Returns the number of sources that mixed audio in the last
    // 100ms — for the HUD "X speakers" badge.
    int ActiveSpeakers();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace voice
