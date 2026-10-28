// The Opus API through the modules, spelled as the headers document it:
// libopus (encoders and decoders at every rate, the 16-bit, 24-bit and float
// interfaces, CTLs, packet parsing, padding, multistream, ambisonics
// projection, the repacketizer, soft clipping, the DRED API of a build
// without DRED), libogg (pages, packets and the bit packer) and opusfile (Ogg
// Opus streams from memory, callbacks and a file: headers, tags, pictures,
// chained links, seeking, gains, dithering and the decode callback). Only the
// module is imported; no Opus header is included.
#include <gtest/gtest.h>

import std;
import mcpplibs.mwrap.opus;

using namespace mcpplibs::mwrap::opus::macros;  // OPUS_OK, OPUS_SET_BITRATE, OP_EOF …

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> tone(int rate, int channels, int samples, double f0, double amplitude = 0.5) {
    std::vector<float> pcm(static_cast<std::size_t>(samples) * channels);
    for (int i = 0; i < samples; ++i)
        for (int c = 0; c < channels; ++c)
            pcm[static_cast<std::size_t>(i) * channels + c] =
                static_cast<float>(amplitude * std::sin(2 * kPi * f0 * (c + 1) * i / rate));
    return pcm;
}

// Signal-to-noise ratio in dB of `out` (delayed by `delay`) against `in`, over [from, to).
double snr(const std::vector<float>& in, const std::vector<float>& out, int channels, int delay, int from, int to) {
    double signal = 0, noise = 0;
    for (int i = from; i < to; ++i)
        for (int c = 0; c < channels; ++c) {
            const double a = in[static_cast<std::size_t>(i) * channels + c];
            const double b = out[static_cast<std::size_t>(i + delay) * channels + c];
            signal += a * a;
            noise += (a - b) * (a - b);
        }
    return 10 * std::log10(signal / (noise + 1e-30));
}

struct Encoder {
    OpusEncoder* st;
    Encoder(opus_int32 rate, int channels, int application) {
        int err = OPUS_INTERNAL_ERROR;
        st = opus_encoder_create(rate, channels, application, &err);
        EXPECT_EQ(err, OPUS_OK);
    }
    ~Encoder() { opus_encoder_destroy(st); }
};

struct Decoder {
    OpusDecoder* st;
    Decoder(opus_int32 rate, int channels) {
        int err = OPUS_INTERNAL_ERROR;
        st = opus_decoder_create(rate, channels, &err);
        EXPECT_EQ(err, OPUS_OK);
    }
    ~Decoder() { opus_decoder_destroy(st); }
};

// ── Ogg Opus streams (RFC 7845), paged with libogg ──────────────────────────

void put16(std::vector<unsigned char>& v, unsigned x) {
    v.push_back(x & 0xFF);
    v.push_back((x >> 8) & 0xFF);
}
void put32(std::vector<unsigned char>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}
void put32be(std::vector<unsigned char>& v, std::uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back((x >> (8 * i)) & 0xFF);
}
void append(std::vector<unsigned char>& file, const ogg_page& page) {
    file.insert(file.end(), page.header, page.header + page.header_len);
    file.insert(file.end(), page.body, page.body + page.body_len);
}

std::string base64(const std::vector<unsigned char>& in) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < in.size(); i += 3) {
        std::uint32_t v = in[i] << 16;
        if (i + 1 < in.size()) v |= in[i + 1] << 8;
        if (i + 2 < in.size()) v |= in[i + 2];
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += i + 1 < in.size() ? table[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? table[v & 63] : '=';
    }
    return out;
}

// A METADATA_BLOCK_PICTURE: a front cover (type 3), a 1x1 PNG's signature and IHDR.
std::string cover() {
    std::vector<unsigned char> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    put32be(png, 13);
    png.insert(png.end(), {'I', 'H', 'D', 'R'});
    put32be(png, 1);
    put32be(png, 1);
    png.insert(png.end(), {8, 2, 0, 0, 0});
    put32be(png, 0);  // CRC, not checked by the parser
    std::vector<unsigned char> block;
    const std::string mime = "image/png", description = "cover";
    put32be(block, 3);
    put32be(block, static_cast<std::uint32_t>(mime.size()));
    block.insert(block.end(), mime.begin(), mime.end());
    put32be(block, static_cast<std::uint32_t>(description.size()));
    block.insert(block.end(), description.begin(), description.end());
    put32be(block, 1);   // width
    put32be(block, 1);   // height
    put32be(block, 24);  // depth
    put32be(block, 0);   // colors
    put32be(block, static_cast<std::uint32_t>(png.size()));
    block.insert(block.end(), png.begin(), png.end());
    return "METADATA_BLOCK_PICTURE=" + base64(block);
}

