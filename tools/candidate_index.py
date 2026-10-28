#!/usr/bin/env python3
"""The compat namespace from mcpp-index's candidate branch, while the published
index does not yet carry compat.opus, compat.opusfile and compat.ogg.

The three are added to mcpp-index by mcpplibs/mcpp-index#612. Until those
descriptors are on the index's main branch, a build of this repository cannot
resolve its dependencies from the published index. This tool then clones
the PR's branch, prepares the unchanged namespace views of its descriptors
(the index's own tests/prepare_mwrap_indices.py) and prints the checkout.

  --checkout-only   print the candidate checkout (nothing once published),
                    for tools/test_template.py --index
  --ci              also append `[indices] compat = { path = <view> }` to this
                    checkout's manifests (the root and examples/opus), in
                    CI's working copy only

Once the published index has them, it does nothing.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import urllib.error
import urllib.request

INDEX = 'https://github.com/mcpplibs/mcpp-index'
BRANCH = 'compat-opus'
PUBLISHED = 'https://raw.githubusercontent.com/mcpplibs/mcpp-index/main/pkgs/c/compat.opusfile.lua'
MANIFESTS = ['mcpp.toml', 'examples/opus/mcpp.toml']
PACKAGES = ['compat.ogg', 'compat.opus', 'compat.opusfile']


def published() -> bool:
    try:
        with urllib.request.urlopen(PUBLISHED, timeout=60) as response:
            return response.status == 200
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return False
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--checkout-only', action='store_true')
    mode.add_argument('--ci', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if published():
        print('compat.opus, compat.opusfile and compat.ogg are published: the published index is used', file=sys.stderr)
        return
    checkout = root / 'target/candidate-index'
    if not (checkout / 'index.toml').exists():
        subprocess.run(['git', 'clone', '--quiet', '--depth', '1', '--branch', BRANCH, INDEX + '.git', str(checkout)],
                       check=True, stdout=sys.stderr)
    subprocess.run([sys.executable, str(checkout / 'tests/prepare_mwrap_indices.py')], cwd=checkout, check=True,
                   stdout=sys.stderr)
    view = checkout / 'tests/mwrap/.indices/compat'
    for package in PACKAGES:
        if not (view / f'pkgs/view/{package}.lua').exists():
            sys.exit(f'{INDEX} {BRANCH} has no {package}')
    if args.ci:
        for manifest in MANIFESTS:
            with (root / manifest).open('a', encoding='utf-8', newline='\n') as out:
                out.write('\n# CI only (tools/candidate_index.py): compat.opus, compat.opusfile and compat.ogg from ' + BRANCH + '\n')
                out.write('[indices]\ncompat = { path = "' + view.as_posix() + '" }\n')
            print('candidate compat index for', manifest, file=sys.stderr)
    print(checkout.as_posix())


if __name__ == '__main__':
    main()
