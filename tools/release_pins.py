#!/usr/bin/env python3
"""Generate the producer record and immutable worker pin from a verified push."""
import argparse
import json
from pathlib import Path
import re
import sys


WORKER_LINE = re.compile(
    r'''(?m)^(?P<prefix>[ \t]*image:[ \t]*)(?P<quote>['"]?)(?P<image>registry\.[\w.]+/chromeless/chromeless(?::[^\s'"#]+|@[^\s'"#]+)?)(?P=quote)(?P<suffix>[ \t]*(?:#.*)?)$''')


def worker_images(text):
    return [m['image'] for m in WORKER_LINE.finditer(text)]


def immutable_image(image, digest):
    if not re.fullmatch(r'registry\.[\w.]+/chromeless/chromeless:cr[0-9]+-[0-9a-f]{7,40}', image):
        raise ValueError('invalid build image reference')
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', digest):
        raise ValueError('invalid immutable image digest')
    return image.rsplit(':', 1)[0] + '@' + digest


def render_manifest(text, image, digest):
    target = immutable_image(image, digest)
    updated, count = WORKER_LINE.subn(
        lambda m: m['prefix'] + m['quote'] + target + m['quote'] + m['suffix'], text)
    if not count:
        raise ValueError('manifest has no recognized worker image pin')
    return updated


def write_release(record_path, manifest_path, commit, image, digest, built_at):
    if not re.fullmatch(r'[0-9a-f]{40}', commit) or not commit.startswith(image.rsplit('-', 1)[-1]):
        raise ValueError('build source does not match image tag')
    # Validate the entire manifest change before touching either tracked file.
    manifest = render_manifest(manifest_path.read_text(), image, digest)
    record = {'schema': 'chromeless.guest-release/v1', 'commit': commit,
              'image': image, 'digest': digest, 'dockerfile': 'build/Dockerfile.runtime',
              'built_at': built_at, 'recorded_by': 'chromeless-kaniko-push.sh'}
    record_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(manifest)
    record_path.write_text(json.dumps(record, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('record', 'manifest', 'commit', 'image', 'digest', 'built-at'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    try:
        write_release(Path(args.record), Path(args.manifest), args.commit,
                      args.image, args.digest, args.built_at)
    except (OSError, ValueError):
        print('ERROR: release record and immutable deployment pin could not be generated', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
