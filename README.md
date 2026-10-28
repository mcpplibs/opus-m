# opus

> Opus audio — the libopus codec, the opusfile reader and the libogg container — through C++23 modules.

**English** · [简体中文](README.zh.md) · [Upstream](https://opus-codec.org) · [Releases](https://github.com/mcpplibs/opus-m/releases) · [mcpp](https://github.com/mcpp-community/mcpp) · [Index](https://github.com/mcpplibs/mcpp-index)

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Opus](https://img.shields.io/badge/libopus%20%C2%B7%20opusfile%20%C2%B7%20libogg-BSD--3--Clause-blue.svg)](upstream/opus/COPYING)
[![mcpp-community](https://img.shields.io/badge/mcpp--community-mwrap-blue)](https://github.com/mcpp-community)
[![CI](https://github.com/mcpplibs/opus-m/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/mcpplibs/opus-m/actions/workflows/ci.yml)

Opus (RFC 6716) is the royalty-free codec for speech and music, from 6 kb/s
voice to transparent stereo. This package is libopus 1.6.1 (encoder and
decoder, multistream, ambisonics, repacketizer), opusfile 0.12 (reading and
seeking `.opus` files, RFC 7845) and libogg 1.3.6, as `compat.opus`,
`compat.opusfile` and `compat.ogg`. All three are BSD-3-Clause and link
statically. The C API is unchanged: `opus_encoder_create`, `opus_decode_float`,
`op_open_memory` and `ogg_stream_packetin` are the upstream globals. The macros
become constants, type aliases and typed CTL requests, so
`opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000))` reads as it does in C. Every
CI run decodes RFC 8251's twelve test vectors at five rates, mono and stereo,
through the module, and checks them with libopus's own `opus_compare`.

## Modules

- `import mcpplibs.mwrap.opus;`: libopus (`<opus.h>`, `<opus_multistream.h>`, `<opus_projection.h>`), re-exporting the three modules below
- `import mcpplibs.mwrap.opus.file;`: opusfile (`<opusfile.h>`): `op_open_file`, `op_open_memory`, `op_read_float`, `op_pcm_seek`, `op_head`, `op_tags` …
- `import mcpplibs.mwrap.opus.ogg;`: libogg (`<ogg/ogg.h>`): `ogg_stream_*`, `ogg_sync_*`, `ogg_page_*`, `oggpack_*`
- `import mcpplibs.mwrap.opus.macros;`: `OPUS_OK`, `OPUS_APPLICATION_AUDIO`, `OPUS_SET_BITRATE`, `OP_EOF`, `opus_int64` … in `mcpplibs::mwrap::opus::macros`

> More Modules API -> [Usage and API](docs/usage.md)

## Getting started

```sh
mcpp new tone --template mcpplibs.mwrap.opus@1.6.1
cd tone
mcpp run
```

The default template encodes two seconds of a chord with libopus, pages it
into an Ogg Opus stream with libogg, and reads it back with opusfile. It
prints the headers, the exact length and the signal-to-noise ratio, all in
memory. Run `mcpp run -- tone.opus` to write the stream to a file too.

```toml
[dependencies]
"mcpplibs.mwrap.opus" = "1.6.1"
```

```cpp
import std;
import mcpplibs.mwrap.opus;
using namespace mcpplibs::mwrap::opus::macros;  // OPUS_SET_BITRATE, OPUS_APPLICATION_AUDIO …

int main(int argc, char** argv) {
    int error = 0;
    OggOpusFile* file = op_open_file(argc > 1 ? argv[1] : "music.opus", &error);
    if (!file) return std::println("not an Ogg Opus file ({})", error), 1;
    std::vector<float> pcm(5760 * 2);  // 120 ms of stereo at 48 kHz
    long samples = 0;
    for (int n; (n = op_read_float_stereo(file, pcm.data(), int(pcm.size()))) > 0;) samples += n;
    std::println("{} samples, {} channel(s), \"{}\"", samples, op_channel_count(file, -1), op_tags(file, -1)->vendor);
    op_free(file);

    OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000));
    opus_int32 bitrate = 0;
    opus_encoder_ctl(enc, OPUS_GET_BITRATE(&bitrate));
    std::println("{}, {} b/s", opus_get_version_string(), bitrate);
    opus_encoder_destroy(enc);
}
```

## Macros and CTLs

Constants such as `OPUS_OK` and `OP_EOF` keep the type of their macro
(`int`). The CTL macros expand to a request and its type-checked arguments
for the variadic `opus_*_ctl` functions. Here they are constexpr functions
returning a `ctl_request` of the same values and types, and each `*_ctl`
function has an overload that takes one. `opus_int8`, `opus_int64` and their
unsigned twins are macros or typedefs depending on the compiler, so they are
type aliases here. Attributes, `OPUS_GNUC_PREREQ` and the CTL argument checks
stay macros in `<mwrap/opus/macros.hpp>`, included before the import. Details:
[docs/usage.md](docs/usage.md).

## What is built

Upstream's default float build of libopus. Deep PLC, DRED, OSCE and QEXT are
off. The code is plain C, without the per-file x86 intrinsics or run-time CPU
detection. opusfile is libopusfile only: libopusurl (HTTP over OpenSSL) is not
built, so its declarations are not exported.

## Examples

- [examples/opus](examples/opus): an Ogg Opus tool (`opus encode`, `opus decode`, `opus info`) and the RFC 6716
  decoder conformance runner, both written against the module.
  [tools/run_conformance.py](tools/run_conformance.py) runs them.

## Build and testing

The CI runs on Linux (x86_64 GCC and LLVM, arm64 GCC), macOS and Windows. It
builds the modules and runs the tests, which check every macro mapping against
the headers and take the address of each of the 212 exported functions. The
export audit ([tools/check_exports.py](tools/check_exports.py)) checks that
the generated sources are fresh, that every declaration is exported and that
every macro is mapped or listed with its reason. The CI then runs RFC 8251's
test vectors: 120 decodes, each with the range decoder's final state checked
after every packet and the output compared with the reference. It also encodes,
describes and decodes WAV files with the tool. A separate job scaffolds the
default template as an installed package and runs it.

## License

[Apache-2.0](LICENSE) for the wrapper, by mcpp-community ([NOTICE](NOTICE)).
libopus ([COPYING](upstream/opus/COPYING), with its royalty-free patent
licenses), opusfile ([COPYING](upstream/opusfile/COPYING)) and libogg
([COPYING](upstream/ogg/COPYING)) are BSD-3-Clause. Their notices are deployed
beside every program that depends on this package.
