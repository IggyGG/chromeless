# Security policy

## Reporting a vulnerability

Do not open a public issue for a suspected vulnerability. Use
[GitHub private vulnerability reporting](https://github.com/IggyGG/chromeless/security/advisories/new)
so maintainers can investigate before details are disclosed. If GitHub private
reporting is unavailable, email `security@triform.ai` with a concise impact
summary and reproduction steps. Never include live credentials, session tokens,
customer data, or browser contents in a report.

We will acknowledge a complete report within three business days and coordinate
remediation and disclosure with the reporter.

## Scope

Reports about browser isolation, DevTools exposure, signaling and TURN
credentials, session authentication, container boundaries, navigation policy,
and cross-session data leakage are especially valuable. Ordinary bugs and
feature requests belong in the public issue tracker.

## Supported versions

Chromeless is source-available and does not yet publish stable releases. Fixes
are applied to `main`; deployments should pin a reviewed commit and update when
security fixes land.

The two fingerprints in `.gitleaksignore` are the literal
`REPLACE_WITH_HEX_32` deployment-template marker, not credentials. Every other
finding remains fatal.
