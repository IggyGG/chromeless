# tests/webrtc/pod-templates — drafter pod-manifest templates

Stable pod-manifest templates for CV2 functional-test drafter-spawned
verification pods. Drafters copy a template into `/tmp/` and substitute
the placeholders before `kubectl apply`.

## Why templates live in-tree

CV2-87 (2026-05-20) revealed cross-boundary literal drift: chromium-7727
split `--use-gl` into `--use-gl` + `--use-angle`. The in-image
`infra/launch-chromeless.sh` was updated, but ad-hoc drafter-generated
pod manifests in `/tmp/` carried the stale `--use-gl=swiftshader` form.
Each new drafter re-derived from the stale `/tmp/` copy, propagating
the drift across verification runs.

Landing templates here gives drafters a single source-of-truth to
regenerate from, and any future chromium-N flag drift is patched in one
place (this directory + `infra/launch-chromeless.sh`) rather than across
N drafter scratch files.

## Templates

| Template | Drives | Verifies |
|---|---|---|
| `cv2-phase-a-m5-r1.yaml.tpl` | `phase-a-m5-r1-percursor-event.mjs` | CV2-78 cursor-routing gate via `Input.dispatchMouseEvent` stimulus |
| `cv2-phase-a-m5-r1-dom-probe.yaml.tpl` | `phase-a-m5-r1-dom-probe.mjs` | CV2-78 Axis 1 (Screen) vs Axis 2 (renderer) gate disambiguation |
| `cv2-phase-a-m5-r1-native-cursor.yaml.tpl` | `phase-a-m5-r1-native-cursor.mjs` | CV2-83 native cursor probe — the only coverage of the SetCursor → CbCursorClient → cursor DataChannel path |

## Substitution

Each template uses three placeholders:

- `__IMAGE_REF__` — pinned-by-digest chromeless image, e.g.
  `registry.triform.cloud/chromeless/chromeless@sha256:<hex>`
- `__REV_TAG__` — short rev label used in pod name + labels, e.g. `rv6`
- `__HARNESS_CM__` — ConfigMap name carrying the harness `.mjs` + `package.json`,
  e.g. `cv2-m5-r1-rv6-harness`

Substitute with `sed` or `envsubst` before applying:

```bash
sed -e 's|__IMAGE_REF__|registry.triform.cloud/chromeless/chromeless@sha256:05d62...|g' \
    -e 's|__REV_TAG__|rv6|g' \
    -e 's|__HARNESS_CM__|cv2-m5-r1-rv6-harness|g' \
    tests/webrtc/pod-templates/cv2-phase-a-m5-r1.yaml.tpl \
    > /tmp/cv2-phase-a-m5-r1-rv6.yaml
```

Or with `envsubst`:

```bash
export __IMAGE_REF__='registry.triform.cloud/chromeless/chromeless@sha256:05d62...'
export __REV_TAG__='rv6'
export __HARNESS_CM__='cv2-m5-r1-rv6-harness'
envsubst < tests/webrtc/pod-templates/cv2-phase-a-m5-r1.yaml.tpl \
    > /tmp/cv2-phase-a-m5-r1-rv6.yaml
```

## Cross-boundary literal source-of-truth

The chromium command-line flags for software rendering must stay in lockstep
across all consumers of the chromeless image:

| Consumer | Path | Locked-by |
|---|---|---|
| Production launch script (in-image) | `infra/launch-chromeless.sh` | Build-czar |
| Build-promotion runtime validation | `infra/k8s/tests/chromeless-webrtc-validation.yaml` | Same image, must match |
| Ad-hoc functional-test drafter pods | `tests/webrtc/pod-templates/*.yaml.tpl` | This directory |

If chromium drops, renames, or splits a GL/ANGLE flag again, patch ALL
THREE in the same commit. Search with:

```bash
git grep -nE 'use-gl|use-angle|swiftshader' infra/ tests/ build/
```
