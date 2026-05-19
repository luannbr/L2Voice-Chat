// audio_io.h — miniaudio capture + playback wrappers.
//
// Capture: WASAPI input, 48 kHz mono, fixed 20 ms frames (960
// samples). on_frame_cb fires on miniaudio's audio thread; keep work
// minimal there (encode + UDP send is fine).
//
// Playback: WASAPI output, 48 kHz stereo. The mixer is fed by
// Enqueue() per source. Gain and pan are supplied by the caller
// (service-computed in protocol rev 2), so the playback path does
// not do any spatial math — just gain + equal-power pan.

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

    bool Start(const char* device_name, CaptureCallback cb);
    void Stop();
    bool IsRunning() const;

private:
    struct Impl;
    Impl* impl_;
};

// ---- Playback / mixer -----------------------------------------------

class AudioPlayback {
public:
    AudioPlayback();
    ~AudioPlayback();

    bool Start(const char* device_name);
    void Stop();
    bool IsRunning() const;

    // Enqueue a decoded PCM frame from a remote speaker. Thread-safe.
    //
    //   src_id : speaker id (= src_session_id from packet)
    //   gain   : 0..1 multiplier (already includes distance falloff)
    //   pan    : -1..+1 (negative=left, positive=right, 0=center)
    void Enqueue(uint32_t src_id,
                 const int16_t* mono_pcm, uint32_t samples,
                 float gain, float pan);

    // Drop a source (e.g. session disconnected).
    void DropSource(uint32_t src_id);

    // Number of sources that mixed audio in the last 100 ms.
    int ActiveSpeakers();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace voice
