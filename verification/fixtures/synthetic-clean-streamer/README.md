# synthetic-clean-streamer fixture

A `sourceRoot` candidate where:
- `capture/streamer-page/` is absent
- the synthetic image manifest at `chromeless_synthetic-clean-streamer/image-manifest.json`
  has `paths: []` (none of the streamer-page paths are present)

Used by R3 tests to exercise the PASS path without mutating the live repo.

This directory intentionally contains no `capture/streamer-page/` — its
absence is the fixture.
