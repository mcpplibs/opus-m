#!/usr/bin/env python3
"""Audit the module surface against the pinned releases' public headers.

- src/opus.cppm, src/file.cppm, src/ogg.cppm, src/macros.cppm,
  tests/macros_mix_test.cpp and tests/link_test.cpp are what
  tools/gen_exports.py writes.
- Every type and function of the installed headers is exported by
  `export using` from its module (libopus from mcpplibs.mwrap.opus, opusfile
  from .file, libogg from .ogg), and nothing else; libopusurl's declarations
  are the only ones left out, and each is listed.
- Every macro of the headers is mapped in src/macros.cppm (a constant, a type
  alias or a CTL function), kept as a macro in <mwrap/opus/macros.hpp> with
  its reason, libopusurl's, or an include guard; each mapping is checked
  against the raw header by tests/macros_mix_test.cpp, and each exported
  function's address is taken by tests/link_test.cpp.
- Every variadic *_ctl function has its ctl_request overload.
"""
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from inventory import URL_FUNCTIONS, URL_MACROS, URL_TYPES, fetch, surface  # noqa: E402
from gen_exports import CTL_FUNCTIONS, SIDE_HEADER, TYPE_MACROS, excluded  # noqa: E402

MODULES = {'codec': 'src/opus.cppm', 'file': 'src/file.cppm', 'ogg': 'src/ogg.cppm'}


def main():
    root = Path(__file__).resolve().parents[1]
    problems = []
    generated = subprocess.run([sys.executable, str(root / 'tools/gen_exports.py'), '--check'], capture_output=True, text=True)
    if generated.returncode != 0:
        problems.append(generated.stderr.strip() or generated.stdout.strip())
    sf = surface(fetch(root))
    exported_all = 0
    for module, path in MODULES.items():
        text = (root / path).read_text(encoding='utf-8')
        exported = set(re.findall(r'^export using ::(\w+);', text, flags=re.M))
        exported_all += len(exported)
        declared = set(sf.types[module]) | set(sf.functions[module])
        if module == 'codec':
            declared -= set(TYPE_MACROS)
        if declared - exported:
            problems.append(f'{path} lacks: ' + ', '.join(sorted(declared - exported)))
        if exported - declared:
            problems.append(f'{path} exports what the headers do not declare: ' + ', '.join(sorted(exported - declared)))
        if exported & (URL_TYPES | URL_FUNCTIONS):
            problems.append(f'{path} exports libopusurl: ' + ', '.join(sorted(exported & (URL_TYPES | URL_FUNCTIONS))))
    if set(sf.excluded_types) != URL_TYPES or set(sf.excluded_functions) != URL_FUNCTIONS:
        problems.append('libopusurl\'s declarations are not the ones the headers declare: '
                        f'{sorted(set(sf.excluded_types + sf.excluded_functions) ^ (URL_TYPES | URL_FUNCTIONS))}')
    ctl = set(sf.variadic) - URL_FUNCTIONS
    if ctl != set(CTL_FUNCTIONS):
        problems.append(f'variadic functions without a ctl_request overload, or overloads of none: {sorted(ctl ^ set(CTL_FUNCTIONS))}')

    macros_text = (root / 'src/macros.cppm').read_text(encoding='utf-8')
    body = macros_text[macros_text.index('export module'):]
    constants = set(re.findall(r'^inline constexpr auto (\w+) = c::value_\1;', body, flags=re.M))
    aliases = set(re.findall(r'^using (\w+) = c::type_\1;', body, flags=re.M))
    ctls = set(re.findall(r'^constexpr ctl_request<[^>]*> \((\w+)\)\(', body, flags=re.M))
    overloads = set(re.findall(r'^int (\w+)\(\w+\* st, ctl_request<Args\.\.\.> r\)', body, flags=re.M))
    if overloads != set(CTL_FUNCTIONS):
        problems.append(f'ctl_request overloads: {sorted(overloads ^ set(CTL_FUNCTIONS))}')
    if aliases != set(TYPE_MACROS):
        problems.append(f'type aliases: {sorted(aliases ^ set(TYPE_MACROS))}')
    mapped = constants | aliases | ctls
    macros = set(sf.macros)
    guards = {m for m in macros if excluded(m) == 'include guard'}
    url = {m for m in macros if m in URL_MACROS}
    if url != URL_MACROS:
        problems.append('libopusurl macros not in the headers: ' + ', '.join(sorted(URL_MACROS - url)))
    unaccounted = macros - mapped - SIDE_HEADER.keys() - guards - url
    if unaccounted:
        problems.append('macros neither mapped, side-header, libopusurl nor include guards: ' + ', '.join(sorted(unaccounted)))
    both = SIDE_HEADER.keys() & mapped
    if both:
        problems.append('listed as side-header yet mapped: ' + ', '.join(sorted(both)))
    stale = SIDE_HEADER.keys() - macros
    if stale:
        problems.append('listed but not in the headers: ' + ', '.join(sorted(stale)))
    test = (root / 'tests/macros_mix_test.cpp').read_text(encoding='utf-8')
    untested = {n for n in mapped if f'm::{n}' not in test}
    if untested:
        problems.append('mapped but not checked against the header: ' + ', '.join(sorted(untested)))
    link = (root / 'tests/link_test.cpp').read_text(encoding='utf-8')
    functions = {f for fs in sf.functions.values() for f in fs}
    unlinked = {f for f in functions if f'&::{f})' not in link}
    if unlinked:
        problems.append('exported but not linked by tests/link_test.cpp: ' + ', '.join(sorted(unlinked)))
    side = (root / 'include/mwrap/opus/macros.hpp').read_text(encoding='utf-8')
    for header in ('<stdint.h>', '<opus.h>', '<opus_multistream.h>', '<opus_projection.h>', '<opusfile.h>'):
        if f'#include {header}' not in side:
            problems.append(f'<mwrap/opus/macros.hpp> does not include {header}')
    if problems:
        sys.exit('\n'.join(problems))
    types = sum(len(sf.types[m]) for m in MODULES) - len(set(TYPE_MACROS) & set(sf.types['codec']))
    print(f'{exported_all} declarations exported ({types} types, {len(functions)} functions: '
          f'libopus {len(sf.functions["codec"])}, opusfile {len(sf.functions["file"])}, libogg {len(sf.functions["ogg"])}), '
          f'each function linked; {len(URL_TYPES) + len(URL_FUNCTIONS)} libopusurl declarations left out; '
          f'{len(macros)} macros: {len(constants)} constants, {len(aliases)} type aliases and {len(ctls)} CTL functions '
          f'mapped (each checked against the header), {len(SIDE_HEADER)} kept as macros, {len(url)} libopusurl, '
          f'{len(guards)} include guards; {len(overloads)} *_ctl overloads')


if __name__ == '__main__':
    main()
