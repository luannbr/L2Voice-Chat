// audio_io.cpp — miniaudio-backed capture + playback.
//
// IMPORTANT: This is the ONLY translation unit that defines
// MINIAUDIO_IMPLEMENTATION. Other TUs include only audio_io.h.
//
// Capture: WASAPI input → 48 kHz mono → fixed 20 ms frames (960
// samples) emitted via CaptureCallback on miniaudio's audio thread.
// Producers should do minimal work (encode + UDP send is fine).
//
// Playback: WASAPI output → 48 kHz stereo. Per-source ring buffers
// hold int16 mono PCM with cached delta_xyz + per-source gain. The
// data callback mixes all sources with equal-power panning and
// distance attenuation already baked into the gain.

#include "audio_io.h"
#include "opus_codec.h"  // for kSampleRate / kFrameSamples

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"  // resolved via VOICE_INCLUDE_DIRS (miniaudio FetchContent root)

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace voice {

namespace {

constexpr uint32_t kPlaybackChannels = 2;             // stereo for panning
constexpr uint32_t kRingSamples      = kFrameSamples * 8;  // 160 ms of mono
constexpr float    kIdleTimeoutMs    = 100.0f;

// Single-producer / single-consumer mono int16 ring buffer. Audio
// thread pops, caller (network thread) pushes.
struct MonoRing {
    int16_t buf[kRingSamples] = {};
    std::atomic<uint32_t> wr{0};
    std::atomic<uint32_t> rd{0};

    uint32_t Available() const {
        return wr.load(std::memory_order_acquire) - rd.load(std::memory_order_acquire);
    }
    void Push(const int16_t* src, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) {
            buf[(wr.load(std::memory_order_relaxed) + i) % kRingSamples] = src[i];
        }
        wr.fetch_add(n, std::memory_order_release);
    }
    uint32_t Pop(int16_t* dst, uint32_t n) {
        uint32_t avail = Available();
        if (n > avail) n = avail;
        for (uint32_t i = 0; i < n; ++i) {
            dst[i] = buf[(rd.load(std::memory_order_relaxed) + i) % kRingSamples];
        }
        rd.fetch_add(n, std::memory_order_release);
        return n;
    }
};

struct Source {
    MonoRing ring;
    float    delta_x = 0, delta_y = 0, delta_z = 0;
    float    volume  = 0.0f;     // already includes distance attenuation
    std::chrono::steady_clock::time_point last_mix{};
};

}  // namespace

// ---- AudioCapture ---------------------------------------------------

struct AudioCapture::Impl {
    ma_device device{};
    CaptureCallback cb;
    std::vector<int16_t> partial;  // accumulates until kFrameSamples
    bool running = false;

    static void DataCallback(ma_device* dev, void*, const void* input, ma_uint32 frame_count) {
        auto* self = static_cast<Impl*>(dev->pUserData);
        if (!self || !self->cb || frame_count == 0) return;
        const int16_t* in = static_cast<const int16_t*>(input);
        self->partial.insert(self->partial.end(), in, in + frame_count);
        while (self->partial.size() >= kFrameSamples) {
            self->cb(self->partial.data(), kFrameSamples);
            self->partial.erase(self->partial.begin(),
                                self->partial.begin() + kFrameSamples);
        }
    }
};

AudioCapture::AudioCapture()  : impl_(new Impl()) {}
AudioCapture::~AudioCapture() { Stop(); delete impl_; }

bool AudioCapture::Start(const char* /*device_name*/, CaptureCallback cb) {
    if (impl_->running) return true;
    impl_->cb = std::move(cb);
    impl_->partial.reserve(kFrameSamples * 2);

    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format   = ma_format_s16;
    cfg.capture.channels = 1;
    cfg.sampleRate       = kSampleRate;
    cfg.dataCallback     = &Impl::DataCallback;
    cfg.pUserData        = impl_;
    // TODO: honor device_name once we expose ma_context-based enumeration.

    if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) return false;
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        return false;
    }
    impl_->running = true;
    return true;
}

void AudioCapture::Stop() {
    if (!impl_->running) return;
    ma_device_uninit(&impl_->device);
    impl_->running = false;
    impl_->partial.clear();
}

bool AudioCapture::IsRunning() const { return impl_->running; }

// ---- AudioPlayback --------------------------------------------------

struct AudioPlayback::Impl {
    ma_device device{};
    std::mutex sources_mu;
    std::unordered_map<uint32_t, Source> sources;
    float min_dist = 500.0f;
    float max_dist = 2500.0f;
    bool running = false;

