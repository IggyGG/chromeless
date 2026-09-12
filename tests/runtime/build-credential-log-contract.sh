#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 - <<'PY'
import json, os, pathlib, subprocess, tempfile, textwrap

paths = sorted(pathlib.Path('infra/k8s').rglob('*.yaml'))
password = 'synthetic-password-SENTINEL-09$`quote'
username = 'synthetic-user-SENTINEL-09'
covered = 0
for manifest in paths:
    script = manifest.read_text()
    if '- name: FORGEJO_PASSWORD' not in script:
        continue
    assert 'AUTH_REPO' not in script, manifest
    # These manifests embed a literal shell block. Dedent the named clone
    # phase, omitting OS installs and Chromium workdirs, without executing YAML.
    block = script.split('# BEGIN PRIVATE GIT AUTH\n', 1)[1].split('# END PRIVATE REPO CLONE', 1)[0]
    block = textwrap.dedent(block)
    for shell, mode in [(shell, mode) for shell in ('bash', 'sh') for mode in ('success', 'shallow-failure', 'failure')]:
        with tempfile.TemporaryDirectory() as tmp:
            d = pathlib.Path(tmp)
            shim = d / 'git'
            shim.write_text('''#!/usr/bin/env python3
import os, pathlib, subprocess, sys, json
args = sys.argv[1:]
p = pathlib.Path(os.environ['MOCK_ROOT'])
print('git ' + json.dumps(args))
assert os.environ.get('GIT_TRACE') is None
assert os.environ.get('GIT_CURL_VERBOSE') is None
assert os.environ['GIT_TERMINAL_PROMPT'] == '0'
askpass = os.environ['GIT_ASKPASS']
(p / 'askpass-path').write_text(askpass)
for prompt, name in [('Username for test', 'FORGEJO_USERNAME'), ('Password for test', 'FORGEJO_PASSWORD')]:
    value = subprocess.check_output([askpass, prompt], text=True).rstrip('\\n')
    assert value == os.environ[name]
assert subprocess.run([askpass, 'unrecognized'], capture_output=True).returncode != 0
if args[0] == 'clone':
    mode = os.environ['MOCK_MODE']
    if mode == 'failure' or (mode == 'shallow-failure' and '--depth' in args):
        print('synthetic clone refusal', file=sys.stderr)
        sys.exit(23)
    target = pathlib.Path(args[-1])
    (target / '.git').mkdir(parents=True)
    (target / 'build').mkdir()
    script = target / 'build/chromeless-build.sh'
    script.write_text('#!/bin/sh\\nexit 0\\n')
    script.chmod(0o700)
''')
            shim.chmod(0o700)
            env = {**os.environ, 'PATH': str(d) + os.pathsep + os.environ['PATH'],
                   'FORGEJO_USERNAME': username, 'FORGEJO_PASSWORD': password,
                   'OUR_REPO': 'https://forgejo.example.invalid/fixture.git',
                   'OUR_REPO_REF': 'synthetic-sha', 'MOCK_ROOT': str(d),
                   'MOCK_MODE': mode, 'GIT_TRACE': '1', 'GIT_CURL_VERBOSE': '1'}
            run = subprocess.run([shell, '-euxc', block.replace('/workspace', str(d / 'workspace'))],
                                 env=env, capture_output=True, text=True, timeout=10)
            output = run.stdout + run.stderr
            assert password not in output and username not in output, (manifest, mode, 'credential leaked')
            assert 'git ' in output, (manifest, mode, 'clone never ran')
            succeeds = mode == 'success' or (mode == 'shallow-failure' and manifest.name == 'build-job-x264-t7.yaml')
            assert (run.returncode == 0) == succeeds, (manifest, mode, run.returncode, output)
            helper = pathlib.Path((d / 'askpass-path').read_text())
            assert not helper.exists(), (manifest, mode, 'askpass helper leaked')
        covered += 1
assert covered == 54, covered
print(f'build credential logging: {covered} traced success/fallback/failure cases pass; no credential in output')
PY
