// opus_codec.h — thin C++ wrapper around libopus.
//
// We compile opus via FetchContent (see client/CMakeLists.txt
// fragment). One encoder per local mic; one decoder per remote
// session id.
//
// Frame: 20ms @ 48kHz mono = 960 samples in. Output Opus payload is
// up to ~120 bytes at 24 kbps. We allow up to 1024 bytes per frame
// for safety / future bitrate bumps.

#pragma once

#include <cstdint>

namespace voice {

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kFrameSamples = 960;   // 20ms
constexpr int      kBitrate = 24000;       // 24 kbps
constexpr int      kMaxPacketBytes = 1024;

class OpusEncoder {
public:
    OpusEncoder();
    ~OpusEncoder();
    bool Init();                          // VOIP application, FEC on, complexity 5

    // Encodes one 20ms frame (kFrameSamples mono int16) into out_buf.
    // Returns the number of bytes written, or 0 on error.
    int Encode(const int16_t* pcm, uint8_t* out_buf, int out_buf_size);

private:
    void* enc_;
};

class OpusDecoder {
public:
    OpusDecoder();
    ~OpusDecoder();
    bool Init();

    // Decodes one Opus payload into a 20ms PCM frame (kFrameSamples).
    // If in_buf is null/empty, performs PLC (packet loss concealment).
    // Returns the number of samples written, or 0 on error.
    int Decode(const uint8_t* in_buf, int in_size,
               int16_t* out_pcm, int out_max_samples);

private:
    void* dec_;
};

}  // namespace voice
