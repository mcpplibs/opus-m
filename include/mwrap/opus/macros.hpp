// <mwrap/opus/macros.hpp> — the libopus, opusfile and libogg headers as the
// modules see them, for the macros a C++ entity cannot be: the attributes and
// keyword spellings (OPUS_EXPORT, OPUS_WARN_UNUSED_RESULT, OPUS_ARG_NONNULL,
// OPUS_RESTRICT, OPUS_INLINE, OP_WARN_UNUSED_RESULT, OP_ARG_NONNULL), the
// compiler-version tests (OPUS_GNUC_PREREQ, OP_GNUC_PREREQ), the feature test
// OPUS_HAVE_OPUS_PROJECTION_H, the CTL macros' argument checks
// (opus_check_int …) and libogg's allocator names (_ogg_malloc …).
//
// Include it before `import mcpplibs.mwrap.opus;`, and rather than <opus.h>
// itself: <stdint.h> comes first, as in every module of this package, so
// <opus_types.h> makes the same choice here as there (whether opus_int8,
// opus_uint8, opus_int64 and opus_uint64 are <stdint.h> typedefs or macros
// depends on what was included before it). Every other macro is defined too;
// while a macro is defined it expands even inside
// `mcpplibs::mwrap::opus::macros::NAME`, and a function-like one does not when
// its name is followed by `)`: `(macros::OPUS_SET_BITRATE)(x)` is the mapped
// function, `OPUS_SET_BITRATE(x)` the header's comma list.
#pragma once
#include <stdint.h>
#include <opus.h>
#include <opus_multistream.h>
#include <opus_projection.h>
#include <opusfile.h>