// One link: OpusHead, OpusTags, then 20 ms packets of a tone ending at exactly
// `samples` after the pre-skip (end trimming, RFC 7845 §4.4).
void link(std::vector<unsigned char>& file, int serial, int channels, int samples, int gain_q8,
          const std::vector<std::string>& comments, double f0) {
    constexpr int rate = 48000, frame = 960;
    Encoder enc(rate, channels, OPUS_APPLICATION_AUDIO);
    opus_int32 preskip = 0;
    opus_encoder_ctl(enc.st, OPUS_GET_LOOKAHEAD(&preskip));

    ogg_stream_state os;
    EXPECT_EQ(ogg_stream_init(&os, serial), 0);
    ogg_page page;
    std::vector<unsigned char> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, static_cast<unsigned char>(channels)};
    put16(head, static_cast<unsigned>(preskip));
    put32(head, rate);
    put16(head, static_cast<unsigned>(gain_q8) & 0xFFFF);
    head.push_back(0);
    ogg_packet op{head.data(), static_cast<long>(head.size()), 1, 0, 0, 0};
    ogg_stream_packetin(&os, &op);
    while (ogg_stream_flush(&os, &page)) append(file, page);

    const std::string vendor = "opus-m api_test";
    std::vector<unsigned char> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
    put32(tags, static_cast<std::uint32_t>(vendor.size()));
    tags.insert(tags.end(), vendor.begin(), vendor.end());
    put32(tags, static_cast<std::uint32_t>(comments.size()));
    for (const auto& c : comments) {
        put32(tags, static_cast<std::uint32_t>(c.size()));
        tags.insert(tags.end(), c.begin(), c.end());
    }
    op = ogg_packet{tags.data(), static_cast<long>(tags.size()), 0, 0, 0, 1};
    ogg_stream_packetin(&os, &op);
    while (ogg_stream_flush(&os, &page)) append(file, page);

    const int total = samples + preskip;
    const int frames = (total + frame - 1) / frame;
    auto pcm = tone(rate, channels, frames * frame, f0);
    unsigned char packet[4000];
    for (int f = 0; f < frames; ++f) {
        const int n = opus_encode_float(enc.st, pcm.data() + static_cast<std::size_t>(f) * frame * channels, frame,
                                        packet, sizeof packet);
        EXPECT_GT(n, 0);
        const bool last = f == frames - 1;
        op = ogg_packet{packet, n, 0, last ? 1 : 0, last ? total : static_cast<ogg_int64_t>(f + 1) * frame, 2 + f};
        ogg_stream_packetin(&os, &op);
        while (ogg_stream_pageout(&os, &page)) append(file, page);
    }
    while (ogg_stream_flush(&os, &page)) append(file, page);
    ogg_stream_clear(&os);
}

std::vector<unsigned char> stream(int samples) {
    std::vector<unsigned char> file;
    link(file, 1234, 2, samples, 0, {"TITLE=A440", "ARTIST=opus-m", "R128_TRACK_GAIN=-256", cover()}, 440.0);
    return file;
}

}  // namespace

// ── libopus ─────────────────────────────────────────────────────────────────

TEST(Codec, VersionAndErrors) {
    EXPECT_STREQ(opus_get_version_string(), "libopus 1.6.1");
    EXPECT_STREQ(opus_strerror(OPUS_OK), "success");
    EXPECT_STREQ(opus_strerror(OPUS_BAD_ARG), "invalid argument");
    EXPECT_STREQ(opus_strerror(OPUS_INVALID_PACKET), "corrupted stream");
    int err = OPUS_OK;
    EXPECT_EQ(opus_encoder_create(44100, 2, OPUS_APPLICATION_AUDIO, &err), nullptr);  // not an Opus rate
    EXPECT_EQ(err, OPUS_BAD_ARG);
}

TEST(Codec, CeltRoundTripFloat) {
    constexpr int rate = 48000, ch = 2, frame = 960, frames = 50;
    Encoder enc(rate, ch, OPUS_APPLICATION_AUDIO);
    Decoder dec(rate, ch);
    ASSERT_EQ(opus_encoder_ctl(enc.st, OPUS_SET_BITRATE(128000)), OPUS_OK);
    opus_int32 lookahead = 0;
    opus_encoder_ctl(enc.st, OPUS_GET_LOOKAHEAD(&lookahead));
    auto in = tone(rate, ch, frame * frames, 440.0);
    std::vector<float> out(in.size() + static_cast<std::size_t>(frame) * ch);
    unsigned char packet[4000];
    int decoded = 0;
    for (int f = 0; f < frames; ++f) {
        const int n = opus_encode_float(enc.st, in.data() + static_cast<std::size_t>(f) * frame * ch, frame, packet,
                                        sizeof packet);
        ASSERT_GT(n, 0);
        EXPECT_GE(packet[0] >> 3, 16);  // TOC config 16..31: CELT-only (RFC 6716 §3.1)
        decoded += opus_decode_float(dec.st, packet, n, out.data() + static_cast<std::size_t>(decoded) * ch, frame, 0);
        // The range coder's final state, the same on both sides of a bit-exact decode.
        opus_uint32 enc_range = 0, dec_range = 1;
        opus_encoder_ctl(enc.st, OPUS_GET_FINAL_RANGE(&enc_range));
        opus_decoder_ctl(dec.st, OPUS_GET_FINAL_RANGE(&dec_range));
        EXPECT_EQ(enc_range, dec_range);
    }
    EXPECT_EQ(decoded, frame * frames);
    EXPECT_GT(snr(in, out, ch, lookahead, frame * 5, frame * frames - lookahead), 20.0);
    opus_int32 duration = 0;
    opus_decoder_ctl(dec.st, OPUS_GET_LAST_PACKET_DURATION(&duration));
    EXPECT_EQ(duration, frame);
}

