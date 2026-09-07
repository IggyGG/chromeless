# infra/k8s/standalone/

The standalone stack, on Kubernetes. Same shape as `infra/compose.yaml`, but as
plain manifests — useful when the machine you sit at cannot run the worker
image (it is amd64-only) or when you want it on real hardware.

**This is how the standalone mode was first proven end to end.** Real video,
1280x720, VP9, relayed through TURN, with navigation working mid-stream.

## What is here

| file | |
| --- | --- |
| `smoke-job.yaml` + `smoke-probe.py` | Does the worker image boot and paint? Takes an image and nothing else. |
| `build-images.yaml` | Two kaniko Jobs: the gateway and the broker, built from a pushed branch. |
| `stack.yaml` | signaling + worker + gateway, Deployments and Services. |
| `networkpolicy.yaml` | Required — the namespace denies all traffic by default. |
| `coturn.yaml` | Reference TURN relay. **Does not deploy here** — see below. |

## Order

```bash
# 0. Prove the image first, in isolation. ~30s.
kubectl create configmap chromeless-smoke-probe -n chromeless \
  --from-file=smoke.py=infra/k8s/standalone/smoke-probe.py
kubectl apply -f infra/k8s/standalone/smoke-job.yaml
kubectl logs -n chromeless -f job/chromeless-smoke     # want: PASS

# 1. Build the images. Push your branch first — kaniko clones from the remote.
kubectl apply -f infra/k8s/standalone/build-images.yaml

# 2. Secrets: the login, and the keypair the gateway signs with.
eval "$(cd infra/gateway && go run ./cmd/keygen)"
kubectl create secret generic chromeless-standalone -n chromeless \
  --from-literal=CHROMELESS_USER=chromeless \
  --from-literal=CHROMELESS_PASS="$(openssl rand -hex 12)" \
  --from-literal=CHROMELESS_AUTH_PRIVKEY="$CHROMELESS_AUTH_PRIVKEY" \
  --from-literal=CHROMELESS_AUTH_PUBKEY="$CHROMELESS_AUTH_PUBKEY"

# The worker needs a BROWSER-role token, and cannot refresh one — the gateway's
# issuer mints 15-minute tokens, which is useless for a worker. cmd/worker-token
# mints 30 days with the same key. (In compose this is keygen's third export;
# here it is minted separately because it lands in its OWN Secret, which is
# what stack.yaml wires into WEBRTC_SIGNALING_TOKEN.)
kubectl create secret generic chromeless-standalone-worker-token -n chromeless \
  --from-literal=token="$(cd infra/gateway && SESSION_ID=dev go run ./cmd/worker-token)"

# 3. Network policy, then the stack.
kubectl apply -f infra/k8s/standalone/networkpolicy.yaml
kubectl apply -f infra/k8s/standalone/stack.yaml

# 4. Reach it.
kubectl port-forward -n chromeless svc/chromeless-standalone-gateway 8443:8443
open https://localhost:8443
```

Teardown is one selector:

```bash
kubectl delete all,secret,cm,networkpolicy -n chromeless \
  -l app.kubernetes.io/part-of=chromeless-standalone
```

## Things that cost time here

**The namespace denies all traffic by default.** `chromeless-default-deny`
selects `podSelector: {}`, so new pods get nothing until a policy allows them.
The symptom is a TIMEOUT, not a refusal — `dial tcp ...: i/o timeout` on the
gateway and `ERR_CONNECTION_TIMED_OUT` on the worker, both of which read as
"the other service is down". The tell is that it fails even between two pods on
the same node. `networkpolicy.yaml` is not optional.

**`kubectl port-forward` needs its own ingress rule.** The forward terminates
at the kubelet, so the connection arrives from the node, not from a pod.
Pod-to-pod rules alone leave it connecting and then hanging.

**Nothing arms capture implicitly.** The gateway does it (see
`armCaptureWhenReady`). Without it the peer connects, negotiates a video track,
delivers no frames, and the worker self-terminates after 30s with
`CV2-GPU-DEATH`. In triform this is physics's job; standalone, the gateway
stands in.

**A TURN relay is effectively mandatory here.** A cluster pod and a laptop are
both behind NAT, so host and srflx candidates never pair — ICE goes
`checking -> failed`, and everything upstream looks perfect. Both peers need
the relay: the worker via `WEBRTC_ICE_SERVERS`, and the browser via the
broker's `TURN_URLS`/`TURN_USER`/`TURN_PASS`.

**`coturn.yaml` does not deploy in this namespace.** `chromeless` *enforces*
PodSecurity `baseline`, which forbids `hostNetwork` and `hostPort`; a relay
needs both, because it allocates from a wide UDP range that must be reachable.
That is exactly why triform's coturn lives in `triform-production`, where the
same policy is only audited. Use an existing relay, or run one in a namespace
without enforcement. The manifest is kept as the reference config.

**One viewer at a time, re-armed between viewers.** After a client disconnects
(`bye`), the worker rebuilds its peer connection in place and offers again to the
next viewer, keeping the browser where it was (`CV2-REARM` in the worker log).
Images before `cr7727-8d2ce2e66288` (2026-08-21) instead went quiet for the life
of the process and needed `kubectl rollout restart` between runs; if you see
that, check the image tag before anything else.

**Distroless images need a numeric `runAsUser`.** They declare `USER nonroot`
by name, and with `runAsNonRoot: true` the kubelet cannot verify that and
refuses to start the container — `CreateContainerConfigError`, with the reason
only visible in `kubectl describe`. Use `runAsUser: 65532`.
