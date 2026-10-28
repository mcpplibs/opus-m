#!/usr/bin/env python3
"""Write the module sources and the generated tests from the libopus, opusfile and libogg headers.

- src/opus.cppm (mcpplibs.mwrap.opus) exports every type and function of the
  libopus headers by `export using` (the upstream globals, unchanged) and
  re-exports the three modules below.
- src/file.cppm (mcpplibs.mwrap.opus.file): every type and function of
  <opusfile.h>, but libopusurl's (see inventory.URL_*).
- src/ogg.cppm (mcpplibs.mwrap.opus.ogg): every type and function of
  <ogg/ogg.h> and <ogg/os_types.h>.
- src/macros.cppm (mcpplibs.mwrap.opus.macros) holds, in
  mcpplibs::mwrap::opus::macros under their own names:
    * the object-like integer macros (OPUS_OK, OPUS_APPLICATION_AUDIO,
      OPUS_SET_BITRATE_REQUEST, OPUS_RESET_STATE, OP_EOF, OP_DEC_FORMAT_FLOAT
      …) as `inline constexpr` of the type of their expansion;
    * the type macros of opus_types.h (opus_int, opus_uint, and opus_int8,
      opus_uint8, opus_int64, opus_uint64, which are macros or typedefs
      depending on the compiler) as type aliases;
    * the CTL macros (OPUS_SET_BITRATE(x), OPUS_GET_LOOKAHEAD(x),
      OPUS_MULTISTREAM_GET_ENCODER_STATE(x, y) …), which expand to the
      request and its type-checked arguments, a comma list for the variadic
      *_ctl functions: each is a constexpr function returning a
      `ctl_request<…>` of the request and its arguments, of the types the
      macro's checks require, and each *_ctl function has an overload taking
      one, so `opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000))` reads as it
      does with the header.
- tests/macros_mix_test.cpp checks each mapping against the raw header in a
  translation unit that includes it beside the import.
- tests/link_test.cpp takes the address of every exported function, through
  the module only: each is defined by the compat libraries.

With --check, nothing is written and a difference fails.
"""
import argparse
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from inventory import HEADERS, URL_MACROS, fetch, surface  # noqa: E402

GUARDS = {'OPUS_TYPES_H', 'OPUS_DEFINES_H', 'OPUS_H', 'OPUS_MULTISTREAM_H', 'OPUS_PROJECTION_H', '_OS_TYPES_H', '_OGG_H',
          '_opusfile_h'}

# Macros that stay macros, in <mwrap/opus/macros.hpp>.
SIDE_HEADER = {
    'OPUS_EXPORT': 'an attribute of declarations (visibility / dllexport when building libopus)',
    'OPUS_RESTRICT': 'a keyword spelling (restrict / __restrict__ / __restrict)',
    'OPUS_INLINE': 'a keyword spelling (inline / __inline__ / __inline)',
    'OPUS_WARN_UNUSED_RESULT': 'an attribute of declarations',
    'OPUS_ARG_NONNULL': 'an attribute of declarations',
    'OPUS_GNUC_PREREQ': 'a preprocessor test of the compiler version, for #if',
    'OPUS_HAVE_OPUS_PROJECTION_H': 'an empty feature-test macro, for #ifdef',
    'opus_check_int': 'a type check inside the CTL macros; the mapped CTL functions take an opus_int32',
    'opus_check_int_ptr': 'a type check inside the CTL macros; the mapped CTL functions take an opus_int32 *',
    'opus_check_uint_ptr': 'a type check inside the CTL macros; the mapped CTL functions take an opus_uint32 *',
    'opus_check_uint8_ptr': 'a type check for CTL macros; no public CTL macro uses it',
    'opus_check_val16_ptr': 'a type check for CTL macros of libopus\'s own (opus_val16 is not a public type)',
    'opus_check_void_ptr': 'a type check inside the CTL macros; the mapped CTL functions take a const void *',
    'opus_check_encstate_ptr': 'a type check inside the CTL macros; the mapped CTL functions take an OpusEncoder **',
    'opus_check_decstate_ptr': 'a type check inside the CTL macros; the mapped CTL functions take an OpusDecoder **',
    'OP_GNUC_PREREQ': 'a preprocessor test of the compiler version, for #if',
    'OP_WARN_UNUSED_RESULT': 'an attribute of declarations',
    'OP_ARG_NONNULL': 'an attribute of declarations',
    '_ogg_malloc': 'the allocator libogg\'s own sources call (malloc)',
    '_ogg_calloc': 'the allocator libogg\'s own sources call (calloc)',
    '_ogg_realloc': 'the allocator libogg\'s own sources call (realloc)',
    '_ogg_free': 'the allocator libogg\'s own sources call (free)',
}