TEST(Codec, SixteenAndTwentyFourBit) {
    constexpr int rate = 48000, frame = 480;
    Encoder enc(rate, 1, OPUS_APPLICATION_AUDIO);
    Decoder dec(rate, 1);
    auto f = tone(rate, 1, frame, 1000.0);
    std::vector<opus_int16> pcm16(frame), out16(frame);
    std::vector<opus_int32> pcm24(frame), out24(frame);
    for (int i = 0; i < frame; ++i) {
        pcm16[i] = static_cast<opus_int16>(f[i] * 32767);
        pcm24[i] = static_cast<opus_int32>(f[i] * 8388607);
    }
    unsigned char packet[1500];
    int n = opus_encode(enc.st, pcm16.data(), frame, packet, sizeof packet);
    ASSERT_GT(n, 0);
    EXPECT_EQ(opus_decode(dec.st, packet, n, out16.data(), frame, 0), frame);
    n = opus_encode24(enc.st, pcm24.data(), frame, packet, sizeof packet);
    ASSERT_GT(n, 0);
    EXPECT_EQ(opus_decode24(dec.st, packet, n, out24.data(), frame, 0), frame);
    // A 10 ms frame size is accepted; 7 ms is not.
    EXPECT_EQ(opus_encode(enc.st, pcm16.data(), 336, packet, sizeof packet), OPUS_BAD_ARG);
}

TEST(Codec, SilkSpeechFecAndConcealment) {
    constexpr int rate = 16000, frame = 320, frames = 25;
    Encoder enc(rate, 1, OPUS_APPLICATION_VOIP);
    Decoder dec(rate, 1);
    opus_encoder_ctl(enc.st, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(enc.st, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc.st, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc.st, OPUS_SET_PACKET_LOSS_PERC(20));
    opus_int32 fec = 0, loss = 0, signal = 0;
    opus_encoder_ctl(enc.st, OPUS_GET_INBAND_FEC(&fec));
    opus_encoder_ctl(enc.st, OPUS_GET_PACKET_LOSS_PERC(&loss));
    opus_encoder_ctl(enc.st, OPUS_GET_SIGNAL(&signal));
    EXPECT_EQ(fec, 1);
    EXPECT_EQ(loss, 20);
    EXPECT_EQ(signal, OPUS_SIGNAL_VOICE);
    auto in = tone(rate, 1, frame * frames, 300.0);
    std::vector<float> out(frame);
    unsigned char packet[1500];
    bool silk = true, lbrr = false;
    for (int f = 0; f < frames; ++f) {
        const int n = opus_encode_float(enc.st, in.data() + static_cast<std::size_t>(f) * frame, frame, packet, sizeof packet);
        ASSERT_GT(n, 0);
        silk = silk && (packet[0] >> 3) <= 11;  // TOC config 0..11: SILK-only
        lbrr = lbrr || opus_packet_has_lbrr(packet, n) == 1;
        // Every fifth packet is "lost": concealed, then recovered from the next one's FEC.
        if (f % 5 == 4) {
            EXPECT_EQ(opus_decode_float(dec.st, nullptr, 0, out.data(), frame, 0), frame);
            EXPECT_EQ(opus_decode_float(dec.st, packet, n, out.data(), frame, 1), frame);
        } else {
            EXPECT_EQ(opus_decode_float(dec.st, packet, n, out.data(), frame, 0), frame);
        }
    }
    EXPECT_TRUE(silk);
    EXPECT_TRUE(lbrr);
    opus_int32 bandwidth = 0;
    opus_decoder_ctl(dec.st, OPUS_GET_BANDWIDTH(&bandwidth));
    EXPECT_EQ(bandwidth, OPUS_BANDWIDTH_WIDEBAND);
}

TEST(Codec, CtlsThroughTheMappedMacros) {
    Encoder enc(48000, 2, OPUS_APPLICATION_AUDIO);
    struct Setting {
        ctl_request<opus_int32> set;
        int get;
        opus_int32 expected;
    };
    const Setting settings[] = {
        {OPUS_SET_COMPLEXITY(3), OPUS_GET_COMPLEXITY_REQUEST, 3},
        {OPUS_SET_VBR(0), OPUS_GET_VBR_REQUEST, 0},
        {OPUS_SET_VBR_CONSTRAINT(0), OPUS_GET_VBR_CONSTRAINT_REQUEST, 0},
        {OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND), OPUS_GET_MAX_BANDWIDTH_REQUEST, OPUS_BANDWIDTH_WIDEBAND},
        {OPUS_SET_APPLICATION(OPUS_APPLICATION_VOIP), OPUS_GET_APPLICATION_REQUEST, OPUS_APPLICATION_VOIP},
        {OPUS_SET_FORCE_CHANNELS(1), OPUS_GET_FORCE_CHANNELS_REQUEST, 1},
        {OPUS_SET_DTX(1), OPUS_GET_DTX_REQUEST, 1},
        {OPUS_SET_LSB_DEPTH(16), OPUS_GET_LSB_DEPTH_REQUEST, 16},
        {OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS), OPUS_GET_EXPERT_FRAME_DURATION_REQUEST, OPUS_FRAMESIZE_10_MS},
        {OPUS_SET_PREDICTION_DISABLED(1), OPUS_GET_PREDICTION_DISABLED_REQUEST, 1},
        {OPUS_SET_PHASE_INVERSION_DISABLED(1), OPUS_GET_PHASE_INVERSION_DISABLED_REQUEST, 1},
    };
    for (const auto& s : settings) {
        EXPECT_EQ(opus_encoder_ctl(enc.st, s.set), OPUS_OK) << s.set.request;
        opus_int32 value = -12345;
        // The GET request with a pointer, as the header's macros pass it.
        EXPECT_EQ(opus_encoder_ctl(enc.st, ctl_request<opus_int32*>{s.get, &value}), OPUS_OK);
        EXPECT_EQ(value, s.expected) << s.get;
    }
    opus_int32 rate = 0;
    EXPECT_EQ(opus_encoder_ctl(enc.st, OPUS_GET_SAMPLE_RATE(&rate)), OPUS_OK);
    EXPECT_EQ(rate, 48000);
    // A constant request passes straight to the variadic function.
    EXPECT_EQ(opus_encoder_ctl(enc.st, OPUS_RESET_STATE), OPUS_OK);
    EXPECT_EQ(opus_encoder_ctl(enc.st, 9999), OPUS_UNIMPLEMENTED);
    static_assert(OPUS_SET_BITRATE(OPUS_AUTO).request == OPUS_SET_BITRATE_REQUEST);
    static_assert(OPUS_SET_BITRATE(OPUS_BITRATE_MAX).a == -1);

    Decoder dec(48000, 2);
    EXPECT_EQ(opus_decoder_ctl(dec.st, OPUS_SET_GAIN(256)), OPUS_OK);
    opus_int32 gain = 0, pitch = -1;
    EXPECT_EQ(opus_decoder_ctl(dec.st, OPUS_GET_GAIN(&gain)), OPUS_OK);
    EXPECT_EQ(gain, 256);
    EXPECT_EQ(opus_decoder_ctl(dec.st, OPUS_GET_PITCH(&pitch)), OPUS_OK);
    EXPECT_EQ(opus_decoder_ctl(dec.st, OPUS_SET_IGNORE_EXTENSIONS(1)), OPUS_OK);
    opus_int32 ignore = 0;
    EXPECT_EQ(opus_decoder_ctl(dec.st, OPUS_GET_IGNORE_EXTENSIONS(&ignore)), OPUS_OK);
    EXPECT_EQ(ignore, 1);
}

