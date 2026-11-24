// Seed generator (not part of the build): uses libroc's own sender_encoder to produce valid
// RTP / RS8M / LDPC / RTCP packet streams in the record formats of the fuzz harnesses, and a
// receiver_decoder fed with them to produce RTCP receiver feedback for the sender target.
// Usage: gen_seeds <outdir>   (writes <outdir>/<target>/testsuite/*)
#include <roc/context.h>
#include <roc/log.h>
#include <roc/receiver_decoder.h>
#include <roc/sender_encoder.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

typedef std::vector<unsigned char> Buf;

static void rec3(Buf& out, int iface, const unsigned char* p, size_t n) {
    out.push_back((unsigned char)iface);
    out.push_back((unsigned char)(n >> 8));
    out.push_back((unsigned char)n);
    out.insert(out.end(), p, p + n);
}
static void rec2(Buf& out, const unsigned char* p, size_t n) {
    out.push_back((unsigned char)(n >> 8));
    out.push_back((unsigned char)n);
    out.insert(out.end(), p, p + n);
}
static void save(const std::string& path, const Buf& b) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); exit(1); }
    fwrite(b.data(), 1, b.size(), f);
    fclose(f);
}

// fec: 0 none, 1 rs8m, 2 ldpc. Returns decoder-records stream; fills feedback records.
static Buf gen(roc_context* ctx, int fec, int frames, bool mono, Buf* feedback, const Buf& prefix) {
    roc_sender_config sc;
    memset(&sc, 0, sizeof(sc));
    sc.frame_encoding.rate = 44100;
    sc.frame_encoding.format = ROC_FORMAT_PCM_FLOAT32;
    sc.frame_encoding.channels = ROC_CHANNEL_LAYOUT_STEREO;
    sc.packet_encoding = mono ? ROC_PACKET_ENCODING_AVP_L16_MONO : ROC_PACKET_ENCODING_AVP_L16_STEREO;
    sc.fec_encoding = fec == 0 ? ROC_FEC_ENCODING_DISABLE
                    : fec == 1 ? ROC_FEC_ENCODING_RS8M : ROC_FEC_ENCODING_LDPC_STAIRCASE;
    if (fec == 1) { // small RS8M blocks; LDPC keeps its defaults (it rejects tiny blocks)
        sc.fec_block_source_packets = 4;
        sc.fec_block_repair_packets = 2;
    }
    sc.clock_source = ROC_CLOCK_SOURCE_EXTERNAL;
    roc_sender_encoder* enc;
    if (roc_sender_encoder_open(ctx, &sc, &enc)) { fprintf(stderr, "enc open\n"); exit(1); }
    roc_protocol sp = fec == 0 ? ROC_PROTO_RTP : fec == 1 ? ROC_PROTO_RTP_RS8M_SOURCE : ROC_PROTO_RTP_LDPC_SOURCE;
    roc_sender_encoder_activate(enc, ROC_INTERFACE_AUDIO_SOURCE, sp);
    if (fec) roc_sender_encoder_activate(enc, ROC_INTERFACE_AUDIO_REPAIR,
                                         fec == 1 ? ROC_PROTO_RS8M_REPAIR : ROC_PROTO_LDPC_REPAIR);
    roc_sender_encoder_activate(enc, ROC_INTERFACE_AUDIO_CONTROL, ROC_PROTO_RTCP);

    roc_receiver_config rc;
    memset(&rc, 0, sizeof(rc));
    rc.frame_encoding = sc.frame_encoding;
    rc.clock_source = ROC_CLOCK_SOURCE_EXTERNAL;
    rc.target_latency = 20000000;
    roc_receiver_decoder* dec;
    if (roc_receiver_decoder_open(ctx, &rc, &dec)) { fprintf(stderr, "dec open\n"); exit(1); }
    roc_receiver_decoder_activate(dec, ROC_INTERFACE_AUDIO_SOURCE, sp);
    if (fec) roc_receiver_decoder_activate(dec, ROC_INTERFACE_AUDIO_REPAIR,
                                           fec == 1 ? ROC_PROTO_RS8M_REPAIR : ROC_PROTO_LDPC_REPAIR);
    roc_receiver_decoder_activate(dec, ROC_INTERFACE_AUDIO_CONTROL, ROC_PROTO_RTCP);

    Buf out = prefix;
    float samples[441 * 2];
    unsigned char pb[4096];
    static const roc_interface ifs[] = { ROC_INTERFACE_AUDIO_SOURCE, ROC_INTERFACE_AUDIO_REPAIR,
                                         ROC_INTERFACE_AUDIO_CONTROL };
    double ph = 0;
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < 441; i++) {
            samples[2 * i] = samples[2 * i + 1] = 0.5f * (float)sin(ph);
            ph += 2 * M_PI * 440 / 44100;
        }
        roc_frame fr = { samples, sizeof(samples) };
        roc_sender_encoder_push_frame(enc, &fr);
        for (int k = 0; k < 3; k++) {
            for (;;) {
                roc_packet p = { pb, sizeof(pb) };
                if (roc_sender_encoder_pop_packet(enc, ifs[k], &p)) break;
                rec3(out, k, pb, p.bytes_size);
                roc_receiver_decoder_push_packet(dec, ifs[k], &p);
            }
        }
        roc_frame rf = { samples, sizeof(samples) };
        roc_receiver_decoder_pop_frame(dec, &rf);
        for (;;) {
            roc_packet p = { pb, sizeof(pb) };
            if (roc_receiver_decoder_pop_feedback_packet(dec, ROC_INTERFACE_AUDIO_CONTROL, &p)) break;
            if (feedback) rec2(*feedback, pb, p.bytes_size);
        }
    }
    roc_receiver_decoder_close(dec);
    roc_sender_encoder_close(enc);
    return out;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::string d = argv[1];
    roc_log_set_level(ROC_LOG_NONE);
    roc_context_config cc;
    memset(&cc, 0, sizeof(cc));
    roc_context* ctx;
    if (roc_context_open(&cc, &ctx)) return 1;
    Buf none;
    Buf fb;
    save(d + "/fuzz_receiver_rtp/testsuite/rtp_stereo", gen(ctx, 0, 12, false, NULL, none));
    save(d + "/fuzz_receiver_rtp/testsuite/rtp_mono", gen(ctx, 0, 12, true, NULL, none));
    save(d + "/fuzz_receiver_rs8m/testsuite/rs8m", gen(ctx, 1, 12, false, &fb, none));
    save(d + "/fuzz_receiver_ldpc/testsuite/ldpc", gen(ctx, 2, 40, false, NULL, none));
    save(d + "/fuzz_sender_feedback/testsuite/rtcp_feedback", fb);
    // config target: header = rate idx 3 (44100), stereo, builtin/speex resampler, profiles,
    // 20ms latency, registered encoding id 100 @ 48000 stereo (unused by the L16 PT 10 stream).
    const unsigned char h1[12] = { 3, 0, 1, 2, 0, 0, 19, 0, 0xbb, 0x80, 2, 0 };
    const unsigned char h2[12] = { 4, 1, 2, 1, 1, 2, 9, 0, 0xac, 0x44, 1, 0 };
    save(d + "/fuzz_receiver_config/testsuite/cfg_44k", gen(ctx, 0, 12, false, NULL, Buf(h1, h1 + 12)));
    save(d + "/fuzz_receiver_config/testsuite/cfg_48k_mono", gen(ctx, 0, 12, true, NULL, Buf(h2, h2 + 12)));
    roc_context_close(ctx);
    return 0;
}
