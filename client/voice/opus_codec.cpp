// opus_codec.cpp — thin libopus wrappers used by the voice pipeline.
//
// libopus is pulled in via CMake FetchContent (see ../CMakeLists.txt).
// We use the VOIP encoder application with FEC enabled and complexity
// 5 — values matched to the protocol spec.

#include "opus_codec.h"

#include <opus/opus.h>
#include <cstring>

namespace voice {

OpusEncoder::OpusEncoder() : enc_(nullptr) {}
OpusEncoder::~OpusEncoder() {
    if (enc_) {
        opus_encoder_destroy(static_cast<::OpusEncoder*>(enc_));
        enc_ = nullptr;
    }
}

bool OpusEncoder::Init() {
    if (enc_) return true;
    int err = 0;
    auto* e = opus_encoder_create(kSampleRate, /*channels*/ 1,
                                  OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !e) return false;
    opus_encoder_ctl(e, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(e, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(e, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(e, OPUS_SET_PACKET_LOSS_PERC(10));  // assume modest loss
    opus_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    enc_ = e;
    return true;
}

int OpusEncoder::Encode(const int16_t* pcm, uint8_t* out_buf, int out_buf_size) {
    if (!enc_) return 0;
    int n = opus_encode(static_cast<::OpusEncoder*>(enc_),
                        pcm, kFrameSamples, out_buf, out_buf_size);
    return (n > 0) ? n : 0;
}

OpusDecoder::OpusDecoder() : dec_(nullptr) {}
OpusDecoder::~OpusDecoder() {
    if (dec_) {
        opus_decoder_destroy(static_cast<::OpusDecoder*>(dec_));
        dec_ = nullptr;
    }
}

bool OpusDecoder::Init() {
    if (dec_) return true;
    int err = 0;
    auto* d = opus_decoder_create(kSampleRate, /*channels*/ 1, &err);
    if (err != OPUS_OK || !d) return false;
    dec_ = d;
    return true;
}

int OpusDecoder::Decode(const uint8_t* in_buf, int in_size,
                        int16_t* out_pcm, int out_max_samples) {
    if (!dec_) return 0;
    int n = opus_decode(static_cast<::OpusDecoder*>(dec_),
                        in_buf, in_size, out_pcm, out_max_samples,
                        /*decode_fec*/ in_buf == nullptr ? 1 : 0);
    return (n > 0) ? n : 0;
}

}  // namespace voice