TEST(Codec, StatesInCallerMemory) {
    const int enc_size = opus_encoder_get_size(2), dec_size = opus_decoder_get_size(2);
    ASSERT_GT(enc_size, 0);
    ASSERT_GT(dec_size, 0);
    std::vector<std::max_align_t> enc_mem(enc_size / sizeof(std::max_align_t) + 1), dec_mem(dec_size / sizeof(std::max_align_t) + 1);
    auto* enc = reinterpret_cast<OpusEncoder*>(enc_mem.data());
    auto* dec = reinterpret_cast<OpusDecoder*>(dec_mem.data());
    ASSERT_EQ(opus_encoder_init(enc, 24000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY), OPUS_OK);
    ASSERT_EQ(opus_decoder_init(dec, 24000, 2), OPUS_OK);
    auto in = tone(24000, 2, 240, 600.0);
    std::vector<float> out(in.size());
    unsigned char packet[1500];
    const int n = opus_encode_float(enc, in.data(), 240, packet, sizeof packet);
    ASSERT_GT(n, 0);
    EXPECT_EQ(opus_decode_float(dec, packet, n, out.data(), 240, 0), 240);
    EXPECT_EQ(opus_decoder_get_nb_samples(dec, packet, n), 240);
}

TEST(Codec, PacketParsingAndPadding) {
    Encoder enc(48000, 1, OPUS_APPLICATION_AUDIO);
    opus_encoder_ctl(enc.st, OPUS_SET_BITRATE(32000));
    auto in = tone(48000, 1, 2880, 800.0);
    std::vector<unsigned char> packet(4000);
    const int n = opus_encode_float(enc.st, in.data(), 2880, packet.data(), 4000);  // 60 ms
    ASSERT_GT(n, 0);
    EXPECT_EQ(opus_packet_get_nb_channels(packet.data()), 1);
    EXPECT_EQ(opus_packet_get_nb_samples(packet.data(), n, 48000), 2880);
    EXPECT_EQ(opus_packet_get_nb_frames(packet.data(), n), 2880 / opus_packet_get_samples_per_frame(packet.data(), 48000));
    unsigned char toc = 0;
    const unsigned char* frames[48];
    opus_int16 sizes[48];
    int offset = 0;
    const int count = opus_packet_parse(packet.data(), n, &toc, frames, sizes, &offset);
    ASSERT_GT(count, 0);
    EXPECT_EQ(toc, packet[0]);
    EXPECT_EQ(count, opus_packet_get_nb_frames(packet.data(), n));
    int total = offset;
    for (int i = 0; i < count; ++i) total += sizes[i];
    EXPECT_LE(total, n);
    // Pad to 3000 bytes, and back.
    ASSERT_EQ(opus_packet_pad(packet.data(), n, 3000), OPUS_OK);
    EXPECT_EQ(opus_packet_get_nb_samples(packet.data(), 3000, 48000), 2880);
    EXPECT_EQ(opus_packet_unpad(packet.data(), 3000), n);
    EXPECT_EQ(opus_packet_get_bandwidth(packet.data()) >= OPUS_BANDWIDTH_NARROWBAND, true);
    unsigned char bad = 0xFF;
    EXPECT_EQ(opus_packet_get_nb_samples(&bad, 1, 48000), OPUS_INVALID_PACKET);
}

