"""Push recovery guards; all cluster and registry calls below are fake."""
import copy
import importlib.util
import json
import pathlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import re
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'infra/k8s/chromeless-build'
spec = importlib.util.spec_from_file_location('push_retry', SCRIPT / 'check-push-retry.py')
CHECK = importlib.util.module_from_spec(spec)
spec.loader.exec_module(CHECK)
TAG = 'cr7727-30178cd4122e'
JOB = 'chromeless-kaniko-push-' + TAG
CONTEXT = '/var/lib/longhorn/chromeless-build/chromium-src/artifacts/context'


def failed_job():
    return {'metadata': {'name': JOB}, 'status': {'conditions': [{'type': 'Failed', 'status': 'True'}]},
            'spec': {'template': {'spec': {'nodeName': 'triform-7', 'containers': [{
                'name': 'kaniko', 'env': [{'name': 'CHROMELESS_KANIKO_TAG', 'value': TAG}],
                'args': ['--destination=registry.triform.cloud/chromeless/chromeless:$(CHROMELESS_KANIKO_TAG)'],
                'volumeMounts': [{'name': 'workspace', 'mountPath': '/workspace', 'readOnly': True}]}],
                'volumes': [{'name': 'workspace', 'hostPath': {'path': CONTEXT}}]}}}}


class RetryIdentity(unittest.TestCase):
    def check(self, job):
        CHECK.validate(job, JOB, 'triform-7', TAG, CONTEXT)

    def test_terminal_failed_matching_push_is_eligible(self):
        self.check(failed_job())

    def test_running_succeeded_unknown_or_terminating_push_is_ineligible(self):
        for status in [{}, {'active': 1}, {'conditions': [{'type': 'Complete', 'status': 'True'}]},
                       {**failed_job()['status'], 'active': 1}, {**failed_job()['status'], 'terminating': 1},
                       {**failed_job()['status'], 'ready': 1}]:
            with self.subTest(status=status):
                job = failed_job(); job['status'] = status
                with self.assertRaises(ValueError): self.check(job)

    def test_other_node_tag_destination_or_context_is_ineligible(self):
        for field in ('name', 'node', 'tag', 'destination', 'context', 'writable', 'deleting'):
            with self.subTest(field=field):
                job = failed_job(); pod = job['spec']['template']['spec']; c = pod['containers'][0]
                if field == 'name': job['metadata']['name'] = 'another-job'
                if field == 'node': pod['nodeName'] = 'triform-8'
                if field == 'tag': c['env'][0]['value'] = 'another-build'
                if field == 'destination': c['args'].append('--destination=another/image:tag')
                if field == 'context': pod['volumes'][0]['hostPath']['path'] = '/different'
                if field == 'writable': c['volumeMounts'][0]['readOnly'] = False
                if field == 'deleting': job['metadata']['deletionTimestamp'] = 'now'
                with self.assertRaises(ValueError): self.check(job)


