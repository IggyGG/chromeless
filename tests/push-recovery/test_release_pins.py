"""The writer and actual lint CLI must recognize the real digest-only pin."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('release_pins_test', ROOT / 'tools/release_pins.py')
PINS = importlib.util.module_from_spec(spec)
spec.loader.exec_module(PINS)
COMMIT = '30178cd4122ea445a2b928b187a4d759c46e1ffc'
IMAGE = 'registry.triform.cloud/chromeless/chromeless:cr7727-30178cd4122e'
DIGEST = 'sha256:' + 'a' * 64
IMMUTABLE = IMAGE.rsplit(':', 1)[0] + '@' + DIGEST


class Pins(unittest.TestCase):
    def test_writer_updates_tags_digests_and_quoted_pins_without_touching_other_images(self):
        for old in [IMAGE, IMAGE + '@sha256:' + 'b' * 64,
                    IMAGE.rsplit(':', 1)[0] + '@sha256:' + 'b' * 64]:
            for quote in ['', '"', "'"]:
                with self.subTest(old=old, quote=quote):
                    before = f'  image: {quote}{old}{quote} # worker\n  image: registry.triform.cloud/chromeless/chromeless-gateway:keep\n'
                    after = PINS.render_manifest(before, IMAGE, DIGEST)
                    self.assertIn(f'  image: {quote}{IMMUTABLE}{quote} # worker', after)
                    self.assertIn('chromeless-gateway:keep', after)
                    self.assertEqual(PINS.worker_images(after), [IMMUTABLE])

    def test_invalid_or_missing_worker_pin_cannot_generate_partial_record(self):
        with tempfile.TemporaryDirectory() as d:
            record, manifest = Path(d) / 'record.json', Path(d) / 'stack.yaml'
            record.write_text('previous record\n'); manifest.write_text('# image: ' + IMAGE + '\n')
            with self.assertRaises(ValueError):
                PINS.write_release(record, manifest, COMMIT, IMAGE, DIGEST, '2026-09-08T00:00:00Z')
            self.assertEqual(record.read_text(), 'previous record\n')
            self.assertEqual(manifest.read_text(), '# image: ' + IMAGE + '\n')

    def test_wrong_source_or_digest_is_rejected_before_files_change(self):
        with tempfile.TemporaryDirectory() as d:
            record, manifest = Path(d) / 'record.json', Path(d) / 'stack.yaml'
            record.write_text('previous record\n'); manifest.write_text('image: ' + IMAGE + '\n')
            for commit, digest in [('b' * 40, DIGEST), (COMMIT, 'sha256:short')]:
                with self.subTest(commit=commit, digest=digest):
                    with self.assertRaises(ValueError):
                        PINS.write_release(record, manifest, commit, IMAGE, digest, '2026-09-08T00:00:00Z')
                    self.assertEqual(record.read_text(), 'previous record\n')
                    self.assertEqual(manifest.read_text(), 'image: ' + IMAGE + '\n')

    def test_cli_rejects_stale_digest_missing_pin_and_mutable_tag(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); (root / 'tools/lint').mkdir(parents=True); (root / 'build').mkdir()
            (root / 'infra/k8s/standalone').mkdir(parents=True)
            shutil.copyfile(ROOT / 'tools/release_pins.py', root / 'tools/release_pins.py')
            lint = root / 'tools/lint/deploy_pin_lint.py'
            shutil.copyfile(ROOT / 'tools/lint/deploy_pin_lint.py', lint)
            (root / 'build/guest-release.json').write_text(json.dumps({'image': IMAGE, 'digest': DIGEST}))
            stack = root / 'infra/k8s/standalone/stack.yaml'
            cases = [(IMMUTABLE, 0), (IMAGE + '@' + DIGEST, 0),
                     (IMMUTABLE[:-1] + 'b', 1), (IMAGE, 1), ('unrelated/image:tag', 1)]
            for image, expected in cases:
                with self.subTest(image=image):
                    stack.write_text('image: ' + image + '\n')
                    result = subprocess.run(['python3', str(lint)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
            stack.unlink()
            self.assertNotEqual(subprocess.run(['python3', str(lint)], capture_output=True).returncode, 0)


if __name__ == '__main__':
    unittest.main()
