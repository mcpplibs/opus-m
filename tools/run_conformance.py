#!/usr/bin/env python3
"""Run RFC 6716's decoder conformance tests and the Ogg Opus tool on
examples/opus, programs written against import mcpplibs.mwrap.opus.

The test vectors are RFC 8251's (opus_testvectors-rfc8251.tar.gz from
opus-codec.org: RFC 6716's twelve bitstreams with the updated reference
outputs), downloaded once into target/upstream and checked against the
checksum in upstream/documents.json. As libopus's own tests/run_vectors.sh
does, for each output rate (48, 24, 16, 12 and 8 kHz), in mono and in stereo,
each of the twelve vectors is

  - decoded by examples/opus's opus_vector_decode (opus_demo's decode loop,
    which also requires the range decoder's final state to match the
    encoder's after every packet), and
  - compared by opus_compare (libopus's own, unchanged — checked here to be
    byte for byte the release's src/opus_compare.c) with the reference
    output testvectorNN.dec, or the alternative testvectorNNm.dec RFC 8251
    adds; one of them must pass.

That is 120 decodes. Nothing is excluded. Then the tool: a stereo 48 kHz and a
mono 16 kHz WAV are encoded to Ogg Opus, described and decoded back, and the
decoded length must be exactly the input's (pre-skip and end trimming).
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from inventory import fetch, fetch_url  # noqa: E402

RATES = [48000, 24000, 16000, 12000, 8000]
VECTORS = [f'{i:02d}' for i in range(1, 13)]


def build(example: Path, mcpp: str, toolchain: str | None) -> Path:
    command = [mcpp, 'build', '--profile', 'release'] + (['--toolchain', toolchain] if toolchain else [])
    print('+', ' '.join(command), flush=True)
    subprocess.run(command, cwd=example, check=True)
    suffix = '.exe' if os.name == 'nt' else ''
    found = sorted((p for p in (example / 'target').rglob('opus_vector_decode' + suffix) if p.parent.name == 'bin'),
                   key=lambda p: p.stat().st_mtime)
    if not found:
        sys.exit('examples/opus was not built')
    return found[-1].parent


def wav(path: Path, rate: int, channels: int, seconds: float):
    n = int(rate * seconds)
    frames = bytearray()
    for i in range(n):
        for c in range(channels):
            frames += struct.pack('<h', int(12000 * math.sin(2 * math.pi * 440 * (c + 1) * i / rate)))
    header = b'RIFF' + struct.pack('<I', 36 + len(frames)) + b'WAVEfmt ' + struct.pack(
        '<IHHIIHH', 16, 1, channels, rate, rate * channels * 2, channels * 2, 16) + b'data' + struct.pack('<I', len(frames))
    path.write_bytes(header + frames)
    return n


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mcpp', default='mcpp')
    parser.add_argument('--toolchain')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    pins = json.loads((root / 'upstream/documents.json').read_text(encoding='utf-8'))
    trees = fetch(root)
    compare = (root / 'examples/opus/conformance/opus_compare.c').read_bytes()
    if compare != (trees['opus'] / 'src/opus_compare.c').read_bytes():
        sys.exit('examples/opus/conformance/opus_compare.c is not libopus 1.6.1\'s src/opus_compare.c')
    print('ok   examples/opus/conformance/opus_compare.c is libopus 1.6.1\'s src/opus_compare.c', flush=True)
    vectors = pins['conformance']
    archive = fetch_url(vectors['url'], vectors['sha256'], root / 'target/upstream' / Path(vectors['url']).name)
    print(f'ok   {archive.name} is {vectors["url"]} ({vectors["sha256"][:16]}…)', flush=True)
    vector_dir = root / 'target/upstream/opus_newvectors'
    if not (vector_dir / 'testvector12.bit').exists():
        with tarfile.open(archive) as tar:
            tar.extractall(root / 'target/upstream', filter='data')

    bin_dir = build(root / 'examples/opus', args.mcpp, args.toolchain)
    exe = lambda name: str(bin_dir / (name + ('.exe' if os.name == 'nt' else '')))  # noqa: E731

    failed, runs = [], 0
    with tempfile.TemporaryDirectory(prefix='opus-m-') as scratch:
        out = Path(scratch) / 'out.sw'
        for rate in RATES:
            for channels in (1, 2):
                for v in VECTORS:
                    runs += 1
                    name = f'testvector{v} at {rate} Hz, {"stereo" if channels == 2 else "mono"}'
                    dec = subprocess.run([exe('opus_vector_decode'), str(rate), str(channels),
                                          str(vector_dir / f'testvector{v}.bit'), str(out)],
                                         capture_output=True, text=True, timeout=600)
                    if dec.returncode != 0:
                        failed.append(name)
                        print(f'FAIL {name}: decoding failed\n{dec.stdout}{dec.stderr}', flush=True)
                        continue
                    results = []
                    for reference in (f'testvector{v}.dec', f'testvector{v}m.dec'):
                        cmp = subprocess.run([exe('opus_compare')] + (['-s'] if channels == 2 else []) +
                                             ['-r', str(rate), str(vector_dir / reference), str(out)],
                                             capture_output=True, text=True, timeout=600)
                        results.append((reference, cmp.returncode, (cmp.stdout + cmp.stderr).strip().splitlines()[-1:]))
                        if cmp.returncode == 0:
                            break
                    passed = [r for r in results if r[1] == 0]
                    if passed:
                        print(f'ok   {name}: {passed[0][0]} {passed[0][2][0] if passed[0][2] else ""}', flush=True)
                    else:
                        failed.append(name)
                        print(f'FAIL {name}: {results}', flush=True)

        # The tool: encode, describe, decode; the decoded length is the input's.
        work = Path(scratch)
        for rate, channels in ((48000, 2), (16000, 1)):
            source = work / f'in-{rate}-{channels}.wav'
            samples = wav(source, rate, channels, 1.5)
            encoded, decoded = work / 'tone.opus', work / 'out.wav'
            steps = [[exe('opus'), 'encode', '--bitrate', '64000', str(source), str(encoded)],
                     [exe('opus'), 'info', str(encoded)],
                     [exe('opus'), 'decode', str(encoded), str(decoded)]]
            ok = True
            for step in steps:
                run = subprocess.run(step, capture_output=True, text=True, timeout=300)
                print(run.stdout.rstrip(), flush=True)
                ok = ok and run.returncode == 0
            if ok:
                data = decoded.read_bytes()
                got = (len(data) - 44) // (2 * channels)
                ok = got == samples * 48000 // rate
                print(f'{"ok  " if ok else "FAIL"} the tool at {rate} Hz, {channels} channel(s): {samples} samples in, '
                      f'{got} at 48 kHz out', flush=True)
            if not ok:
                failed.append(f'the tool at {rate} Hz')

    print(f'RFC 6716 / RFC 8251 test vectors: {runs} decodes ({len(VECTORS)} vectors, {len(RATES)} rates, mono and '
          f'stereo), {runs - len([f for f in failed if f.startswith("testvector")])} passed; {len(failed)} failed')
    if failed or runs != len(RATES) * 2 * len(VECTORS):
        sys.exit(1)


if __name__ == '__main__':
    main()