TEST(Codec, Repacketizer) {
    Encoder enc(48000, 1, OPUS_APPLICATION_AUDIO);
    auto in = tone(48000, 1, 1920, 500.0);
    unsigned char a[1500], b[1500], out[3000];
    const int na = opus_encode_float(enc.st, in.data(), 960, a, sizeof a);
    const int nb = opus_encode_float(enc.st, in.data() + 960, 960, b, sizeof b);
    OpusRepacketizer* rp = opus_repacketizer_create();
    ASSERT_NE(rp, nullptr);
    EXPECT_EQ(opus_repacketizer_cat(rp, a, na), OPUS_OK);
    EXPECT_EQ(opus_repacketizer_cat(rp, b, nb), OPUS_OK);
    EXPECT_EQ(opus_repacketizer_get_nb_frames(rp), 2);
    const int n = opus_repacketizer_out(rp, out, sizeof out);
    EXPECT_EQ(opus_packet_get_nb_frames(out, n), 2);
    EXPECT_EQ(opus_packet_get_nb_samples(out, n, 48000), 1920);
    const int first = opus_repacketizer_out_range(rp, 0, 1, out, sizeof out);
    EXPECT_EQ(opus_packet_get_nb_samples(out, first, 48000), 960);
    opus_repacketizer_destroy(rp);
    EXPECT_GT(opus_repacketizer_get_size(), 0);
}

TEST(Codec, SurroundMultistream) {
    constexpr int rate = 48000, ch = 6, frame = 960;
    int err = 0, streams = 0, coupled = 0;
    unsigned char mapping[8] = {};
    OpusMSEncoder* enc = opus_multistream_surround_encoder_create(rate, ch, 1, &streams, &coupled, mapping,
                                                                  OPUS_APPLICATION_AUDIO, &err);
    ASSERT_EQ(err, OPUS_OK);
    EXPECT_EQ(streams, 4);  // 5.1: L/R and Ls/Rs coupled, C and LFE alone
    EXPECT_EQ(coupled, 2);
    OpusMSDecoder* dec = opus_multistream_decoder_create(rate, ch, streams, coupled, mapping, &err);
    ASSERT_EQ(err, OPUS_OK);
    auto in = tone(rate, ch, frame, 220.0);
    std::vector<float> out(in.size());
    unsigned char packet[8000];
    const int n = opus_multistream_encode_float(enc, in.data(), frame, packet, sizeof packet);
    ASSERT_GT(n, 0);
    EXPECT_EQ(opus_multistream_decode_float(dec, packet, n, out.data(), frame, 0), frame);
    // The state of one stream, through the mapped two-argument CTL.
    OpusEncoder* first = nullptr;
    EXPECT_EQ(opus_multistream_encoder_ctl(enc, OPUS_MULTISTREAM_GET_ENCODER_STATE(0, &first)), OPUS_OK);
    ASSERT_NE(first, nullptr);
    opus_int32 bitrate = 0;
    EXPECT_EQ(opus_encoder_ctl(first, OPUS_GET_BITRATE(&bitrate)), OPUS_OK);
    EXPECT_GT(bitrate, 0);
    OpusDecoder* second = nullptr;
    EXPECT_EQ(opus_multistream_decoder_ctl(dec, OPUS_MULTISTREAM_GET_DECODER_STATE(1, &second)), OPUS_OK);
    EXPECT_NE(second, nullptr);
    EXPECT_EQ(opus_multistream_packet_pad(packet, n, n + 100, streams), OPUS_OK);
    EXPECT_EQ(opus_multistream_packet_unpad(packet, n + 100, streams), n);
    opus_multistream_encoder_destroy(enc);
    opus_multistream_decoder_destroy(dec);
}

TEST(Codec, AmbisonicsProjection) {
    constexpr int rate = 48000, ch = 4, frame = 960;
    int err = 0, streams = 0, coupled = 0;
    OpusProjectionEncoder* enc = opus_projection_ambisonics_encoder_create(rate, ch, 3, &streams, &coupled,
                                                                            OPUS_APPLICATION_AUDIO, &err);
    ASSERT_EQ(err, OPUS_OK);
    opus_int32 size = 0, gain = 1;
    ASSERT_EQ(opus_projection_encoder_ctl(enc, OPUS_PROJECTION_GET_DEMIXING_MATRIX_SIZE(&size)), OPUS_OK);
    ASSERT_GT(size, 0);
    EXPECT_EQ(opus_projection_encoder_ctl(enc, OPUS_PROJECTION_GET_DEMIXING_MATRIX_GAIN(&gain)), OPUS_OK);
    std::vector<unsigned char> matrix(static_cast<std::size_t>(size));
    ASSERT_EQ(opus_projection_encoder_ctl(enc, OPUS_PROJECTION_GET_DEMIXING_MATRIX(matrix.data(), size)), OPUS_OK);
    OpusProjectionDecoder* dec = opus_projection_decoder_create(rate, ch, streams, coupled, matrix.data(), size, &err);
    ASSERT_EQ(err, OPUS_OK);
    auto in = tone(rate, ch, frame, 330.0);
    std::vector<float> out(in.size());
    unsigned char packet[8000];
    const int n = opus_projection_encode_float(enc, in.data(), frame, packet, sizeof packet);
    ASSERT_GT(n, 0);
    EXPECT_EQ(opus_projection_decode_float(dec, packet, n, out.data(), frame, 0), frame);
    opus_projection_encoder_destroy(enc);
    opus_projection_decoder_destroy(dec);
}

