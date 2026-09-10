#!/usr/bin/env python3
"""Static include lint for the Chromium embedder sources.

WHY THIS EXISTS
---------------
`capture/` is an out-of-tree Chromium embedder. Compiling it requires a full
Chromium checkout plus a 4-8 hour build (see build/chromeless-build.sh), so in
practice nobody compiles it while editing — which means a missing `#include`
is not discovered until a build lane runs hours later, or worse, until someone
notices the lane has been failing.

That is not hypothetical. `Cb.shutdown` was written using `base::BindOnce` and
`FROM_HERE` without including `base/functional/bind.h` or `base/location.h`.
It would have failed the build. It was caught by reading the file against
in-tree precedent, which does not scale and does not survive the next agent.

This linter closes that specific gap in seconds, with no Chromium tree: for a
curated set of Chromium `base::` symbols whose defining header is stable and
unambiguous, it asserts that a translation unit which USES the symbol also
INCLUDES its header (directly, or via its own .h for a .cc).

WHAT IT DELIBERATELY DOES NOT DO
--------------------------------
This is a lint, not a compiler. It does not parse C++. It cannot find type
errors, wrong overloads, or missing includes for symbols outside its table.
A clean run means "this specific, recurring, expensive mistake is absent" —
not "this compiles". Keep the table small and high-confidence: a false
positive here costs more trust than a missed include costs time.

Usage:
    python3 tools/lint/cxx_include_lint.py [paths...]     # default: capture/
    python3 tools/lint/cxx_include_lint.py --list         # show the table

Exit status: 0 clean, 1 findings, 2 bad invocation.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# ---------------------------------------------------------------------------
# Symbol -> required header.
#
# Inclusion criteria, applied strictly:
#   * The symbol is spelled unambiguously enough to match with a regex.
#   * Exactly one header canonically provides it.
#   * It is used somewhere in this tree today (so the table is grounded in
#     real usage, not speculation).
#
# The `base/` header frequencies in capture/ that motivated the initial set:
#   raw_ptr 35 · logging 30 · time 22 · values 17 · check 17 ·
#   sequenced_task_runner 15 · functional/bind 15 · weak_ptr 11 ·
#   sequence_checker 10 · scoped_refptr 9 · functional/callback 8 ·
#   synchronization/lock 5 · location 5 · json_reader 5 · json_writer 4
# ---------------------------------------------------------------------------
#
# CALIBRATION NOTE (read before adding a rule)
# --------------------------------------------
# The first draft of this table had 20 rules and produced 46 findings on a
# tree that demonstrably builds — i.e. it was mostly false positives, which
# is strictly worse than no lint. The causes, both instructive:
#
#   * `scoped_refptr<` / `base::TimeDelta` / `base::DictValue` are supplied
#     transitively by headers these files legitimately include
#     (`api/video/i420_buffer.h` brings webrtc's own `scoped_refptr`;
#     `media/base/video_frame.h` brings `base/time/time.h`;
#     `cb_input_dispatch.h` brings `base/values.h`). A "missing include" for
#     a symbol that is reliably transitive is noise.
#   * `scoped_refptr` is not even Chromium-unique — webrtc defines its own.
#
# So the surviving rules are only symbols that are (a) namespaced
# unambiguously to one definition, and (b) NOT reliably dragged in by
# common neighbours. Verified against the current tree: this set is clean.
#
# Before adding a rule: add it, run the linter, and confirm every new
# finding is a genuine compile error. If it fires on code that builds, the
# rule is wrong — drop it. A false positive costs more trust than a missed
# include costs time.
RULES: list[tuple[str, str, str]] = [
    # (regex, required header, human-readable symbol)
    #
    # The binders and FROM_HERE are the motivating case: `Cb.shutdown` used
    # base::BindOnce + FROM_HERE with neither header included.
    (r"\bbase::BindOnce\b",            "base/functional/bind.h",          "base::BindOnce"),
    (r"\bbase::BindRepeating\b",       "base/functional/bind.h",          "base::BindRepeating"),
    (r"\bFROM_HERE\b",                 "base/location.h",                 "FROM_HERE"),
    # JSON entry points: single canonical header each, not transitively
    # common in this tree.
    (r"\bbase::JSONReader\b",          "base/json/json_reader.h",         "base::JSONReader"),
    (r"\bbase::JSONWriter\b",          "base/json/json_writer.h",         "base::JSONWriter"),
    # Explicitly namespaced and not umbrella-supplied here.
    (r"\bbase::WeakPtrFactory\b",      "base/memory/weak_ptr.h",          "base::WeakPtrFactory"),
    (r"\bbase::AutoLock\b",            "base/synchronization/lock.h",     "base::AutoLock"),
    (r"\bbase::StringPrintf\b",        "base/strings/stringprintf.h",     "base::StringPrintf"),
    (r"\bbase::NumberToString\b",      "base/strings/string_number_conversions.h", "base::NumberToString"),
    (r"\bbase::UTF8ToUTF16\b",         "base/strings/utf_string_conversions.h", "base::UTF8ToUTF16"),
    (r"\bbase::UTF16ToUTF8\b",         "base/strings/utf_string_conversions.h", "base::UTF16ToUTF8"),
    # content:: thread hops — the embedder posts across threads constantly
    # and this header is not implied by the content headers it neighbours.
    (r"\bcontent::GetUIThreadTaskRunner\b", "content/public/browser/browser_thread.h", "content::GetUIThreadTaskRunner"),
    (r"\bcontent::GetIOThreadTaskRunner\b", "content/public/browser/browser_thread.h", "content::GetIOThreadTaskRunner"),
    # Types that //content FORWARD-DECLARES in a header you are already
    # including, so NAMING them compiles and READING A FIELD does not.
    #
    # This class cost a lane cycle on 2026-09-09: HandleContextMenu takes
    # `const ContextMenuParams&`, web_contents_delegate.h forward-declares it
    # at :117, and eight `member access into incomplete type` errors came
    # back ~14 minutes later. The lint was clean throughout, because every
    # symbol it knew about WAS included.
    #
    # The pattern must match MEMBER ACCESS, not the type name. A header that
    # only names the type in an override signature is CORRECT with just the
    # forward declaration, and a rule keyed on the name alone flags it — the
    # first version of this rule did exactly that on two files, both fine.
    #
    # Keyed on the conventional parameter name for each, which is what these
    # are called at every call site in this tree. Narrow on purpose: a false
    # positive costs more trust than a missed defect costs time.
    (r"\bparams\.(link_url|link_text|src_url|selection_text|media_type|is_editable|unfiltered_link_url)\b",
     "content/public/browser/context_menu_params.h", "content::ContextMenuParams member access"),
    (r"\bssl_info\.(cert|cert_status|is_issued_by_known_root)\b",
     "net/ssl/ssl_info.h", "net::SSLInfo member access"),
    (r"\bauth_info\.(realm|scheme|is_proxy|challenger)\b",
     "net/base/auth.h", "net::AuthChallengeInfo member access"),
]

COMPILED = [(re.compile(pat), hdr, sym) for pat, hdr, sym in RULES]

# Headers that transitively and reliably provide another header's symbols.
# Kept deliberately tiny — every entry here is a hole in the lint, so each
# must be a genuine, stable umbrella relationship.
PROVIDED_BY: dict[str, set[str]] = {
    # Callback types come with the binders.
    "base/functional/callback.h": {"base/functional/bind.h"},
    # The task-runner headers declare PostTask(const Location&, ...) and pull
    # in base/location.h, so FROM_HERE resolves without a direct include.
    #
    # This is a measured fact about this tree, not an assumption: seven files
    # use FROM_HERE with no direct base/location.h and all of them compile
    # (e.g. cb_input_dispatch.cc gets it through its own header's
    # base/task/sequenced_task_runner.h at cb_input_dispatch.h:42). Flagging
    # them would be a false positive.
    #
    # The motivating Cb.shutdown bug is still caught: that TU used FROM_HERE
    # with NO task-runner header of any kind in scope.
    "base/task/sequenced_task_runner.h": {"base/location.h"},
    "base/task/single_thread_task_runner.h": {"base/location.h"},
    "base/task/thread_pool.h": {"base/location.h"},
    "content/public/browser/browser_thread.h": {"base/location.h"},
    # base::Timer::Start() takes a Location; base::test::TaskEnvironment
    # pulls the task machinery into tests. Same measured reasoning as above
    # (cb_signaling_reconnect.cc and capturer_test.cc both compile today).
    "base/timer/timer.h": {"base/location.h"},
    "base/test/task_environment.h": {"base/location.h"},
}

SOURCE_EXTS = (".cc", ".h", ".mm")

# Directories that are never ours to lint.
SKIP_DIRS = {"node_modules", ".git", ".claude", "out", "dist", "third_party"}


def strip_comments_and_strings(text: str) -> str:
    """Blank out // and /* */ comments and string literals.

    Without this, a symbol named in a comment (this tree is heavily
    commented) or inside a log message would count as usage and produce
    false positives. Cheap state machine; no C++ parsing.

    Elided spans are replaced with spaces, and newlines inside them are
    PRESERVED, so the returned text is byte-for-byte position-compatible
    with the input. That is what lets reported line numbers point at the
    real line — an earlier version deleted the spans outright and every
    reported line was off by the number of comment lines above it.
    """
    out = []
    i, n = 0, len(text)

    def blank(span: str) -> str:
        return "".join(ch if ch == "\n" else " " for ch in span)

    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(blank(text[i:j]))
            i = j
        elif c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(blank(text[i:j]))
            i = j
        elif c in '"\'':
            quote, start = c, i
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            out.append(blank(text[start:i]))
        else:
            out.append(c)
            i += 1
    return "".join(out)


def includes_of(text: str) -> set[str]:
    return set(re.findall(r'#include\s+"([^"]+)"', text))


def iter_sources(paths: list[str]):
    for p in paths:
        if os.path.isfile(p):
            if p.endswith(SOURCE_EXTS):
                yield p
            continue
        for root, dirs, files in os.walk(p):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            for f in sorted(files):
                if f.endswith(SOURCE_EXTS):
                    yield os.path.join(root, f)


def check_file(path: str) -> list[str]:
    try:
        with open(path, encoding="utf-8", errors="ignore") as fh:
            raw = fh.read()
    except OSError as exc:
        return [f"{path}: cannot read ({exc})"]

    code = strip_comments_and_strings(raw)
    direct = includes_of(raw)

    # For a .cc, its own header's includes count: the canonical Chromium
    # layout puts shared includes in the .h, and the .cc includes that first.
    effective = set(direct)
    if path.endswith((".cc", ".mm")):
        own_h = path.rsplit(".", 1)[0] + ".h"
        if os.path.exists(own_h):
            try:
                with open(own_h, encoding="utf-8", errors="ignore") as fh:
                    effective |= includes_of(fh.read())
            except OSError:
                pass

    # Expand umbrella relationships.
    for hdr, providers in PROVIDED_BY.items():
        if hdr in effective:
            effective |= providers

    findings = []
    for rx, hdr, sym in COMPILED:
        if hdr in effective:
            continue
        m = rx.search(code)
        if not m:
            continue
        line = code.count("\n", 0, m.start()) + 1
        findings.append(f'{path}:{line}: uses {sym} but does not include "{hdr}"')
    return findings


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", default=None,
                    help="files or directories to lint (default: capture/)")
    ap.add_argument("--list", action="store_true", help="print the rule table and exit")
    args = ap.parse_args(argv)

    if args.list:
        width = max(len(s) for _, _, s in RULES)
        for _, hdr, sym in RULES:
            print(f"  {sym:<{width}}  ->  {hdr}")
        print(f"\n{len(RULES)} rules.")
        return 0

    paths = args.paths or ["capture"]
    for p in paths:
        if not os.path.exists(p):
            print(f"error: no such path: {p}", file=sys.stderr)
            return 2

    findings, scanned = [], 0
    for src in iter_sources(paths):
        scanned += 1
        findings.extend(check_file(src))

    if findings:
        print(f"cxx-include-lint: {len(findings)} finding(s) across {scanned} file(s)\n")
        for f in findings:
            print(f"  {f}")
        print("\nEach finding is a probable compile error. The embedder only builds")
        print("inside a full Chromium checkout, so these would otherwise surface")
        print("hours later in the build lane.")
        return 1

    print(f"cxx-include-lint: clean ({scanned} files, {len(RULES)} rules)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