# opus_types.h: `#define opus_int int`, `#define opus_uint unsigned int`, and
# opus_int8 / opus_uint8 / opus_int64 / opus_uint64 as macros that the header
# #undefs and replaces with <stdint.h> typedefs when it decides stdint.h is
# there (C99, or GCC-compatible compilers once <stdint.h> was included).
TYPE_MACROS = ['opus_int', 'opus_uint', 'opus_int8', 'opus_uint8', 'opus_int64', 'opus_uint64']

# The argument checks of the CTL macros, and the type each requires.
CHECKS = {
    'opus_check_int': 'opus_int32',
    'opus_check_int_ptr': 'opus_int32 *',
    'opus_check_uint_ptr': 'opus_uint32 *',
    'opus_check_uint8_ptr': 'opus_uint8 *',
    'opus_check_void_ptr': 'const void *',
    'opus_check_encstate_ptr': 'OpusEncoder **',
    'opus_check_decstate_ptr': 'OpusDecoder **',
}
# An argument a CTL macro passes unchecked, and the type its documentation gives.
UNCHECKED = {('OPUS_PROJECTION_GET_DEMIXING_MATRIX', 'x'): 'unsigned char *'}

# The variadic *_ctl functions and the state each takes.
CTL_FUNCTIONS = {
    'opus_encoder_ctl': 'OpusEncoder',
    'opus_decoder_ctl': 'OpusDecoder',
    'opus_dred_decoder_ctl': 'OpusDREDDecoder',
    'opus_multistream_encoder_ctl': 'OpusMSEncoder',
    'opus_multistream_decoder_ctl': 'OpusMSDecoder',
    'opus_projection_encoder_ctl': 'OpusProjectionEncoder',
    'opus_projection_decoder_ctl': 'OpusProjectionDecoder',
}


def excluded(name: str) -> str | None:
    if name in GUARDS:
        return 'include guard'
    if name in URL_MACROS:
        return 'libopusurl (HTTP streaming), which compat.opusfile does not build'
    return None


def split_args(text: str) -> list[str]:
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
            continue
        depth += ch == '('
        depth -= ch == ')'
        cur += ch
    out.append(cur.strip())
    return out


def ctl_args(macro) -> list[tuple[str, str]]:
    """The CTL macro's arguments after the request: (parameter, type)."""
    args = split_args(macro.body)[1:]
    out = []
    for a in args:
        m = re.fullmatch(r'(\w+)\((\w+)\)', a)
        if m and m.group(1) in CHECKS:
            out.append((m.group(2), CHECKS[m.group(1)]))
        elif a in (macro.params or []) and (macro.name, a) in UNCHECKED:
            out.append((a, UNCHECKED[(macro.name, a)]))
        else:
            sys.exit(f'{macro.name}: an argument the generator does not know: {a}')
    if [p for p, _ in out] != macro.params:
        sys.exit(f'{macro.name}: the arguments are not the parameters in order')
    return out


