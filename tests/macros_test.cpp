// The macros module on its own (no header, no root module): the constants,
// type aliases and CTL requests are constant expressions, as the macros are,
// so they work in constant-initialised tables, case labels and static_asserts.
import std;
import mcpplibs.mwrap.opus.macros;

namespace macros = mcpplibs::mwrap::opus::macros;
using namespace mcpplibs::mwrap::opus::macros;

// Every constant has the type its macro expands to.
static_assert(std::is_same_v<decltype(OPUS_OK), const int> && OPUS_OK == 0);
static_assert(OPUS_BAD_ARG == -1 && OPUS_ALLOC_FAIL == -7 && OPUS_AUTO == -1000 && OPUS_BITRATE_MAX == -1);
static_assert(OPUS_APPLICATION_VOIP == 2048 && OPUS_APPLICATION_AUDIO == 2049 && OPUS_APPLICATION_RESTRICTED_CELT == 2053);
static_assert(OPUS_BANDWIDTH_NARROWBAND == 1101 && OPUS_BANDWIDTH_FULLBAND == 1105);
static_assert(OPUS_FRAMESIZE_ARG == 5000 && OPUS_FRAMESIZE_120_MS == 5009 && OPUS_RESET_STATE == 4028);
static_assert(OPUS_GET_GAIN_REQUEST == 4045);  // "Should have been 4035", upstream says
static_assert(OPUS_SET_IGNORE_EXTENSIONS_REQUEST == 4058 && OPUS_PROJECTION_GET_DEMIXING_MATRIX_REQUEST == 6005);
static_assert(OP_FALSE == -1 && OP_EOF == -2 && OP_HOLE == -3 && OP_EBADTIMESTAMP == -139);
static_assert(OPUS_CHANNEL_COUNT_MAX == 255 && OP_PIC_FORMAT_PNG == 2 && OP_DEC_USE_DEFAULT == 6720);
static_assert(OP_HEADER_GAIN == 0 && OP_TRACK_GAIN == 3008 && OP_DEC_FORMAT_SHORT == 7008);

// The type macros of <opus_types.h>.
static_assert(std::is_same_v<opus_int, int> && std::is_same_v<opus_uint, unsigned int>);
static_assert(sizeof(opus_int8) == 1 && std::is_signed_v<opus_int8> && sizeof(opus_uint8) == 1);
static_assert(sizeof(opus_int64) == 8 && std::is_signed_v<opus_int64> && std::is_unsigned_v<opus_uint64>);

// The CTL requests: the request and its typed arguments, at compile time.
static_assert(OPUS_SET_BITRATE(64000).request == OPUS_SET_BITRATE_REQUEST && OPUS_SET_BITRATE(64000).a == 64000);
static_assert(std::is_same_v<decltype(OPUS_SET_BITRATE(1)), ctl_request<std::int32_t>>);
static_assert(std::is_same_v<decltype(OPUS_GET_FINAL_RANGE(nullptr)), ctl_request<std::uint32_t*>>);
static_assert(OPUS_SET_DNN_BLOB(nullptr, 7).b == 7 && OPUS_SET_DNN_BLOB(nullptr, 7).request == OPUS_SET_DNN_BLOB_REQUEST);
static_assert(OPUS_MULTISTREAM_GET_ENCODER_STATE(3, nullptr).a == 3);
// A floating-point argument converts, as opus_check_int's cast does.
static_assert(OPUS_SET_GAIN(2.9).a == 2);

// A constant-initialised table and a switch, as the macros allow.
constexpr ctl_request<std::int32_t> voice[] = {OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE), OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND)};
static_assert(voice[1].request == OPUS_SET_BANDWIDTH_REQUEST && voice[1].a == 1103);

constexpr const char* name(int error) {
    switch (error) {
    case OPUS_OK: return "ok";
    case OPUS_BAD_ARG: return "bad argument";
    case OP_EOF: return "end of stream";
    default: return "other";
    }
}
static_assert(std::string_view(name(OP_EOF)) == "end of stream");

int main() {
    return macros::OPUS_APPLICATION_AUDIO == 2049 && macros::OP_HOLE == -3 ? 0 : 1;
}
