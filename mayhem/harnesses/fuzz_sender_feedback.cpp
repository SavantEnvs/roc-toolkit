// Fuzz the sender side of libroc: RTCP feedback (receiver reports, XR, SDES, BYE) pushed into
// roc_sender_encoder_push_feedback_packet(), interleaved with push_frame()/pop_packet() so the
// packetizer, RS8M FEC writer, RTCP communicator and sender latency/feedback monitor all run.
//
// Input: records [len:u16 BE][len bytes], each one RTCP compound packet.
// State kept across inputs: none. The roc_context and the encoder are opened and closed per input;
// only the log level (set once to NONE) is process-global.
#include <roc/context.h>
#include <roc/log.h>
#include <roc/sender_encoder.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    roc_log_set_level(ROC_LOG_NONE);
    return 0;
}

enum { FrameSamples = 441 * 2 }; // 10ms stereo @ 44100

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    roc_sender_config sc;
    memset(&sc, 0, sizeof(sc));
    sc.frame_encoding.rate = 44100;
    sc.frame_encoding.format = ROC_FORMAT_PCM_FLOAT32;
    sc.frame_encoding.channels = ROC_CHANNEL_LAYOUT_STEREO;
    sc.packet_encoding = ROC_PACKET_ENCODING_AVP_L16_STEREO;
    sc.fec_encoding = ROC_FEC_ENCODING_RS8M;
    sc.clock_source = ROC_CLOCK_SOURCE_EXTERNAL;

    roc_context_config cc;
    memset(&cc, 0, sizeof(cc));
    roc_context* context = NULL;
    if (roc_context_open(&cc, &context) != 0) {
        abort();
    }
    roc_sender_encoder* encoder = NULL;
    if (roc_sender_encoder_open(context, &sc, &encoder) != 0) {
        abort();
    }
    roc_sender_encoder_activate(encoder, ROC_INTERFACE_AUDIO_SOURCE, ROC_PROTO_RTP_RS8M_SOURCE);
    roc_sender_encoder_activate(encoder, ROC_INTERFACE_AUDIO_REPAIR, ROC_PROTO_RS8M_REPAIR);
    roc_sender_encoder_activate(encoder, ROC_INTERFACE_AUDIO_CONTROL, ROC_PROTO_RTCP);

    static float samples[FrameSamples];
    static uint8_t out_buf[8192];
    static uint8_t pkt_buf[65536];
    static const roc_interface ifaces[] = { ROC_INTERFACE_AUDIO_SOURCE,
                                            ROC_INTERFACE_AUDIO_REPAIR,
                                            ROC_INTERFACE_AUDIO_CONTROL };

    while (size >= 2) {
        size_t len = (size_t)data[0] << 8 | data[1];
        data += 2;
        size -= 2;
        if (len > size) {
            len = size;
        }
        memcpy(pkt_buf, data, len);
        data += len;
        size -= len;

        roc_frame frame;
        frame.samples = samples;
        frame.samples_size = sizeof(samples);
        (void)roc_sender_encoder_push_frame(encoder, &frame);

        for (size_t i = 0; i < sizeof(ifaces) / sizeof(ifaces[0]); i++) {
            for (int n = 0; n < 16; n++) {
                roc_packet out;
                out.bytes = out_buf;
                out.bytes_size = sizeof(out_buf);
                if (roc_sender_encoder_pop_packet(encoder, ifaces[i], &out) != 0) {
                    break;
                }
            }
        }

        roc_packet fb;
        fb.bytes = pkt_buf;
        fb.bytes_size = len;
        (void)roc_sender_encoder_push_feedback_packet(encoder, ROC_INTERFACE_AUDIO_CONTROL, &fb);
    }

    roc_sender_encoder_close(encoder);
    roc_context_close(context);
    return 0;
}
