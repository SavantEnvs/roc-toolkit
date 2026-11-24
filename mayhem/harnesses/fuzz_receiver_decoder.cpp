// Fuzz the receiver side of libroc through its socket-less public API:
// roc_receiver_decoder_push_packet() + pop_frame() + pop_feedback_packet().
//
// Built four times with -DFUZZ_MODE=<n>:
//   0 (RTP)    : AUDIO_SOURCE=RTP,              AUDIO_CONTROL=RTCP
//   1 (RS8M)   : AUDIO_SOURCE=RTP_RS8M_SOURCE,  AUDIO_REPAIR=RS8M_REPAIR, AUDIO_CONTROL=RTCP
//   2 (LDPC)   : AUDIO_SOURCE=RTP_LDPC_SOURCE,  AUDIO_REPAIR=LDPC_REPAIR, AUDIO_CONTROL=RTCP
//   3 (CONFIG) : like RTP, but a 12-byte header selects the receiver config (frame rate/layout,
//                resampler and latency tuner backends/profiles, target latency) and registers a
//                custom RTP encoding (payload type, rate, layout) in the context.
//
// Input: [config header (mode 3 only)] then records: [iface:u8][len:u16 BE][len bytes].
// iface % 3 selects SOURCE / REPAIR / CONTROL. After each push one frame is popped and any
// feedback (RTCP) packets are drained.
//
// State kept across inputs: none. Every mode opens and closes its own roc_context (pools, arena,
// encoding registry, network and control loops) and decoder per input, so each result depends
// only on that input. Only the log level (set once to NONE, never changed) is process-global.
#include <roc/context.h>
#include <roc/log.h>
#include <roc/receiver_decoder.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef FUZZ_MODE
#define FUZZ_MODE 0
#endif

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    roc_log_set_level(ROC_LOG_NONE);
    return 0;
}

enum { MaxFrameSamples = 48000 / 100 * 8 }; // 10ms, up to 8 channels at 48k

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    roc_context* context = NULL;

    roc_receiver_config rc;
    memset(&rc, 0, sizeof(rc));
    rc.frame_encoding.rate = 44100;
    rc.frame_encoding.format = ROC_FORMAT_PCM_FLOAT32;
    rc.frame_encoding.channels = ROC_CHANNEL_LAYOUT_STEREO;
    rc.clock_source = ROC_CLOCK_SOURCE_EXTERNAL;
    rc.target_latency = 20000000; // 20ms, so a few packets already reach the depacketizer

#if FUZZ_MODE == 3
    if (size < 12) {
        return -1;
    }
    static const unsigned rates[] = { 8000, 16000, 22050, 44100, 48000, 96000 };
    rc.frame_encoding.rate = rates[data[0] % 6];
    rc.frame_encoding.channels = (data[1] & 1) ? ROC_CHANNEL_LAYOUT_MONO : ROC_CHANNEL_LAYOUT_STEREO;
    rc.resampler_backend = (roc_resampler_backend)(data[2] % 4);
    rc.resampler_profile = (roc_resampler_profile)(data[3] % 4);
    rc.latency_tuner_backend = (data[4] & 1) ? ROC_LATENCY_TUNER_BACKEND_NIQ
                                             : ROC_LATENCY_TUNER_BACKEND_DEFAULT;
    rc.latency_tuner_profile = (roc_latency_tuner_profile)(data[5] % 4);
    rc.target_latency = (unsigned long long)(1 + data[6]) * 1000000ULL; // 1..256 ms
    roc_media_encoding enc;
    memset(&enc, 0, sizeof(enc));
    const int enc_id = 100 + (data[7] % 28);
    enc.rate = (unsigned)data[8] << 8 | data[9];
    enc.format = ROC_FORMAT_PCM_FLOAT32;
    enc.channels = (roc_channel_layout)(1 + data[10] % 3);
    enc.tracks = enc.channels == ROC_CHANNEL_LAYOUT_MULTITRACK ? (data[11] % 1100) : 0;
    data += 12;
    size -= 12;
