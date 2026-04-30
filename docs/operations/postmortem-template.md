# Postmortem: <one-line incident summary>

> **Status:** draft / final
> **Severity:** SEV-{1,2,3}
> **On-call:** <name>
> **Authors:** <names>
> **Incident timeline:** <YYYY-MM-DD HH:MM UTC> – <YYYY-MM-DD HH:MM UTC>
> **User-visible duration:** <minutes>
> **Sessions affected:** <count, or "all", or "tenant X only">

Copy this template into a new doc per incident and fill it in. The
goal is **systemic improvement, not blame**. We follow Google SRE's
[blameless postmortem](https://sre.google/sre-book/postmortem-culture/)
conventions; if you read a draft and feel the urge to identify a
culprit, rewrite the sentence to identify the *gap in the system*
that let the human do the wrong thing.

---

## TL;DR

Two or three sentences. What happened, who was affected, what we
did, what we'll change. A reader skimming a quarterly review should
walk away with the gist from this section alone.

## Impact

- **User-visible:** what did users see? "all sessions failed to
  connect for 13 minutes," "p95 latency rose from 80 ms to 320 ms
  for 47 minutes," etc.
- **SLO impact:** which SLO from [`sla.md`](./sla.md) breached?
  Reference the alert(s) that fired (or didn't, if detection was
  the gap).
- **Tenants affected:** "all tenants," "tenant-X only," "anonymous
  sessions only," etc. Pull from
  `cb_signaling_active_sessions{tenant=...}` history.

## Detection

- **First signal:** alert that fired, dashboard panel that turned
  red, user complaint, internal report. Time-to-first-signal from
  the actual start of the incident.
- **Detection latency:** if the alert/page fired N minutes after
  the incident started, that's a gap. Was the right metric being
  collected? Was the threshold set right? Was the alert routed
  correctly?

## Timeline

UTC times. Be precise; rough timelines paper over response gaps.

| Time | Event |
|---|---|
| HH:MM | Background change that planted the seed (e.g., last image push). |
| HH:MM | Symptom begins (per metrics). |
| HH:MM | First alert fires. |
| HH:MM | On-call acknowledges. |
| HH:MM | Initial diagnosis (correct or incorrect). |
| HH:MM | Mitigation begins. |
| HH:MM | User-visible recovery. |
| HH:MM | Full recovery (last lingering effect resolved). |
| HH:MM | Postmortem started. |

## Root cause

What was actually broken? **Cause vs trigger:**

- **Trigger** is what kicked it off (the deploy at 14:32 UTC).
- **Cause** is the underlying gap that let the trigger have its
  effect (e.g., "the new session pool template referenced an
  image tag that didn't exist; the pool reconciler retried
  forever; warm pool drained").

Aim for cause, not trigger. The trigger is a one-line fact;
the cause is a paragraph.

## Contributing factors

- **What enabled the cause to happen?** Process gaps,
  documentation gaps, observability gaps, organisational gaps.
- **What made it worse than it had to be?** Did detection lag
  amplify the impact? Did a runbook step take longer than expected?
- **What worked well?** Important: the system isn't all bad. Note
  what helped (a dashboard surfaced the right signal, a runbook
  step worked, an alert fired correctly).

## Mitigation

- **Immediate (during the incident):** what stopped the bleeding?
- **Short-term (within 24 hours after):** rollbacks, config
  patches, scale changes.
- **Long-term (in the repair items below):** code, doc, alerting,
  process changes that prevent recurrence or speed detection.

## Repair items

Each item is a separate ticket; assign an owner and due date.

| Title | Owner | Due | Ticket |
|---|---|---|---|
| Add alert for X | infra-dev | YYYY-MM-DD | T-N |
| Update runbook section Y | qa-tester | YYYY-MM-DD | T-N |
| Code change: ensure Z can't happen | webrtc-dev | YYYY-MM-DD | T-N |

**Rule:** every postmortem ships with at least one repair item.
"This was a freak thing, no action needed" is almost never the
right answer; if it really is, write *one paragraph* explaining
why, and treat the next recurrence as a re-open of the same
incident.

## What we learned

Two-paragraph reflection:

- **The unexpected thing.** What surprised the responders during
  the incident? Surprise is the source of the next incident
  unless captured here.
- **The missing thing.** What instrumentation, runbook entry,
  alert, or training would have made the response faster or made
  the incident smaller? This becomes input to the repair items.

## Cross-references

- Alert rules that fired:
  [`infra/observability/alerts/cb-alerts.yaml`](../../infra/observability/alerts/cb-alerts.yaml)
  → `<rule name>`.
- SLO breached:
  [`sla.md`](./sla.md) → `<SLO row>`.
- Runbook entries used:
  [`runbook.md`](./runbook.md) → `#<section>`.
- Related PRs / commits: <links>.
- Related prior incidents: <links>.

---

## Reviewers

- [ ] Author
- [ ] On-call lead
- [ ] Engineering lead
- [ ] (For SEV-1) Product lead

When all four boxes are checked + repair items have owners +
due-dates, this postmortem is final. Move it from "draft" to "final"
status above and post the link in the team's incident channel.
