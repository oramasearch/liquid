#!/usr/bin/env python3
"""Exercise repository selection through the installed public CLI surface."""

import argparse
import json
import os
import pathlib
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--liquid', required=True, type=pathlib.Path)
    parser.add_argument('--fixture', required=True, type=pathlib.Path)
    parser.add_argument('--source-fixture', required=True, type=pathlib.Path)
    parser.add_argument('--work', required=True, type=pathlib.Path)
    args = parser.parse_args()
    work = args.work.resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    env = os.environ.copy()
    env['LIQUID_HOME'] = str(work / 'identity')
    env['LIQUID_WRAP_CACHE'] = str(work / 'build-cache')
    env['SOURCE_DATE_EPOCH'] = '1700000000'
    package = work / 'package'
    package.mkdir()
    (package / 'payload.txt').write_text('payload\n')

    def run(*command, cwd=package, expected=0):
        result = subprocess.run(command, cwd=cwd, env=env, text=True, capture_output=True)
        print('$ ' + ' '.join(str(value) for value in command), flush=True)
        print(result.stdout, end='', flush=True)
        print(result.stderr, end='', flush=True)
        assert result.returncode == expected, (result.returncode, command)
        return result.stdout if expected == 0 else result.stderr

    def liquid(*command, **kwargs):
        return run(str(args.liquid), *command, **kwargs)

    def project(target, source, directory, cwd=package, expected=0):
        result = liquid('project', target, source, '--name', 'example', '--version', '1.0.0',
                        '--arch', 'arm64', '--out', str(work / directory), '--json',
                        cwd=cwd, expected=expected)
        if expected:
            assert 'wrap' not in result
            assert 'accepted inputs' in result
            return result
        report = json.loads(result)
        assert report['artifacts']
        for artifact in report['artifacts']:
            assert pathlib.Path(artifact['path']).exists()
        return report

    liquid('init', '.')
    change = liquid('cut', 'release').strip().removeprefix('change=')
    tree = liquid('tree').strip()
    assert tree.startswith('blake3:')
    change_report = project('deb', change, 'deb-change')
    tree_report = project('deb', tree, 'deb-tree')
    repeated = project('deb', change, 'deb-repeat')
    deb = pathlib.Path(change_report['artifacts'][0]['path']).read_bytes()
    assert deb.startswith(b'!<arch>\n')
    assert deb == pathlib.Path(tree_report['artifacts'][0]['path']).read_bytes()
    assert deb == pathlib.Path(repeated['artifacts'][0]['path']).read_bytes()
    project('oci', change, 'oci-change')
    project('oci', tree, 'oci-tree')
    project('rpm', change, 'rpm-change')
    project('apk', tree, 'apk-tree')
    error = project('dsc', tree, 'dsc-bare', expected=1)
    assert 'bare tree' in error
    assert not (work / 'build-cache').exists()
    error = project('deb', 'payload.txt', 'bad-path', expected=1)
    assert 'payload.txt' in error
    error = project('deb', 'blake3:' + '0' * 64, 'missing', expected=1)
    assert 'not_found' in error

    source = work / 'source'
    shutil.copytree(args.source_fixture, source)
    liquid('init', '.', cwd=source)
    source_change = liquid('cut', 'source release', cwd=source).strip().removeprefix('change=')
    project('dsc', source_change, 'dsc-change', cwd=source)
    assert not (work / 'build-cache').exists()

    # Only the compatibility fixture knows how to populate the build repository.
    ids = dict(line.split('=', 1) for line in
               run(str(args.fixture), 'build', str(package)).strip().splitlines())
    project('deb', ids['result'], 'deb-result')
    project('oci', ids['result'], 'oci-result')
    project('dsc', ids['result'], 'dsc-result')
    project('dsc', ids['tree'], 'dsc-install-tree')
    project('deb', change, 'deb-with-build-cache')
    project('deb', tree, 'tree-with-build-cache')
    error = project('deb', ids['action'], 'action', expected=1)
    assert 'an action' in error
    error = project('deb', ids['blob'], 'blob', expected=1)
    assert 'a blob' in error

    # Preserve result projection for a checkout with no ordinary repository.
    checkout = work / 'checkout'
    checkout.mkdir()
    (checkout / 'payload.txt').write_text('different build payload\n')
    build_ids = dict(line.split('=', 1) for line in
                     run(str(args.fixture), 'build', str(checkout)).strip().splitlines())
    project('deb', build_ids['result'], 'deb-build-only', cwd=checkout)
    print('project CLI: ordinary changes/trees, source packages, result compatibility, '
          'input errors, and deterministic bytes passed')


if __name__ == '__main__':
    main()