TEST(Codec, SoftClipAndDredWithoutDred) {
    float pcm[4] = {0.5f, 1.5f, -2.0f, 0.25f};
    float memory[1] = {0};
    opus_pcm_soft_clip(pcm, 4, 1, memory);
    for (float v : pcm) EXPECT_LE(std::fabs(v), 1.0f);
    // compat.opus is built without DRED (upstream's default): the API is there,
    // and reports it.
    int err = OPUS_OK;
    EXPECT_EQ(opus_dred_alloc(&err), nullptr);
    EXPECT_EQ(err, OPUS_UNIMPLEMENTED);
    Encoder enc(48000, 1, OPUS_APPLICATION_AUDIO);
    EXPECT_NE(opus_encoder_ctl(enc.st, OPUS_SET_DRED_DURATION(10)), OPUS_OK);
}

// ── libogg ──────────────────────────────────────────────────────────────────

TEST(Ogg, BitPacker) {
    oggpack_buffer w;
    oggpack_writeinit(&w);
    oggpack_write(&w, 5, 3);
    oggpack_write(&w, 0x1234, 16);
    oggpack_write(&w, 1, 1);
    EXPECT_EQ(oggpack_bits(&w), 20);
    EXPECT_EQ(oggpack_bytes(&w), 3);
    oggpack_buffer r;
    oggpack_readinit(&r, oggpack_get_buffer(&w), static_cast<int>(oggpack_bytes(&w)));
    EXPECT_EQ(oggpack_read(&r, 3), 5);
    EXPECT_EQ(oggpack_look(&r, 16), 0x1234);
    oggpack_adv(&r, 16);
    EXPECT_EQ(oggpack_read1(&r), 1);
    oggpack_writeclear(&w);
    // The big-endian packer.
    oggpackB_writeinit(&w);
    oggpackB_write(&w, 0xA, 4);
    EXPECT_EQ(oggpackB_get_buffer(&w)[0], 0xA0);
    oggpackB_writeclear(&w);
}

TEST(Ogg, PagesThroughTheSyncLayer) {
    auto file = stream(4800);
    ogg_sync_state oy;
    ASSERT_EQ(ogg_sync_init(&oy), 0);
    char* buffer = ogg_sync_buffer(&oy, static_cast<long>(file.size()));
    std::memcpy(buffer, file.data(), file.size());
    ASSERT_EQ(ogg_sync_wrote(&oy, static_cast<long>(file.size())), 0);
    ogg_page page;
    int pages = 0, bos = 0, eos = 0;
    ogg_int64_t last = -1;
    while (ogg_sync_pageout(&oy, &page) == 1) {
        ++pages;
        bos += ogg_page_bos(&page) != 0;  // the header-type flags: 0x02, 0x04
        eos += ogg_page_eos(&page) != 0;
        EXPECT_EQ(ogg_page_serialno(&page), 1234);
        EXPECT_EQ(ogg_page_version(&page), 0);
        if (ogg_page_granulepos(&page) >= 0) last = ogg_page_granulepos(&page);
    }
    EXPECT_GE(pages, 3);
    EXPECT_EQ(bos, 1);
    EXPECT_EQ(eos, 1);
    opus_int32 preskip = 0;
    {
        Encoder enc(48000, 2, OPUS_APPLICATION_AUDIO);
        opus_encoder_ctl(enc.st, OPUS_GET_LOOKAHEAD(&preskip));
    }
    EXPECT_EQ(last, 4800 + preskip);
    ogg_sync_clear(&oy);
}

// ── opusfile ────────────────────────────────────────────────────────────────

TEST(File, OpenFromMemory) {
    constexpr int samples = 48000 + 123;  // not whole frames
    auto file = stream(samples);
    OpusHead probe;
    EXPECT_EQ(op_test(&probe, file.data(), file.size()), 0);
    EXPECT_EQ(probe.channel_count, 2);
    int err = -1;
    OggOpusFile* of = op_open_memory(file.data(), file.size(), &err);
    ASSERT_NE(of, nullptr);
    EXPECT_EQ(err, 0);
    EXPECT_TRUE(op_seekable(of));
    EXPECT_EQ(op_link_count(of), 1);
    EXPECT_EQ(op_serialno(of, -1), 1234u);
    EXPECT_EQ(op_channel_count(of, -1), 2);
    EXPECT_EQ(op_pcm_total(of, -1), samples);
    EXPECT_EQ(op_raw_total(of, -1), static_cast<opus_int64>(file.size()));
    EXPECT_GT(op_bitrate(of, -1), 0);
    const OpusHead* head = op_head(of, 0);
    ASSERT_NE(head, nullptr);
    EXPECT_EQ(head->version, 1);
    EXPECT_EQ(head->input_sample_rate, 48000u);
    EXPECT_EQ(head->mapping_family, 0);
    EXPECT_EQ(opus_granule_sample(head, head->pre_skip + 10), 10);

    std::vector<float> pcm(5760 * 2);
    long total = 0;
    int link = -1;
    for (;;) {
        const int n = op_read_float(of, pcm.data(), static_cast<int>(pcm.size()), &link);
        if (n <= 0) {
            EXPECT_EQ(n, 0);
            break;
        }
        EXPECT_EQ(link, 0);
        total += n;
    }
    EXPECT_EQ(total, samples);
    EXPECT_EQ(op_read_float(of, pcm.data(), static_cast<int>(pcm.size()), nullptr), 0);  // OP_EOF is 0 here

    EXPECT_EQ(op_pcm_seek(of, 24000), 0);
    EXPECT_EQ(op_pcm_tell(of), 24000);
    opus_int16 s16[5760 * 2];
    const int n = op_read_stereo(of, s16, 5760 * 2);
    EXPECT_GT(n, 0);
    EXPECT_EQ(op_pcm_tell(of), 24000 + n);
    EXPECT_EQ(op_raw_seek(of, 0), 0);
    EXPECT_GT(op_read(of, s16, 5760 * 2, nullptr), 0);
    EXPECT_EQ(op_pcm_seek(of, samples + 1), OP_EINVAL);
    op_free(of);
    EXPECT_EQ(op_open_memory(file.data(), 10, &err), nullptr);
    EXPECT_NE(err, 0);
}