    // Compute pan from listener-relative delta. We assume the listener
    // faces +Y in L2's world frame (good-enough approximation for MVP;
    // the client doesn't expose camera yaw cleanly to memory).
    static void PanFromDelta(float dx, float /*dy*/, float /*dz*/,
                             float& left, float& right) {
        // Pan in [-1, +1] based on horizontal X delta, clamped to a
        // 1500 cm half-width (audio is mostly localized within ~15 m).
        float p = dx / 1500.0f;
        if (p < -1.f) p = -1.f;
        if (p >  1.f) p =  1.f;
        // Equal-power: gain_L = cos((p+1)*pi/4), gain_R = sin((p+1)*pi/4)
        float theta = (p + 1.f) * 0.785398163f;  // pi/4
        left  = std::cos(theta);
        right = std::sin(theta);
    }

    static void DataCallback(ma_device* dev, void* output, const void*, ma_uint32 frame_count) {
        auto* self = static_cast<Impl*>(dev->pUserData);
        int16_t* out = static_cast<int16_t*>(output);
        std::memset(out, 0, frame_count * kPlaybackChannels * sizeof(int16_t));

        std::lock_guard<std::mutex> lk(self->sources_mu);
        for (auto& [id, src] : self->sources) {
            if (src.ring.Available() == 0) continue;

            // Pull mono into temp buffer (up to frame_count samples).
            int16_t mono[1024];
            ma_uint32 n = frame_count > 1024 ? 1024 : frame_count;
            uint32_t got = src.ring.Pop(mono, n);
            if (got == 0) continue;

            float lpan, rpan;
            PanFromDelta(src.delta_x, src.delta_y, src.delta_z, lpan, rpan);
            float gain = src.volume;

            for (uint32_t i = 0; i < got; ++i) {
                int32_t l = out[i * 2 + 0] + (int32_t)(mono[i] * lpan * gain);
                int32_t r = out[i * 2 + 1] + (int32_t)(mono[i] * rpan * gain);
                if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
                if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
                out[i * 2 + 0] = (int16_t)l;
                out[i * 2 + 1] = (int16_t)r;
            }
            src.last_mix = std::chrono::steady_clock::now();
        }
    }
};

AudioPlayback::AudioPlayback()  : impl_(new Impl()) {}
AudioPlayback::~AudioPlayback() { Stop(); delete impl_; }

bool AudioPlayback::Start(const char* /*device_name*/) {
    if (impl_->running) return true;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_s16;
    cfg.playback.channels = kPlaybackChannels;
    cfg.sampleRate        = kSampleRate;
    cfg.dataCallback      = &Impl::DataCallback;
    cfg.pUserData         = impl_;
    if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) return false;
    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        return false;
    }
    impl_->running = true;
    return true;
}

void AudioPlayback::Stop() {
    if (!impl_->running) return;
    ma_device_uninit(&impl_->device);
    impl_->running = false;
    std::lock_guard<std::mutex> lk(impl_->sources_mu);
    impl_->sources.clear();
}

bool AudioPlayback::IsRunning() const { return impl_->running; }

void AudioPlayback::SetAttenuation(float min_dist, float max_dist) {
    impl_->min_dist = min_dist;
    impl_->max_dist = max_dist;
}

void AudioPlayback::Enqueue(uint32_t src_id,
                            const int16_t* mono_pcm, uint32_t samples,
                            float dx, float dy, float dz, float volume) {
    std::lock_guard<std::mutex> lk(impl_->sources_mu);
    auto& src = impl_->sources[src_id];
    src.delta_x = dx; src.delta_y = dy; src.delta_z = dz;
    src.volume  = volume;
    if (mono_pcm && samples > 0) {
        src.ring.Push(mono_pcm, samples);
    }
}

void AudioPlayback::DropSource(uint32_t src_id) {
    std::lock_guard<std::mutex> lk(impl_->sources_mu);
    impl_->sources.erase(src_id);
}

int AudioPlayback::ActiveSpeakers() {
    auto now = std::chrono::steady_clock::now();
    int count = 0;
    std::lock_guard<std::mutex> lk(impl_->sources_mu);
    for (auto& [id, src] : impl_->sources) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - src.last_mix).count();
        if (ms < (long long)kIdleTimeoutMs) ++count;
    }
    return count;
}

}  // namespace voice
