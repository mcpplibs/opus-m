// opus — an Ogg Opus tool written against import mcpplibs.mwrap.opus:
//
//   opus encode [--bitrate <bits/s>] <in.wav> <out.opus>   16-bit PCM WAV (8, 12, 16, 24 or 48 kHz,
//                                                          mono or stereo) to Ogg Opus (RFC 7845)
//   opus decode <in.opus> <out.wav>                        Ogg Opus to 16-bit 48 kHz WAV, every
//                                                          link, gains applied, end-trimmed
//   opus info <file.opus>                                  links, OpusHead, OpusTags, length, bitrate
//
// The encoder is libopus (OPUS_APPLICATION_AUDIO, 20 ms frames); the pages are
// libogg's; the reader is opusfile, which applies the pre-skip, the output
// gain and end trimming, so a decode is exactly as long as the encoded input.
import std;
import mcpplibs.mwrap.opus;

using namespace mcpplibs::mwrap::opus::macros;

namespace {

struct Wav {
    int rate = 0;
    int channels = 0;
    std::vector<opus_int16> samples;  // interleaved
};

std::uint32_t le(const unsigned char* p, int n) {
    std::uint32_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
void put(std::vector<unsigned char>& v, std::uint32_t x, int n) {
    for (int i = 0; i < n; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}

std::optional<Wav> read_wav(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)), {});
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return std::nullopt;
    Wav wav;
    int bits = 0;
    for (std::size_t at = 12; at + 8 <= b.size();) {
        const std::uint32_t size = le(&b[at + 4], 4);
        const unsigned char* body = &b[at + 8];
        if (at + 8 + size > b.size()) return std::nullopt;
        if (!std::memcmp(&b[at], "fmt ", 4) && size >= 16) {
            if (le(body, 2) != 1) return std::nullopt;  // PCM only
            wav.channels = static_cast<int>(le(body + 2, 2));
            wav.rate = static_cast<int>(le(body + 4, 4));
            bits = static_cast<int>(le(body + 14, 2));
        } else if (!std::memcmp(&b[at], "data", 4)) {
            if (bits != 16) return std::nullopt;
            wav.samples.resize(size / 2);
            for (std::size_t i = 0; i < wav.samples.size(); ++i)
                wav.samples[i] = static_cast<opus_int16>(le(body + 2 * i, 2));
        }
        at += 8 + size + (size & 1);
    }
    if (wav.channels < 1 || wav.rate <= 0) return std::nullopt;
    return wav;
}

bool write_wav(const std::string& path, int rate, int channels, const std::vector<opus_int16>& samples) {
    std::vector<unsigned char> b = {'R', 'I', 'F', 'F'};
    const std::uint32_t data = static_cast<std::uint32_t>(samples.size() * 2);
    put(b, 36 + data, 4);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put(b, 16, 4);
    put(b, 1, 2);
    put(b, static_cast<std::uint32_t>(channels), 2);
    put(b, static_cast<std::uint32_t>(rate), 4);
    put(b, static_cast<std::uint32_t>(rate * channels * 2), 4);
    put(b, static_cast<std::uint32_t>(channels * 2), 2);
    put(b, 16, 2);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put(b, data, 4);
    for (opus_int16 s : samples) put(b, static_cast<std::uint16_t>(s), 2);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(out);
}

void append(std::vector<unsigned char>& file, const ogg_page& page) {
    file.insert(file.end(), page.header, page.header + page.header_len);
    file.insert(file.end(), page.body, page.body + page.body_len);
}