TEST(File, TagsGainsAndPictures) {
    auto file = stream(9600);
    int err = 0;
    OggOpusFile* of = op_open_memory(file.data(), file.size(), &err);
    ASSERT_NE(of, nullptr);
    const OpusTags* tags = op_tags(of, -1);
    ASSERT_NE(tags, nullptr);
    EXPECT_STREQ(tags->vendor, "opus-m api_test");
    EXPECT_EQ(tags->comments, 4);
    EXPECT_STREQ(opus_tags_query(tags, "title", 0), "A440");  // tag names are case-insensitive
    EXPECT_EQ(opus_tags_query_count(tags, "ARTIST"), 1);
    EXPECT_EQ(opus_tags_query(tags, "ALBUM", 0), nullptr);
    int gain = 0;
    EXPECT_EQ(opus_tags_get_track_gain(tags, &gain), 0);
    EXPECT_EQ(gain, -256);
    EXPECT_EQ(opus_tags_get_album_gain(tags, &gain), OP_FALSE);
    EXPECT_EQ(opus_tagcompare("TITLE", "title=x"), 0);
    EXPECT_EQ(opus_tagncompare("TITLE", 5, "TITLE=x"), 0);
    EXPECT_NE(opus_tagncompare("TIT", 3, "TITLE=x"), 0);  // the name must end at the '='

    OpusPictureTag pic;
    opus_picture_tag_init(&pic);
    const char* block = opus_tags_query(tags, "METADATA_BLOCK_PICTURE", 0);
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(opus_picture_tag_parse(&pic, block), 0);
    EXPECT_EQ(pic.type, 3);
    EXPECT_STREQ(pic.mime_type, "image/png");
    EXPECT_STREQ(pic.description, "cover");
    EXPECT_EQ(pic.format, OP_PIC_FORMAT_PNG);
    EXPECT_EQ(pic.width, 1u);
    EXPECT_EQ(pic.height, 1u);
    opus_picture_tag_clear(&pic);

    // Tags of one's own, from a copy of the stream's. (opus_tags_copy reads the
    // source's vendor string, which opus_tags_init leaves null: in 0.12 a copy
    // of freshly initialised tags dereferences it.)
    OpusTags mine;
    ASSERT_EQ(opus_tags_copy(&mine, tags), 0);
    EXPECT_STREQ(mine.vendor, "opus-m api_test");
    EXPECT_EQ(opus_tags_add(&mine, "GENRE", "Test"), 0);
    EXPECT_EQ(opus_tags_add_comment(&mine, "COMMENT=hello"), 0);
    EXPECT_EQ(mine.comments, 6);
    const unsigned char text[] = {2, 3};
    EXPECT_EQ(opus_tags_set_binary_suffix(&mine, text, 2), OP_EINVAL);  // binary data starts with a 1 bit
    const unsigned char suffix[] = {1, 2, 3};
    EXPECT_EQ(opus_tags_set_binary_suffix(&mine, suffix, 3), 0);
    OpusTags copy;
    EXPECT_EQ(opus_tags_copy(&copy, &mine), 0);
    EXPECT_STREQ(opus_tags_query(&copy, "genre", 0), "Test");
    int length = -1;
    const unsigned char* data = opus_tags_get_binary_suffix(&copy, &length);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(length, 3);
    EXPECT_EQ(data[2], 3);
    opus_tags_clear(&copy);
    opus_tags_clear(&mine);
    OpusTags empty;
    opus_tags_init(&empty);
    EXPECT_EQ(empty.comments, 0);
    opus_tags_clear(&empty);

    // A gain offset and no dithering change the 16-bit output, not its length.
    EXPECT_EQ(op_set_gain_offset(of, OP_ABSOLUTE_GAIN, -6 * 256), 0);
    op_set_dither_enabled(of, 0);
    opus_int16 s16[5760 * 2];
    EXPECT_GT(op_read_stereo(of, s16, 5760 * 2), 0);
    EXPECT_EQ(op_set_gain_offset(of, 12345, 0), OP_EINVAL);
    op_free(of);
}