def classify(sf):
    constants, types, ctls = [], [], []
    for macro in sf.macros.values():
        if macro.name in SIDE_HEADER or excluded(macro.name):
            continue
        if macro.name in TYPE_MACROS:
            types.append(macro.name)
        elif macro.params is not None:
            if not re.match(r'\w+_REQUEST\s*,', macro.body):
                sys.exit(f'{macro.name}: a function-like macro that is not a CTL macro')
            ctls.append(macro)
        elif re.fullmatch(r'\(?\s*-?\s*\d+\s*\)?', macro.body):
            constants.append(macro.name)
        else:
            sys.exit(f'{macro.name}: an object-like macro that is not an integer: {macro.body}')
    return constants, types, ctls


GUARD = '#include <mwrap/opus/macros.hpp>'

PREAMBLE = '''// Generated by tools/gen_exports.py from the libopus 1.6.1, opusfile 0.12 and
// libogg 1.3.6 headers — do not edit by hand.
//
'''


def render_root(sf) -> str:
    out = [PREAMBLE + '''// mcpplibs.mwrap.opus — libopus 1.6.1, the Opus audio codec (RFC 6716), as a
// C++ module: every type and function of <opus.h>, <opus_multistream.h> and
// <opus_projection.h> (with <opus_types.h> and <opus_defines.h>), the upstream
// globals unchanged — the encoder and decoder, multistream and ambisonics
// projection, the repacketizer, the packet parsers and the DRED API. It
// re-exports the Ogg Opus file reader (mcpplibs.mwrap.opus.file), the Ogg
// container (mcpplibs.mwrap.opus.ogg) and the macro mappings
// (mcpplibs.mwrap.opus.macros). The libraries are compat.opus,
// compat.opusfile and compat.ogg, built from the same releases.
module;
''' + GUARD + '''

export module mcpplibs.mwrap.opus;

export import mcpplibs.mwrap.opus.file;
export import mcpplibs.mwrap.opus.ogg;
export import mcpplibs.mwrap.opus.macros;
''']
    out.append(f'// types ({len(sf.types["codec"])}; opus_int8, opus_uint8, opus_int64 and opus_uint64 are in the macros module)')
    out += [f'export using ::{n};' for n in sf.types['codec']]
    out.append(f'\n// functions ({len(sf.functions["codec"])})')
    out += [f'export using ::{n};' for n in sf.functions['codec']]
    return '\n'.join(out) + '\n'


def render_file(sf) -> str:
    out = [PREAMBLE + '''// mcpplibs.mwrap.opus.file — opusfile 0.12, reading Ogg Opus files (RFC 7845):
// every type and function of <opusfile.h>, the upstream globals unchanged —
// opening files, memory and callbacks, OpusHead / OpusTags / OpusPictureTag,
// seeking, and reading 16-bit or float PCM. Not exported: libopusurl's
// declarations (OpusServerInfo, op_url_stream_create, op_open_url,
// op_test_url and their va_list forms, opus_server_info_init / _clear),
// which compat.opusfile does not build.
module;
''' + GUARD + '''

export module mcpplibs.mwrap.opus.file;
''']
    out.append(f'// types ({len(sf.types["file"])})')
    out += [f'export using ::{n};' for n in sf.types['file']]
    out.append(f'\n// functions ({len(sf.functions["file"])})')
    out += [f'export using ::{n};' for n in sf.functions['file']]
    return '\n'.join(out) + '\n'


def render_ogg(sf) -> str:
    out = [PREAMBLE + '''// mcpplibs.mwrap.opus.ogg — libogg 1.3.6, the Ogg container: every type and
// function of <ogg/ogg.h> and <ogg/os_types.h>, the upstream globals
// unchanged — pages and packets (ogg_stream_*, ogg_sync_*, ogg_page_*), and
// the bit packers (oggpack_*, oggpackB_*). What an Ogg Opus writer pages its
// packets with, and the types <opusfile.h> declares its positions in.
module;
''' + GUARD + '''

export module mcpplibs.mwrap.opus.ogg;
''']
    out.append(f'// types ({len(sf.types["ogg"])})')
    out += [f'export using ::{n};' for n in sf.types['ogg']]
    out.append(f'\n// functions ({len(sf.functions["ogg"])})')
    out += [f'export using ::{n};' for n in sf.functions['ogg']]
    return '\n'.join(out) + '\n'


