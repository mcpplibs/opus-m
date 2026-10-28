// opus_vector_decode — RFC 6716's decoder conformance through import
// mcpplibs.mwrap.opus: decode a test vector (.bit, opus_demo's format) to
// 16-bit little-endian PCM as `opus_demo -d <rate> <channels>
// -ignore_extensions` does, checking the range decoder's final state against
// the encoder's after every packet — the bit-exactness half of the test; the
// output's quality against the reference (.dec) is opus_compare's half.
//
//   opus_vector_decode <rate> <channels> <testvectorNN.bit> <out.sw>
//
// The decode loop is libopus 1.6.1's src/opus_demo.c (decode-only path): each
// record is a 32-bit big-endian length, the encoder's final range, the packet;
// an empty packet is lost and concealed, and a lost packet followed by one
// carrying LBRR data is recovered from that data (in-band FEC). The output is
// decoded at 24 bits (opus_decode24) and rounded to 16 as opus_demo writes it.
// This build has no DRED (upstream's default), so loss is concealed or
// recovered from FEC only, as opus_demo does in such a build.
import std;
import mcpplibs.mwrap.opus;

using namespace mcpplibs::mwrap::opus::macros;

namespace {

constexpr int max_packet = 15000;      // opus_demo's MAX_PACKET
constexpr int max_frame_size = 48000 * 2;

std::uint32_t be32(const unsigned char* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::println(std::cerr, "usage: opus_vector_decode <rate> <channels> <in.bit> <out.sw>");
        return 2;
    }
    const opus_int32 rate = std::atoi(argv[1]);
    const int channels = std::atoi(argv[2]);
    std::ifstream in(argv[3], std::ios::binary);
    std::ofstream out(argv[4], std::ios::binary);
    if (!in || !out) {
        std::println(std::cerr, "cannot open {} or {}", argv[3], argv[4]);
        return 1;
    }
    int err = 0;
    OpusDecoder* dec = opus_decoder_create(rate, channels, &err);
    if (err != OPUS_OK) {
        std::println(std::cerr, "opus_decoder_create: {}", opus_strerror(err));
        return 1;
    }
    opus_decoder_ctl(dec, OPUS_SET_COMPLEXITY(0));
    opus_decoder_ctl(dec, OPUS_SET_IGNORE_EXTENSIONS(1));

    std::vector<unsigned char> data(max_packet);
    std::vector<opus_int32> pcm(static_cast<std::size_t>(max_frame_size) * channels);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(max_frame_size) * channels * 2);
    bool lost = false, lost_prev = true;
    int lost_count = 0;
    long count = 0, samples = 0;
    for (;; ++count) {
        unsigned char field[4];
        if (!in.read(reinterpret_cast<char*>(field), 4)) break;
        const int len = static_cast<int>(be32(field));
        if (len > max_packet || len < 0) {
            std::println(std::cerr, "invalid payload length: {}", len);
            break;
        }
        if (!in.read(reinterpret_cast<char*>(field), 4)) break;
        const opus_uint32 enc_final_range = be32(field);
        if (!in.read(reinterpret_cast<char*>(data.data()), len)) {
            std::println(std::cerr, "ran out of input, expecting {} bytes", len);
            break;
        }
        lost = len == 0;
        int run_decoder = 0;
        if (lost) {
            ++lost_count;
        } else {
            run_decoder = 1 + lost_count;
        }
        for (int fr = 0; fr < run_decoder; ++fr) {
            opus_int32 output = 0;
            if (fr == lost_count - 1 && opus_packet_has_lbrr(data.data(), len)) {
                opus_decoder_ctl(dec, OPUS_GET_LAST_PACKET_DURATION(&output));
                output = opus_decode24(dec, data.data(), len, pcm.data(), output, 1);
            } else if (fr < lost_count) {
                opus_decoder_ctl(dec, OPUS_GET_LAST_PACKET_DURATION(&output));
                output = opus_decode24(dec, nullptr, 0, pcm.data(), output, 0);
            } else {
                output = opus_decode24(dec, data.data(), len, pcm.data(), max_frame_size, 0);
            }
            if (output <= 0) {
                std::println(std::cerr, "error decoding frame {}: {}", count, opus_strerror(output));
                continue;
            }
            for (int i = 0; i < output * channels; ++i) {
                opus_int32 s = pcm[i];
                s = std::clamp<opus_int32>(s, -0x007fff00, 0x007fff00);
                s = (s + 128) >> 8;
                bytes[2 * i] = static_cast<unsigned char>(s & 0xFF);
                bytes[2 * i + 1] = static_cast<unsigned char>((s >> 8) & 0xFF);
            }
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(output) * channels * 2);
            samples += output;
        }
        opus_uint32 dec_final_range = 0;
        opus_decoder_ctl(dec, OPUS_GET_FINAL_RANGE(&dec_final_range));
        if (enc_final_range != 0 && !lost && !lost_prev && dec_final_range != enc_final_range) {
            std::println(std::cerr, "range coder state mismatch in frame {}: {:#010x} vs {:#010x}", count,
                         enc_final_range, dec_final_range);
            opus_decoder_destroy(dec);
            return 1;
        }
        lost_prev = lost;
        if (!lost) lost_count = 0;
    }
    opus_decoder_destroy(dec);
    std::println("{}: {} packets, {} samples at {} Hz, {} channel(s), every final range matching", argv[3], count,
                 samples, rate, channels);
    return 0;
}