TEST(File, HeaderAndTagPackets) {
    const unsigned char head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, 0x38, 0x01, 0x80, 0xBB, 0, 0, 0, 0, 0};
    OpusHead h;
    ASSERT_EQ(opus_head_parse(&h, head, sizeof head), 0);
    EXPECT_EQ(h.channel_count, 2);
    EXPECT_EQ(h.pre_skip, 312u);
    EXPECT_EQ(h.input_sample_rate, 48000u);
    EXPECT_EQ(opus_head_parse(&h, head, 10), OP_EBADHEADER);
    const unsigned char tags[] = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 1, 0, 0, 0, 'v', 1, 0, 0, 0, 3, 0, 0, 0, 'A', '=', 'b'};
    OpusTags t;
    ASSERT_EQ(opus_tags_parse(&t, tags, sizeof tags), 0);
    EXPECT_STREQ(t.vendor, "v");
    EXPECT_STREQ(opus_tags_query(&t, "A", 0), "b");
    opus_tags_clear(&t);
}

TEST(File, ChainedLinks) {
    std::vector<unsigned char> file;
    link(file, 11, 1, 9600, 0, {"TITLE=one"}, 440.0);
    link(file, 22, 2, 4800, 256, {"TITLE=two"}, 660.0);
    int err = 0;
    OggOpusFile* of = op_open_memory(file.data(), file.size(), &err);
    ASSERT_NE(of, nullptr);
    EXPECT_EQ(op_link_count(of), 2);
    EXPECT_EQ(op_serialno(of, 0), 11u);
    EXPECT_EQ(op_serialno(of, 1), 22u);
    EXPECT_EQ(op_channel_count(of, 0), 1);
    EXPECT_EQ(op_channel_count(of, 1), 2);
    EXPECT_EQ(op_pcm_total(of, 0), 9600);
    EXPECT_EQ(op_pcm_total(of, 1), 4800);
    EXPECT_EQ(op_pcm_total(of, -1), 14400);
    EXPECT_STREQ(opus_tags_query(op_tags(of, 1), "TITLE", 0), "two");
    EXPECT_EQ(op_head(of, 1)->output_gain, 256);
    // Stereo downmixing reads both links as one stereo stream.
    std::vector<float> pcm(5760 * 2);
    long total = 0;
    for (int n; (n = op_read_float_stereo(of, pcm.data(), static_cast<int>(pcm.size()))) > 0;) {
        total += n;
        EXPECT_GE(op_current_link(of), 0);
    }
    EXPECT_EQ(total, 14400);
    op_free(of);
}

TEST(File, CallbacksAndDecodeCallback) {
    auto file = stream(9600);
    OpusFileCallbacks cb{};
    void* source = op_mem_stream_create(&cb, file.data(), file.size());
    ASSERT_NE(source, nullptr);
    ASSERT_NE(cb.read, nullptr);
    int err = 0;
    OggOpusFile* of = op_test_callbacks(source, &cb, nullptr, 0, &err);
    ASSERT_NE(of, nullptr);
    EXPECT_EQ(op_test_open(of), 0);
    // The application decodes each packet itself, here into floats.
    struct Count {
        int calls = 0;
        int samples = 0;
    } count;
    op_decode_cb_func decode = [](void* ctx, OpusMSDecoder* dec, void* pcm, const ogg_packet* op, int nsamples,
                                  int nchannels, int format, int li) -> int {
        auto* c = static_cast<Count*>(ctx);
        ++c->calls;
        c->samples += nsamples;
        EXPECT_EQ(nchannels, 2);
        EXPECT_EQ(li, 0);
        if (format != OP_DEC_FORMAT_FLOAT) return OP_DEC_USE_DEFAULT;
        const int n = opus_multistream_decode_float(dec, op->packet, op->bytes, static_cast<float*>(pcm), nsamples, 0);
        return n == nsamples ? 0 : OP_EBADPACKET;
    };
    op_set_decode_callback(of, decode, &count);
    std::vector<float> pcm(5760 * 2);
    long total = 0;
    for (int n; (n = op_read_float(of, pcm.data(), static_cast<int>(pcm.size()), nullptr)) > 0;) total += n;
    EXPECT_EQ(total, 9600);
    EXPECT_GT(count.calls, 0);
    op_free(of);
}

TEST(File, OpenAFile) {
    auto file = stream(4800);
    const auto path = std::filesystem::temp_directory_path() / "opus-m-api-test.opus";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    int err = 0;
    OggOpusFile* of = op_open_file(path.string().c_str(), &err);
    ASSERT_NE(of, nullptr) << err;
    EXPECT_EQ(op_pcm_total(of, -1), 4800);
    EXPECT_EQ(op_raw_tell(of) > 0, true);
    op_free(of);
    OpusFileCallbacks cb{};
    void* fp = op_fopen(&cb, path.string().c_str(), "rb");
    ASSERT_NE(fp, nullptr);
    of = op_open_callbacks(fp, &cb, nullptr, 0, &err);
    ASSERT_NE(of, nullptr);
    EXPECT_EQ(op_pcm_total(of, -1), 4800);
    EXPECT_EQ(op_bitrate_instant(of), OP_FALSE);  // nothing decoded yet
    op_free(of);
    of = op_test_file(path.string().c_str(), &err);
    ASSERT_NE(of, nullptr);
    op_free(of);
    of = op_test_memory(file.data(), file.size(), &err);
    ASSERT_NE(of, nullptr);
    op_free(of);
    std::filesystem::remove(path);
    EXPECT_EQ(op_open_file("/nonexistent/opus-m.opus", &err), nullptr);
    EXPECT_EQ(err, OP_EFAULT);
}