// Ogg Opus (RFC 7845): OpusHead, OpusTags, then 20 ms packets; granule
// positions count 48 kHz samples, and the last one trims the end.
int encode(const Wav& wav, opus_int32 bitrate, std::vector<unsigned char>& file) {
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(wav.rate, wav.channels, OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK) {
        std::println(std::cerr, "opus_encoder_create({} Hz, {} channels): {}", wav.rate, wav.channels, opus_strerror(err));
        return 1;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
    opus_int32 lookahead = 0;
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));
    const int scale = 48000 / wav.rate;  // granule positions are at 48 kHz
    const opus_int32 preskip = lookahead * scale;

    ogg_stream_state os;
    ogg_stream_init(&os, 0x6f707573);
    ogg_page page;
    std::vector<unsigned char> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, static_cast<unsigned char>(wav.channels)};
    put(head, static_cast<std::uint32_t>(preskip), 2);
    put(head, static_cast<std::uint32_t>(wav.rate), 4);
    put(head, 0, 2);
    head.push_back(0);
    ogg_packet op{head.data(), static_cast<long>(head.size()), 1, 0, 0, 0};
    ogg_stream_packetin(&os, &op);
    while (ogg_stream_flush(&os, &page)) append(file, page);
    const std::string vendor = opus_get_version_string();
    std::vector<unsigned char> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'};
    put(tags, static_cast<std::uint32_t>(vendor.size()), 4);
    tags.insert(tags.end(), vendor.begin(), vendor.end());
    put(tags, 1, 4);
    const std::string encoder = "ENCODER=opus-m example";
    put(tags, static_cast<std::uint32_t>(encoder.size()), 4);
    tags.insert(tags.end(), encoder.begin(), encoder.end());
    op = ogg_packet{tags.data(), static_cast<long>(tags.size()), 0, 0, 0, 1};
    ogg_stream_packetin(&os, &op);
    while (ogg_stream_flush(&os, &page)) append(file, page);

    const int frame = wav.rate / 50;
    const long input = static_cast<long>(wav.samples.size()) / wav.channels;
    const long total = input + lookahead;  // input samples, plus the encoder's delay flushed out
    std::vector<opus_int16> pcm(static_cast<std::size_t>(frame) * wav.channels);
    unsigned char packet[4000];
    long packets = 0;
    for (long at = 0; at < total; at += frame, ++packets) {
        std::fill(pcm.begin(), pcm.end(), opus_int16{0});
        const long n = std::clamp<long>(input - at, 0, frame);
        std::copy_n(wav.samples.begin() + at * wav.channels, n * wav.channels, pcm.begin());
        const int len = opus_encode(enc, pcm.data(), frame, packet, sizeof packet);
        if (len < 0) {
            std::println(std::cerr, "opus_encode: {}", opus_strerror(len));
            return 1;
        }
        const bool last = at + frame >= total;
        const ogg_int64_t granule = last ? total * scale : (at + frame) * scale;
        op = ogg_packet{packet, len, 0, last ? 1 : 0, granule, 2 + packets};
        ogg_stream_packetin(&os, &op);
        while (ogg_stream_pageout(&os, &page)) append(file, page);
    }
    while (ogg_stream_flush(&os, &page)) append(file, page);
    ogg_stream_clear(&os);
    opus_encoder_destroy(enc);
    std::println("{} samples at {} Hz, {} channel(s): {} packets, {} bytes at {} b/s", input, wav.rate, wav.channels,
                 packets, file.size(), bitrate);
    return 0;
}

std::string duration(ogg_int64_t samples) {
    return std::format("{}:{:06.3f}", samples / (48000 * 60), static_cast<double>(samples % (48000 * 60)) / 48000);
}