MACROS_HEADER = PREAMBLE + '''// mcpplibs.mwrap.opus.macros — what the libopus, opusfile and libogg headers
// define as macros, under the original names in mcpplibs::mwrap::opus::macros:
//
//   - integer macros (OPUS_OK, OPUS_BAD_ARG, OPUS_APPLICATION_AUDIO,
//     OPUS_BANDWIDTH_FULLBAND, OPUS_FRAMESIZE_20_MS, OPUS_AUTO, OPUS_RESET_STATE,
//     every *_REQUEST, OP_EOF, OP_HOLE, OP_DEC_FORMAT_FLOAT, OP_TRACK_GAIN,
//     OPUS_CHANNEL_COUNT_MAX …) as `inline constexpr`, each of the type its
//     macro expands to (all `int`);
//   - the type macros of <opus_types.h> as type aliases: opus_int (int),
//     opus_uint (unsigned int), and opus_int8, opus_uint8, opus_int64 and
//     opus_uint64, which the header leaves macros or replaces with <stdint.h>
//     typedefs depending on the compiler;
//   - the CTL macros. OPUS_SET_BITRATE(x) expands to
//     `OPUS_SET_BITRATE_REQUEST, opus_check_int(x)`, the arguments of the
//     variadic opus_encoder_ctl. Here it is a constexpr function returning a
//     ctl_request<opus_int32>: the request and its argument, of the type the
//     macro's check requires (opus_int32, opus_int32 *, opus_uint32 *,
//     OpusEncoder ** …). Every *_ctl function has an overload taking a
//     ctl_request, found by argument-dependent lookup, so
//     `opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000))` and
//     `opus_decoder_ctl(dec, OPUS_GET_FINAL_RANGE(&range))` read as they do
//     with the header, and pass the same arguments.
//
// Attributes, keyword spellings, the compiler-version tests and the CTL
// macros' argument checks stay macros, in <mwrap/opus/macros.hpp>, included
// before the import. tools/check_exports.py accounts for every macro.
module;
''' + GUARD + '''
'''

CTL_SUPPORT = '''
// ── CTL requests ────────────────────────────────────────────────────────────
// What a CTL macro expands to: the request and its arguments.
template <class... Args>
struct ctl_request;
template <>
struct ctl_request<> {
    int request;
};
template <class A>
struct ctl_request<A> {
    int request;
    A a;
};
template <class A, class B>
struct ctl_request<A, B> {
    int request;
    A a;
    B b;
};

}  // namespace mcpplibs::mwrap::opus::macros

namespace mwrap_opus_ctl {
// The variadic call a CTL macro's comma list makes.
template <class F, class State, class... Args>
int call(F f, State* st, const mcpplibs::mwrap::opus::macros::ctl_request<Args...>& r) noexcept {
    if constexpr (sizeof...(Args) == 0)
        return f(st, r.request);
    else if constexpr (sizeof...(Args) == 1)
        return f(st, r.request, r.a);
    else
        return f(st, r.request, r.a, r.b);
}
}  // namespace mwrap_opus_ctl

export namespace mcpplibs::mwrap::opus::macros {
'''


def ctl_function(macro) -> list[str]:
    args = ctl_args(macro)
    types = ', '.join(t for _, t in args)
    params = ', '.join(f'{t} {p}' for p, t in args)
    request = split_args(macro.body)[0]
    init = ', '.join([request] + [p for p, _ in args])
    return [f'// #define {macro.name}({", ".join(macro.params)}) {macro.body}',
            f'constexpr ctl_request<{types}> ({macro.name})({params}) noexcept {{ return {{{init}}}; }}']


