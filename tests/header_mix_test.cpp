// The raw headers and the import in one translation unit (the side header
// first, the canonical order): the module's names are the very same entities
// as the headers'; the header's CTL macros and the mapped ones pass the same
// requests to the same functions; and the macros a C++ entity cannot be work
// beside them — OPUS_EXPORT, OPUS_WARN_UNUSED_RESULT, OPUS_ARG_NONNULL,
// OPUS_RESTRICT and OPUS_INLINE on declarations of one's own, OPUS_GNUC_PREREQ
// in #if, the OPUS_HAVE_OPUS_PROJECTION_H feature test.
#include <mwrap/opus/macros.hpp>
#include <gtest/gtest.h>

import std;
import mcpplibs.mwrap.opus;

namespace m = mcpplibs::mwrap::opus::macros;

// Declarations of one's own in libopus's style.
extern "C" OPUS_EXPORT OPUS_WARN_UNUSED_RESULT int mwrap_opus_twice(const opus_int16* OPUS_RESTRICT pcm, int n) OPUS_ARG_NONNULL(1);
extern "C" int mwrap_opus_twice(const opus_int16* OPUS_RESTRICT pcm, int n) { return 2 * pcm[n - 1]; }
static OPUS_INLINE int mwrap_opus_inline(int x) { return x + 1; }

#ifndef OPUS_HAVE_OPUS_PROJECTION_H
#error "<opus_projection.h> announces itself"
#endif
// A compiler-version test for #if: GCC-compatible compilers of at least that
// version, and 0 for every other (MSVC-compatible clang on Windows).
#if defined(__GNUC__)
#if !OPUS_GNUC_PREREQ(3, 4) || !OP_GNUC_PREREQ(3, 4)
#error "a GCC-compatible compiler is at least GCC 3.4"
#endif
#elif OPUS_GNUC_PREREQ(0, 0) || OP_GNUC_PREREQ(0, 0)
#error "OPUS_GNUC_PREREQ is 0 without __GNUC__"
#endif

TEST(HeaderMix, SameEntities) {
    static_assert(std::is_same_v<decltype(&::opus_encoder_create), decltype(&opus_encoder_create)>);
    static_assert(std::is_same_v<::opus_int32, std::int32_t> && std::is_same_v<::opus_uint16, std::uint16_t>);
    static_assert(std::is_same_v<::ogg_int64_t, std::int64_t>);
    EXPECT_EQ(&::opus_decode_float, &opus_decode_float);
    EXPECT_EQ(&::op_open_memory, &op_open_memory);
    EXPECT_EQ(&::ogg_stream_init, &ogg_stream_init);
    EXPECT_STREQ(opus_get_version_string(), "libopus 1.6.1");
    const opus_int16 pcm[2] = {3, 21};
    EXPECT_EQ(mwrap_opus_twice(pcm, 2), 42);
    EXPECT_EQ(mwrap_opus_inline(41), 42);
}

TEST(HeaderMix, TypeMacrosBesideTheAliases) {
    // opus_int8 / opus_int64 … are macros or <stdint.h> typedefs, the same
    // choice here as in the modules (both see <stdint.h> first).
    using raw_int64 = opus_int64;
    using raw_uint8 = opus_uint8;
    using raw_int = opus_int;
#pragma push_macro("opus_int64")
#pragma push_macro("opus_uint8")
#pragma push_macro("opus_int")
#undef opus_int64
#undef opus_uint8
#undef opus_int
    static_assert(std::is_same_v<m::opus_int64, raw_int64> && sizeof(m::opus_int64) == 8);
    static_assert(std::is_same_v<m::opus_uint8, raw_uint8> && std::is_same_v<m::opus_int, raw_int>);
#pragma pop_macro("opus_int")
#pragma pop_macro("opus_uint8")
#pragma pop_macro("opus_int64")
}

TEST(HeaderMix, CtlMacrosBesideTheMappings) {
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &err);
    ASSERT_EQ(err, OPUS_OK);
    // The header's comma list, to the variadic function.
    EXPECT_EQ(opus_encoder_ctl(enc, OPUS_SET_BITRATE(48000)), OPUS_OK);
    opus_int32 bitrate = 0;
    // The mapped request, to the overload (found by argument-dependent lookup).
    EXPECT_EQ(opus_encoder_ctl(enc, (m::OPUS_GET_BITRATE)(&bitrate)), OPUS_OK);
    EXPECT_EQ(bitrate, 48000);
    EXPECT_EQ(opus_encoder_ctl(enc, (m::OPUS_SET_COMPLEXITY)(2)), OPUS_OK);
    opus_int32 complexity = 0;
    EXPECT_EQ(opus_encoder_ctl(enc, OPUS_GET_COMPLEXITY(&complexity)), OPUS_OK);
    EXPECT_EQ(complexity, 2);
    // An object-like macro expands even in a qualified name: set it aside to
    // name the constant.
    constexpr auto raw = OPUS_RESET_STATE;
#pragma push_macro("OPUS_RESET_STATE")
#undef OPUS_RESET_STATE
    static_assert(m::OPUS_RESET_STATE == raw);
    EXPECT_EQ(opus_encoder_ctl(enc, m::OPUS_RESET_STATE), OPUS_OK);
#pragma pop_macro("OPUS_RESET_STATE")
    opus_encoder_destroy(enc);
}

TEST(HeaderMix, OpusfileConstants) {
    namespace m = mcpplibs::mwrap::opus::macros;
    // A function-like macro is not expanded when its name is followed by `)`;
    // an object-like one is everywhere, so compare through push_macro.
    constexpr auto raw_eof = OP_EOF, raw_float = OP_DEC_FORMAT_FLOAT;
#pragma push_macro("OP_EOF")
#pragma push_macro("OP_DEC_FORMAT_FLOAT")
#undef OP_EOF
#undef OP_DEC_FORMAT_FLOAT
    static_assert(m::OP_EOF == raw_eof && m::OP_DEC_FORMAT_FLOAT == raw_float);
#pragma pop_macro("OP_DEC_FORMAT_FLOAT")
#pragma pop_macro("OP_EOF")
    int error = 0;
    const unsigned char nothing[1] = {0};
    EXPECT_EQ(op_open_memory(nothing, 1, &error), nullptr);
    EXPECT_EQ(error, OP_ENOTFORMAT);
}
