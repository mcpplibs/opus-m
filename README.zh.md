# opus

> Opus 音频——libopus 编解码器、opusfile 读取库与 libogg 容器——以 C++23 模块使用。

[English](README.md) · **简体中文** · [上游](https://opus-codec.org) · [发布](https://github.com/mcpplibs/opus-m/releases) · [mcpp](https://github.com/mcpp-community/mcpp) · [索引](https://github.com/mcpplibs/mcpp-index)

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Opus](https://img.shields.io/badge/libopus%20%C2%B7%20opusfile%20%C2%B7%20libogg-BSD--3--Clause-blue.svg)](upstream/opus/COPYING)
[![mcpp-community](https://img.shields.io/badge/mcpp--community-mwrap-blue)](https://github.com/mcpp-community)
[![CI](https://github.com/mcpplibs/opus-m/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/mcpplibs/opus-m/actions/workflows/ci.yml)

Opus(RFC 6716)是用于语音与音乐的免版税编解码器,从 6 kb/s 的语音到透明音质的立体声。本包包含
libopus 1.6.1(编码器与解码器、multistream、ambisonics、repacketizer)、opusfile 0.12(读取与定位
`.opus` 文件,RFC 7845)和 libogg 1.3.6,分别由 `compat.opus`、`compat.opusfile`、`compat.ogg` 提供。
三者均为 BSD-3-Clause,可静态链接。C API 保持不变:`opus_encoder_create`、`opus_decode_float`、
`op_open_memory`、`ogg_stream_packetin` 都是上游的全局名。宏映射为常量、类型别名和带类型的 CTL 请求,
因此 `opus_encoder_ctl(enc, OPUS_SET_BITRATE(64000))` 的写法与 C 中一致。每次 CI 都经由模块以五种采样率、
单声道与立体声解码 RFC 8251 的十二个测试向量,并用 libopus 自带的 `opus_compare` 检查结果。

## 模块

- `import mcpplibs.mwrap.opus;`:libopus(`<opus.h>`、`<opus_multistream.h>`、`<opus_projection.h>`),并重新导出下面三个模块
- `import mcpplibs.mwrap.opus.file;`:opusfile(`<opusfile.h>`):`op_open_file`、`op_open_memory`、`op_read_float`、`op_pcm_seek`、`op_head`、`op_tags` …
- `import mcpplibs.mwrap.opus.ogg;`:libogg(`<ogg/ogg.h>`):`ogg_stream_*`、`ogg_sync_*`、`ogg_page_*`、`oggpack_*`
- `import mcpplibs.mwrap.opus.macros;`:`OPUS_OK`、`OPUS_APPLICATION_AUDIO`、`OPUS_SET_BITRATE`、`OP_EOF`、`opus_int64` …,位于 `mcpplibs::mwrap::opus::macros`

> 更多模块 API -> [用法与 API](docs/usage.md)

## 快速开始

```sh
mcpp new tone --template mcpplibs.mwrap.opus@1.6.1
cd tone
mcpp run
```

默认模板用 libopus 编码两秒和弦,用 libogg 分页为 Ogg Opus 流,再用 opusfile 读回,打印头信息、精确长度
和信噪比,全部在内存中完成。运行 `mcpp run -- tone.opus` 还会把流写入文件。

```toml
[dependencies]
"mcpplibs.mwrap.opus" = "1.6.1"
```

```cpp
import std;
import mcpplibs.mwrap.opus;
using namespace mcpplibs::mwrap::opus::macros;  // OPUS_SET_BITRATE、OPUS_APPLICATION_AUDIO …

int main(int argc, char** argv) {
    int error = 0;
    OggOpusFile* file = op_open_file(argc > 1 ? argv[1] : "music.opus", &error);
    if (!file) return std::println("not an Ogg Opus file ({})", error), 1;
    std::vector<float> pcm(5760 * 2);  // 48 kHz 立体声的 120 ms
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

## 宏与 CTL

`OPUS_OK`、`OP_EOF` 等常量保持宏的类型(`int`)。CTL 宏会展开为请求号及其经过类型检查的参数,
供可变参数的 `opus_*_ctl` 函数使用;这里它们是 constexpr 函数,返回值与类型都相同的 `ctl_request`,
每个 `*_ctl` 函数都有接受它的重载。`opus_int8`、`opus_int64` 及其无符号版本随编译器不同可能是宏也可能是
typedef,这里是类型别名。属性、`OPUS_GNUC_PREREQ` 和 CTL 参数检查保留为宏,在 `<mwrap/opus/macros.hpp>`
中,于 import 之前包含。详见 [docs/usage.md](docs/usage.md)。

## 构建内容

libopus 采用上游默认的浮点构建;deep PLC、DRED、OSCE 与 QEXT 关闭。代码为纯 C,不使用需逐文件设置的 x86
内建函数,也不做运行时 CPU 检测。opusfile 只构建 libopusfile:libopusurl(基于 OpenSSL 的 HTTP)不构建,
其声明也不导出。

## 示例

- [examples/opus](examples/opus):Ogg Opus 工具(`opus encode`、`opus decode`、`opus info`)和 RFC 6716 解码器
  一致性测试,均基于模块编写;[tools/run_conformance.py](tools/run_conformance.py) 运行它们。

## 构建与测试

CI 在 Linux(x86_64 GCC 与 LLVM、arm64 GCC)、macOS 和 Windows 上运行。它构建模块并运行测试:测试对照头文件
检查每个宏映射,并取全部 212 个导出函数的地址。导出审计([tools/check_exports.py](tools/check_exports.py))
检查生成的源码是最新的、每个声明都已导出、每个宏都已映射或注明原因。之后 CI 运行 RFC 8251 的测试向量:
共 120 次解码,每个包之后都检查区间解码器的最终状态,并把输出与参考结果比较。它还用示例工具编码、查看并
解码 WAV 文件。另有一个作业把默认模板作为已安装的包生成并运行。

## 许可证

封装为 [Apache-2.0](LICENSE),由 mcpp-community 提供([NOTICE](NOTICE))。libopus
([COPYING](upstream/opus/COPYING),含其免版税专利许可)、opusfile([COPYING](upstream/opusfile/COPYING))
和 libogg([COPYING](upstream/ogg/COPYING))均为 BSD-3-Clause;它们的许可文件会部署到每个依赖本包的程序旁。