def render_macros(sf) -> str:
    constants, types, ctls = classify(sf)
    out = [MACROS_HEADER]
    out.append('export module mcpplibs.mwrap.opus.macros;\n')
    out.append('// The headers\' values and types, taken while their macros are defined (module linkage).')
    out.append('namespace mwrap_opus_capture {')
    out += [f'inline constexpr auto value_{n} = {n};' for n in constants]
    out += [f'using type_{n} = {n};' for n in types]
    out.append('}  // namespace mwrap_opus_capture\n')
    out.append('export namespace mcpplibs::mwrap::opus::macros {')
    out.append(CTL_SUPPORT)
    out.append('// ── the *_ctl functions, taking a ctl_request ────────────────────────────────')
    for f, state in CTL_FUNCTIONS.items():
        out += ['template <class... Args>',
                f'int {f}({state}* st, ctl_request<Args...> r) noexcept {{ return mwrap_opus_ctl::call(::{f}, st, r); }}']
    out.append('\n// ── the CTL macros, defined while the headers\' macros are ─────────────────────')
    for macro in ctls:
        out += ctl_function(macro)
    out.append('\n}  // namespace mcpplibs::mwrap::opus::macros\n')
    out.append('// Every mapped object-like name, so the definitions below are not rewritten.')
    out += [f'#undef {n}' for n in constants + types]
    out.append('\nnamespace c = mwrap_opus_capture;\n')
    out.append('export namespace mcpplibs::mwrap::opus::macros {\n')
    out.append('// ── the type macros of <opus_types.h> ─────────────────────────────────────────')
    out += [f'using {n} = c::type_{n};' for n in types]
    out.append('\n// ── constants ───────────────────────────────────────────────────────────────')
    for header in [h.split('/', 1)[1] for hs in HEADERS.values() for _, h in hs]:
        names = [n for n in constants if sf.macros[n].header == header]
        if names:
            out.append(f'// {header}')
            out += [f'inline constexpr auto {n} = c::value_{n};' for n in names]
    out.append('\n}  // namespace mcpplibs::mwrap::opus::macros')
    return '\n'.join(out) + '\n'


TEST_HEADER = PREAMBLE + '''// Every mapping against the raw headers, in one translation unit that includes
// <mwrap/opus/macros.hpp> (that is, <opus.h>, <opus_multistream.h>,
// <opus_projection.h> and <opusfile.h>) beside the import:
//
//   - each constant has its macro's type and value, and each type alias is
//     the type its macro (or typedef) names (the macro is set aside with
//     push_macro around the comparison: while it is defined it expands even
//     inside `m::NAME`);
//   - each CTL function returns the request and arguments its macro's comma
//     list passes, of the same types and values; a function-like macro is
//     not expanded when its name is followed by `)`, so `(m::NAME)(args)`
//     calls the function beside the macro `NAME(args)`.
#include <mwrap/opus/macros.hpp>
#include <gtest/gtest.h>

import std;
import mcpplibs.mwrap.opus;

namespace m = mcpplibs::mwrap::opus::macros;

namespace {

// The arguments a CTL macro's comma list passes, as a tuple of their types.
template <class... T>
constexpr std::tuple<T...> args(T... v) { return {v...}; }
template <class... A, std::size_t... I>
constexpr auto args_of(const m::ctl_request<A...>& r, std::index_sequence<I...>) {
    if constexpr (sizeof...(A) == 0) return std::tuple<int>{r.request};
    else if constexpr (sizeof...(A) == 1) return std::tuple<int, A...>{r.request, r.a};
    else return std::tuple<int, A...>{r.request, r.a, r.b};
}
template <class... A>
constexpr auto args_of(const m::ctl_request<A...>& r) { return args_of(r, std::index_sequence_for<A...>{}); }

// One sample of each argument type.
opus_int32 int_value = 0;
opus_uint32 uint_value = 0;
OpusEncoder* encoder_state = nullptr;
OpusDecoder* decoder_state = nullptr;
unsigned char matrix[8] = {};

}  // namespace
'''