class Wrapper(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='chromeless-push-test-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        folder = self.root / 'infra/k8s/chromeless-build'; folder.mkdir(parents=True)
        for name in ('chromeless-kaniko-push.sh', 'chromeless-kaniko-push.yaml', 'check-push-retry.py'):
            shutil.copyfile(SCRIPT / name, folder / name)
        (self.root / 'tools').mkdir()
        shutil.copyfile(ROOT / 'tools/release_pins.py', self.root / 'tools/release_pins.py')
        stack = self.root / 'infra/k8s/standalone/stack.yaml'; stack.parent.mkdir(parents=True)
        stack.write_text('image: registry.triform.cloud/chromeless/chromeless@sha256:' + 'b' * 64 + '\n')
        self.script = folder / 'chromeless-kaniko-push.sh'
        self.bin = self.root / 'bin'; self.bin.mkdir()
        self.env = {**os.environ, 'PATH': str(self.bin) + os.pathsep + os.environ['PATH'],
                    'CHROMELESS_KANIKO_TAG': TAG, 'CHROMELESS_PUSH_ATTEMPT': '2',
                    'PUSH_TEST_ROOT': str(self.root), 'PUSH_TEST_JOB': json.dumps(failed_job())}
        self.executable('kubectl', '''#!/usr/bin/env python3
import json,os,pathlib,sys
r=pathlib.Path(os.environ['PUSH_TEST_ROOT']); a=sys.argv[1:]
with (r/'calls').open('a') as f:f.write(json.dumps(a)+'\\n')
if 'get' in a and 'job' in a:
 if a[-1]=='json':
  if '--ignore-not-found' in a:
   existing=os.environ.get('PUSH_TEST_EXISTING','')
   if existing:print(existing)
  else:print(os.environ['PUSH_TEST_JOB'])
  sys.exit(0)
 if 'completionTime' in a[-1]:print('2026-09-08T21:13:13Z');sys.exit(0)
 terminal=os.environ['PUSH_TEST_TERMINAL']
 if 'range .status.conditions' in a[-1]:print(terminal+'=True')
 elif terminal in a[-1]:print('True')
 sys.exit(0)
if a[:2]==['create','-f']:
 (r/'manifest').write_text(sys.stdin.read());sys.exit(0 if os.environ.get('PUSH_TEST_TERMINAL') else 73)
if 'get' in a and 'pods' in a:print('fake-push-pod');sys.exit(0)
if 'get' in a and 'pod' in a:print('Succeeded' if os.environ['PUSH_TEST_TERMINAL']=='Complete' else 'Failed');sys.exit(0)
if 'logs' in a:print('[fake] log observer finished');sys.exit(0)
raise SystemExit('unexpected kubectl call')
''')
        self.executable('crane', '''#!/usr/bin/env python3
import os,sys,pathlib
mode=os.environ.get('PUSH_TEST_REGISTRY','missing')
if mode=='exists' or (pathlib.Path(os.environ['PUSH_TEST_ROOT'])/'manifest').exists():print('sha256:'+ 'a'*64);sys.exit(0)
print('MANIFEST_UNKNOWN' if mode=='missing' else 'UNAUTHORIZED',file=sys.stderr);sys.exit(1)
''')
        self.executable('git', '''#!/usr/bin/env python3
import sys
if 'rev-parse' in sys.argv:print('30178cd4122ea445a2b928b187a4d759c46e1ffc');sys.exit(0)
if 'merge-base' in sys.argv:sys.exit(0)
raise SystemExit('unexpected git call')
''')

    def executable(self, name, source):
        p = self.bin / name; p.write_text(source); p.chmod(0o755)

    def run_wrapper(self):
        return subprocess.run(['bash', str(self.script), 'triform-7', 'x264-t7'],
                              env=self.env, capture_output=True, text=True, timeout=15)

    def test_retry_creates_distinct_job_and_preserves_deadline_and_prior_job(self):
        result = self.run_wrapper()
        self.assertEqual(result.returncode, 73, result.stderr)
        manifest = (self.root / 'manifest').read_text()
        self.assertIn('name: ' + JOB + '-attempt2', manifest)
        # The test's NAME is "preserves deadline", and that is the contract:
        # the retry manifest must carry the deadline from the template, NOT a
        # weakened one. Asserting the literal 300 also asserted a value that
        # is not this test's business — it broke when the template moved to
        # 900s on 2026-09-08 (a 210 MB layer was being severed mid-upload at
        # 300s; see the comment on activeDeadlineSeconds in the yaml).
        #
        # Compare against the template instead, so this keeps testing
        # preservation and stops testing a number someone else owns.
        template = (pathlib.Path(__file__).resolve().parents[2] /
                    'infra/k8s/chromeless-build/chromeless-kaniko-push.yaml').read_text()
        expected = [ln.strip() for ln in template.splitlines()
                    if ln.strip().startswith('activeDeadlineSeconds:')]
        self.assertEqual(len(expected), 1, 'template lost its deadline')
        self.assertIn(expected[0], manifest)
        self.assertIn('name: verify-build-tag', manifest)
        self.assertIn('$(CHROMELESS_KANIKO_TAG)', manifest)
        calls = [json.loads(l) for l in (self.root / 'calls').read_text().splitlines()]
        self.assertFalse(any('delete' in c or 'apply' in c for c in calls))
        self.assertFalse((self.root / 'build/guest-release.json').exists())

    def test_existing_tag_or_unavailable_registry_never_creates_job(self):
        for mode in ('exists', 'unauthorized'):
            with self.subTest(mode=mode):
                self.env['PUSH_TEST_REGISTRY'] = mode
                self.assertNotEqual(self.run_wrapper().returncode, 0)
                self.assertFalse((self.root / 'manifest').exists())

    def test_running_previous_job_never_creates_attempt(self):
        job = failed_job(); job['status'] = {'active': 1}
        self.env['PUSH_TEST_JOB'] = json.dumps(job)
        self.assertNotEqual(self.run_wrapper().returncode, 0)
        self.assertFalse((self.root / 'manifest').exists())

    def test_attempt_limit_prevents_cluster_calls(self):
        self.env['CHROMELESS_PUSH_ATTEMPT'] = '4'
        self.assertNotEqual(self.run_wrapper().returncode, 0)
        self.assertFalse((self.root / 'calls').exists())

    def test_completed_attempt_recovers_record_without_recreating_or_repushing(self):
        job = failed_job(); job['metadata']['name'] = JOB + '-attempt2'
        job['status']['conditions'] = [{'type': 'Complete', 'status': 'True'}]
        self.env['PUSH_TEST_EXISTING'] = json.dumps(job)
        self.env['PUSH_TEST_REGISTRY'] = 'exists'
        result = self.run_wrapper()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.root / 'build/guest-release.json').exists())
        self.assertFalse((self.root / 'manifest').exists())

    def test_existing_active_attempt_cannot_be_recreated(self):
        job = failed_job(); job['metadata']['name'] = JOB + '-attempt2'; job['status'] = {'active': 1}
        self.env['PUSH_TEST_EXISTING'] = json.dumps(job)
        self.assertNotEqual(self.run_wrapper().returncode, 0)
        self.assertFalse((self.root / 'manifest').exists())

    def test_record_recovery_preserves_prior_record_when_digest_cannot_be_verified(self):
        job = failed_job(); job['metadata']['name'] = JOB + '-attempt2'
        job['status']['conditions'] = [{'type': 'Complete', 'status': 'True'}]
        self.env['PUSH_TEST_EXISTING'] = json.dumps(job)
        self.env['PUSH_TEST_REGISTRY'] = 'unavailable'
        record = self.root / 'build/guest-release.json'; record.parent.mkdir()
        record.write_text('{"previous":"preserve exactly"}\n')
        before = record.read_bytes()
        self.assertNotEqual(self.run_wrapper().returncode, 0)
        self.assertEqual(record.read_bytes(), before)
        self.assertFalse((self.root / 'manifest').exists())

    def test_invalid_retry_tag_prevents_cluster_calls(self):
        self.env['CHROMELESS_KANIKO_TAG'] = 'cr7727-hotfix'
        self.assertNotEqual(self.run_wrapper().returncode, 0)
        self.assertFalse((self.root / 'calls').exists())

    def test_failed_job_is_reported_promptly_without_writing_release_record(self):
        self.env['PUSH_TEST_TERMINAL'] = 'Failed'
        result = self.run_wrapper()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn('kaniko-push FAILED', result.stderr)
        self.assertFalse((self.root / 'build/guest-release.json').exists())

    def test_successful_attempt_records_the_build_tag_and_updates_both_pins(self):
        self.env['PUSH_TEST_TERMINAL'] = 'Complete'
        stack = self.root / 'infra/k8s/standalone/stack.yaml'; stack.parent.mkdir(parents=True, exist_ok=True)
        stack.write_text('image: registry.triform.cloud/chromeless/chromeless:old\n')
        result = self.run_wrapper()
        self.assertEqual(result.returncode, 0, result.stderr)
        record = json.loads((self.root / 'build/guest-release.json').read_text())
        self.assertEqual(record['commit'], '30178cd4122ea445a2b928b187a4d759c46e1ffc')
        self.assertEqual(record['digest'], 'sha256:' + 'a' * 64)
        self.assertEqual(record['built_at'], '2026-09-08T21:13:13Z')
        self.assertTrue(record['image'].endswith(':' + TAG))
        self.assertIn(record['image'].rsplit(':', 1)[0] + '@' + record['digest'], stack.read_text())

    def test_staged_tag_guard_rejects_missing_and_different_builds(self):
        manifest = (SCRIPT / 'chromeless-kaniko-push.yaml').read_text()
        block = re.search(r'            - \|\n((?:              .*\n)+)', manifest)[1]
        guard = textwrap.dedent(block).replace('/workspace/IMAGE_TAG', str(self.root / 'IMAGE_TAG'))
        for value, expected in [(None, 1), ('cr7727-another', 1), (TAG, 0)]:
            with self.subTest(value=value):
                if value is not None:(self.root / 'IMAGE_TAG').write_text(value + '\n')
                result = subprocess.run(['sh', '-ec', guard], env=self.env, capture_output=True)
                self.assertEqual(result.returncode, expected)


if __name__ == '__main__':
    unittest.main()