#endif

    roc_context_config cc;
    memset(&cc, 0, sizeof(cc));
    if (roc_context_open(&cc, &context) != 0) {
        abort();
    }
#if FUZZ_MODE == 3
    // The registration may legitimately fail on invalid values; the decoder then just
    // sees an unknown payload type.
    (void)roc_context_register_encoding(context, enc_id, &enc);
#endif

    roc_receiver_decoder* decoder = NULL;
    if (roc_receiver_decoder_open(context, &rc, &decoder) != 0) {
#if FUZZ_MODE == 3
        roc_context_close(context);
        return 0;
#else
        abort();
#endif
    }

#if FUZZ_MODE == 1
    roc_receiver_decoder_activate(decoder, ROC_INTERFACE_AUDIO_SOURCE, ROC_PROTO_RTP_RS8M_SOURCE);
    roc_receiver_decoder_activate(decoder, ROC_INTERFACE_AUDIO_REPAIR, ROC_PROTO_RS8M_REPAIR);
#elif FUZZ_MODE == 2
    roc_receiver_decoder_activate(decoder, ROC_INTERFACE_AUDIO_SOURCE, ROC_PROTO_RTP_LDPC_SOURCE);
    roc_receiver_decoder_activate(decoder, ROC_INTERFACE_AUDIO_REPAIR, ROC_PROTO_LDPC_REPAIR);
#else
    roc_receiver_decoder_activate(decoder, ROC_INTERFACE_AUDIO_SOURCE, ROC_PROTO_RTP);
#endif
    roc_receiver_decoder_activate(decoder, ROC_INTERFACE_AUDIO_CONTROL, ROC_PROTO_RTCP);

    const size_t num_ch = rc.frame_encoding.channels == ROC_CHANNEL_LAYOUT_MONO ? 1 : 2;
    const size_t frame_samples = rc.frame_encoding.rate / 100 * num_ch;
    static float samples[MaxFrameSamples];
    static uint8_t fb_buf[8192];
    static uint8_t pkt_buf[65536];

    while (size >= 3) {
        const uint8_t sel = data[0] % 3;
        size_t len = (size_t)data[1] << 8 | data[2];
        data += 3;
        size -= 3;
        if (len > size) {
            len = size;
        }
        // Copy so the decoder never sees bytes past the record (and ASan catches overreads).
        memcpy(pkt_buf, data, len);
        data += len;
        size -= len;

        roc_interface iface = sel == 0   ? ROC_INTERFACE_AUDIO_SOURCE
                              : sel == 1 ? ROC_INTERFACE_AUDIO_REPAIR
                                         : ROC_INTERFACE_AUDIO_CONTROL;
        roc_packet pkt;
        pkt.bytes = pkt_buf;
        pkt.bytes_size = len;
        (void)roc_receiver_decoder_push_packet(decoder, iface, &pkt);

        roc_frame frame;
        frame.samples = samples;
        frame.samples_size = frame_samples * sizeof(float);
        (void)roc_receiver_decoder_pop_frame(decoder, &frame);

        for (int i = 0; i < 4; i++) {
            roc_packet fb;
            fb.bytes = fb_buf;
            fb.bytes_size = sizeof(fb_buf);
            if (roc_receiver_decoder_pop_feedback_packet(decoder, ROC_INTERFACE_AUDIO_CONTROL,
                                                         &fb)
                != 0) {
                break;
            }
        }
    }
    // Drain a little more audio so the depacketizer/FEC reader run on everything queued.
    for (int i = 0; i < 4; i++) {
        roc_frame frame;
        frame.samples = samples;
        frame.samples_size = frame_samples * sizeof(float);
        (void)roc_receiver_decoder_pop_frame(decoder, &frame);
    }

    roc_receiver_decoder_close(decoder);
    roc_context_close(context);
    return 0;
}
