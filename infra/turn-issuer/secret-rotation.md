# TURN shared-secret rotation

T76 issuer + coturn share an HMAC secret. Periodic rotation is a
defence-in-depth: if the secret leaks, an attacker can mint TURN
credentials at will until the next rotation closes them off.
Operationally the gotcha is that **issuer and coturn must agree on
"current" through the entire rotation window**, otherwise legitimate
clients can't relay through TURN until they fetch new credentials.

This doc covers the operational dance: which envs to flip, when to
SIGHUP, and how to tell the rotation worked.

## Variables involved

| Component | Variable | Role |
|---|---|---|
| Issuer | `CHROMELESS_TURN_SHARED_SECRET` | the secret the issuer uses to *mint* credentials |
| Issuer | `CHROMELESS_TURN_SHARED_SECRET_PREV` | optional; only set during a rotation overlap. The issuer doesn't *use* this to mint, but its presence is the signal "we are rolling, expect existing credentials minted with the previous secret to still be in flight." |
| coturn | `static-auth-secret` (in `turnserver.conf`) | the secret coturn uses to *validate* credentials presented by clients |
| coturn | `static-auth-secret-fallback` (compile-time option in some forks; otherwise we run two `turnserver.conf` blocks during overlap) | optional secondary secret coturn will accept while the primary rolls forward |

## Why a grace period

Credentials issued at time T have a TTL up to 86400 (the issuer's
max). A client that pulled credentials at T-1 hour will still be
relaying through TURN with those credentials when the rotation runs.
If coturn flips to a new secret at T0, the client's existing
credentials become invalid mid-call. They can re-fetch, but only after
their next ICE failure handler kicks in — that's a visible glitch.

The fix is to keep the *old* secret valid on coturn for at least as
long as the longest still-issued credential's remaining TTL. With our
defaults (TTL ≤ 1h, max 24h), a 24-hour grace window is the safe
upper bound; for typical traffic 2 hours is enough.

## The dance

For a deploy where you control both the issuer and the coturn
StatefulSet (the manifest in `infra/k8s/turn-deployment.yaml`):

1. **Pre-roll.** Pick a new secret `S_new`. Generate with
   `openssl rand -hex 32`.
2. **Update coturn first.** Stage `S_new` as the primary in
   `turnserver.conf`, `S_old` as the fallback. Apply the update and
   `supervisorctl restart coturn` (or rolling-restart the StatefulSet
   pods one at a time). After this step both secrets are valid for
   *validation* on coturn; existing credentials minted with `S_old`
   keep working.
3. **Verify.** From a sample pod:
   ```
   curl -fsS http://turn.example.com:3478 -X TURNTEST   # not a real CLI
   # Easier: just watch coturn's stderr for "rejected" auth events;
   # there should be none.
   ```
4. **Update issuer.** Set `CHROMELESS_TURN_SHARED_SECRET=S_new` and
   `CHROMELESS_TURN_SHARED_SECRET_PREV=S_old`. Roll the issuer Deployment.
   New issuances now use `S_new`. Existing in-flight credentials minted
   with `S_old` keep validating against coturn's fallback.
5. **Wait the grace period.** At minimum, the longest `ttlSeconds`
   you let through. With our defaults: 1 hour. With a permissive max:
   24 hours.
6. **Drop the fallback.** Update `turnserver.conf` to remove the
   `S_old` fallback. Roll coturn again. Update the issuer to drop
   `CHROMELESS_TURN_SHARED_SECRET_PREV` (set to empty).
7. **Verify** by checking
   `cb_turn_issuer_credentials_issued_total` is climbing on the
   /metrics endpoint and coturn's auth-failure counter is flat.

The whole window is on the order of hours, not days. Automate this
with your CD pipeline: it's two K8s Secret patches and two rolling
restarts.

## Failure modes

- **Issuer cuts over before coturn does.** New credentials sign with
  `S_new` but coturn only knows `S_old` — every TURN allocation
  fails. Symptom: spike in client-side ICE failures, no TURN
  connectivity, `cb_webrtc_round_trip_time_ms` cliff. Fix: roll
  coturn forward.
- **coturn cuts over and removes fallback before grace period
  elapses.** Existing in-flight `S_old` credentials fail. Symptom:
  active sessions disconnect mid-stream. Fix: re-add fallback,
  extend grace period.
- **Issuer's `_PREV` env never gets cleared.** Functionally fine —
  the issuer doesn't actually use the prev secret for anything,
  it's just a marker. But it'll generate alarming-looking
  `prev_secret_set: true` log lines forever. Hygiene: clear it after
  the grace period.

## Programmatic verification

The test `TestIssue_SecretRotation_GracePeriod` in `main_test.go`
verifies the issuer's behaviour mid-rotation:

- Minting with `S_new` produces a credential that validates under
  `S_new` (deterministic HMAC).
- The same username, HMAC'd with `S_old`, produces a *different*
  credential — i.e., there's no overlap surface where one credential
  validates under both secrets. The grace period is purely a coturn
  side concession.

Run with `go test ./infra/turn-issuer/`.