SAMPLES = {
    'opus_int32': 'opus_int32{7}',
    'opus_int32 *': '&int_value',
    'opus_uint32 *': '&uint_value',
    'opus_uint8 *': 'static_cast<opus_uint8 *>(matrix)',
    'const void *': 'static_cast<const void *>(matrix)',
    'OpusEncoder **': '&encoder_state',
    'OpusDecoder **': '&decoder_state',
    'unsigned char *': 'matrix',
}


def render_test(sf) -> str:
    constants, types, ctls = classify(sf)
    out = [TEST_HEADER]
    out.append('// ── constants ───────────────────────────────────────────────────────────────')
    for n in constants:
        out.append(f'constexpr auto raw_{n} = {n};')
    for n in constants:
        out += [f'#pragma push_macro("{n}")', f'#undef {n}',
                f'static_assert(std::is_same_v<decltype(m::{n}), decltype(raw_{n})> && m::{n} == raw_{n});',
                f'#pragma pop_macro("{n}")']
    out.append('\n// ── the type macros of <opus_types.h> ─────────────────────────────────────────')
    for n in types:
        out.append(f'using raw_type_{n} = {n};')
    for n in types:
        out += [f'#pragma push_macro("{n}")', f'#undef {n}',
                f'static_assert(std::is_same_v<m::{n}, raw_type_{n}>);',
                f'#pragma pop_macro("{n}")']
    out.append('\n// ── CTL macros ──────────────────────────────────────────────────────────────')
    for macro in ctls:
        values = ', '.join(SAMPLES[t] for _, t in ctl_args(macro))
        out += [f'TEST(Ctl, {macro.name}) {{',
                f'    auto raw = args({macro.name}({values}));',
                f'    auto mapped = args_of((m::{macro.name})({values}));',
                '    static_assert(std::is_same_v<decltype(raw), decltype(mapped)>);',
                '    EXPECT_TRUE(raw == mapped);',
                '}']
    return '\n'.join(out) + '\n'


def render_link_test(sf) -> str:
    out = [PREAMBLE + '''// The address of every exported function, through the module only (no header):
// each is defined by compat.opus, compat.opusfile or compat.ogg, so a program
// that calls any of them links.
import std;
import mcpplibs.mwrap.opus;

using fn = void (*)();

int main() {
    const fn functions[] = {''']
    for module in HEADERS:
        out.append(f'        // {module} ({len(sf.functions[module])})')
        out += [f'        reinterpret_cast<fn>(&::{n}),' for n in sf.functions[module]]
    out.append('''    };
    std::size_t defined = 0;
    for (fn f : functions) defined += f != nullptr;
    std::println("{} of {} functions defined", defined, std::size(functions));
    return defined == std::size(functions) ? 0 : 1;
}''')
    return '\n'.join(out) + '\n'


def outputs(root: Path) -> dict[Path, str]:
    sf = surface(fetch(root))
    # opus_int8 / opus_uint8 / opus_int64 / opus_uint64 are typedefs only on some
    # compilers: they are the macros module's type aliases, not exported here.
    sf.types['codec'] = [t for t in sf.types['codec'] if t not in TYPE_MACROS]
    return {
        root / 'src/opus.cppm': render_root(sf),
        root / 'src/file.cppm': render_file(sf),
        root / 'src/ogg.cppm': render_ogg(sf),
        root / 'src/macros.cppm': render_macros(sf),
        root / 'tests/macros_mix_test.cpp': render_test(sf),
        root / 'tests/link_test.cpp': render_link_test(sf),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    stale = []
    for path, text in outputs(root).items():
        # Text mode: a checkout with CRLF line ends (Windows) reads the same.
        current = path.read_text(encoding='utf-8') if path.exists() else None
        if current == text:
            continue  # unchanged: the timestamp is kept, nothing rebuilds
        if args.check:
            stale.append(path.relative_to(root).as_posix())
        else:
            path.write_text(text, encoding='utf-8', newline='\n')
            print('wrote', path.relative_to(root).as_posix())
    if stale:
        sys.exit('not what tools/gen_exports.py writes: ' + ', '.join(stale))


if __name__ == '__main__':
    main()