int info(const std::string& path) {
    int err = 0;
    OggOpusFile* of = op_open_file(path.c_str(), &err);
    if (!of) {
        std::println(std::cerr, "{}: not an Ogg Opus file ({})", path, err);
        return 1;
    }
    const int links = op_link_count(of);
    std::println("{}: {} link(s), {} ({} samples at 48 kHz), {} bytes, {} b/s", path, links, duration(op_pcm_total(of, -1)),
                 op_pcm_total(of, -1), op_raw_total(of, -1), op_bitrate(of, -1));
    for (int li = 0; li < links; ++li) {
        const OpusHead* head = op_head(of, li);
        const OpusTags* tags = op_tags(of, li);
        std::println("link {}: serial {:#010x}, {} channel(s), mapping family {}, pre-skip {}, input {} Hz, output gain {} dB, {}",
                     li, op_serialno(of, li), head->channel_count, head->mapping_family, head->pre_skip,
                     head->input_sample_rate, head->output_gain / 256.0, duration(op_pcm_total(of, li)));
        std::println("  vendor: {}", tags->vendor);
        for (int ci = 0; ci < tags->comments; ++ci) {
            const std::string_view comment(tags->user_comments[ci], static_cast<std::size_t>(tags->comment_lengths[ci]));
            if (opus_tagncompare("METADATA_BLOCK_PICTURE", 22, tags->user_comments[ci]) == 0) {
                OpusPictureTag pic;
                opus_picture_tag_init(&pic);
                if (opus_picture_tag_parse(&pic, tags->user_comments[ci]) == 0)
                    std::println("  picture: type {}, {}, {}x{}", pic.type, pic.mime_type, pic.width, pic.height);
                opus_picture_tag_clear(&pic);
            } else {
                std::println("  {}", comment);
            }
        }
    }
    op_free(of);
    return 0;
}

int decode(const std::string& in, const std::string& out) {
    int err = 0;
    OggOpusFile* of = op_open_file(in.c_str(), &err);
    if (!of) {
        std::println(std::cerr, "{}: not an Ogg Opus file ({})", in, err);
        return 1;
    }
    // One channel count for the whole WAV: the first link's, or stereo when
    // the links differ or have more than two (opusfile downmixes).
    int channels = op_channel_count(of, 0);
    for (int li = 1; li < op_link_count(of); ++li)
        if (op_channel_count(of, li) != channels) channels = 2;
    if (channels > 2) channels = 2;
    std::vector<opus_int16> samples;
    std::vector<opus_int16> buffer(120 * 48 * 8);
    for (;;) {
        int n = 0;
        if (channels == 2) {
            n = op_read_stereo(of, buffer.data(), static_cast<int>(buffer.size()));
        } else {
            n = op_read(of, buffer.data(), static_cast<int>(buffer.size()), nullptr);
        }
        if (n == OP_HOLE) continue;  // a gap in the data: opusfile resumes after it
        if (n < 0) {
            std::println(std::cerr, "{}: decoding failed ({})", in, n);
            op_free(of);
            return 1;
        }
        if (n == 0) break;
        samples.insert(samples.end(), buffer.begin(), buffer.begin() + n * channels);
    }
    op_free(of);
    if (!write_wav(out, 48000, channels, samples)) {
        std::println(std::cerr, "cannot write {}", out);
        return 1;
    }
    std::println("{}: {} samples at 48 kHz, {} channel(s) -> {}", in, samples.size() / channels, channels, out);
    return 0;
}

int usage() {
    std::println(std::cerr, "usage: opus encode [--bitrate <bits/s>] <in.wav> <out.opus>\n"
                            "       opus decode <in.opus> <out.wav>\n"
                            "       opus info <file.opus>");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    if (args[0] == "info" && args.size() == 2) return info(args[1]);
    if (args[0] == "decode" && args.size() == 3) return decode(args[1], args[2]);
    if (args[0] == "encode") {
        opus_int32 bitrate = 96000;
        std::size_t at = 1;
        if (args.size() > 2 && args[1] == "--bitrate") {
            bitrate = std::stoi(args[2]);
            at = 3;
        }
        if (args.size() != at + 2) return usage();
        auto wav = read_wav(args[at]);
        if (!wav) {
            std::println(std::cerr, "{}: not a 16-bit PCM WAV file", args[at]);
            return 1;
        }
        std::vector<unsigned char> file;
        if (int rc = encode(*wav, bitrate, file)) return rc;
        std::ofstream out(args[at + 1], std::ios::binary);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        return out ? 0 : 1;
    }
    return usage();
}
