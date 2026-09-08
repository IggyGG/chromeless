#!/usr/bin/env python3
"""Require a terminal failed push of this exact tag, node and context."""
import argparse
import json
import sys


def validate(job, name, node, tag, context, required_state='Failed'):
    if job.get('metadata', {}).get('name') != name or job['metadata'].get('deletionTimestamp'):
        raise ValueError('previous job identity is missing or being deleted')
    status = job.get('status', {})
    conditions = status.get('conditions', [])
    if (required_state not in ('Failed', 'Complete')
            or not any(c.get('type') == required_state and c.get('status') == 'True' for c in conditions)
            or any(c.get('type') in ('Failed', 'Complete') and c.get('type') != required_state
                   and c.get('status') == 'True' for c in conditions)
            or any(status.get(k, 0) != 0 for k in ('active', 'ready', 'terminating'))):
        raise ValueError('push has not reached the required terminal state')
    spec = job.get('spec', {}).get('template', {}).get('spec', {})
    if spec.get('nodeName') != node:
        raise ValueError('previous push used a different build node')
    containers = spec.get('containers', [])
    if len(containers) != 1 or containers[0].get('name') != 'kaniko':
        raise ValueError('previous push is not a canonical kaniko job')
    container = containers[0]
    bindings = [e for e in container.get('env', []) if e.get('name') == 'CHROMELESS_KANIKO_TAG']
    if len(bindings) != 1 or bindings[0].get('value') != tag:
        raise ValueError('previous push used a different image tag')
    destinations = [a for a in container.get('args', []) if a.startswith('--destination=')]
    expected = 'registry.triform.cloud/chromeless/chromeless:'
    if destinations not in ([f'--destination={expected}$(CHROMELESS_KANIKO_TAG)'],
                            [f'--destination={expected}{tag}']):
        raise ValueError('previous push used a different registry destination')
    mounts = [m for m in container.get('volumeMounts', []) if m.get('mountPath') == '/workspace']
    volumes = {v.get('name'): v for v in spec.get('volumes', [])}
    if (len(mounts) != 1 or mounts[0].get('readOnly') is not True
            or volumes.get(mounts[0].get('name'), {}).get('hostPath', {}).get('path') != context):
        raise ValueError('previous push used a different or writable build context')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ('name', 'node', 'tag', 'context'):
        parser.add_argument('--' + arg, required=True)
    parser.add_argument('--required-state', choices=('Failed', 'Complete'), default='Failed')
    args = parser.parse_args()
    try:
        validate(json.load(sys.stdin), args.name, args.node, args.tag, args.context, args.required_state)
    except (ValueError, TypeError, KeyError, AttributeError):
        print('ERROR: operation requires the matching canonical push in the required terminal state', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
