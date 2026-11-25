#!/usr/bin/env python3
"""Secret scan, Python standard library only (no installs).

Looks for common API key formats, private keys, tracked .env files and high-entropy
strings. By default it scans the files git tracks (or, outside a git repo, every file
except dependencies and build output). With --staged it scans what is about to be
committed; the pre-commit hook uses that.

Allowlist: .secret-scan-allow in the project root. One entry per line:
  skip:<glob>          never scan files matching the glob (e.g. skip:docs/fixtures/*)
  <fingerprint>        ignore one finding; copy it from this script's output
Lines starting with # are comments.

It never prints a full secret, only the first 4 characters and the length.
Usage: python3 -I scripts/secret_scan.py [--staged] [--root DIR] [files...]
Exit codes: 0 clean, 1 secrets found, 2 usage or git error.

A later upgrade is Gitleaks, which knows many more key formats; it is an install, so it
needs Nathan's OK first.
"""
from __future__ import annotations

import fnmatch
import hashlib
import math
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterator, List, Optional, Set, Tuple

RULES: List[Tuple[str, "re.Pattern[str]"]] = [
    ("private-key", re.compile(r"-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP |ENCRYPTED )?PRIVATE KEY(?: BLOCK)?-----")),
    ("aws-access-key", re.compile(r"\b(?:AKIA|ASIA|AGPA|AIDA|AROA|ANPA)[0-9A-Z]{16}\b")),
    ("github-token", re.compile(r"\b(?:gh[pousr]_[A-Za-z0-9]{36,}|github_pat_[A-Za-z0-9_]{60,})\b")),
    ("anthropic-key", re.compile(r"\bsk-ant-[A-Za-z0-9_\-]{20,}")),
    ("openai-key", re.compile(r"\bsk-(?:proj-|svcacct-|admin-)?[A-Za-z0-9_\-]{32,}")),
    ("slack-token", re.compile(r"\bxox[abposr]-[A-Za-z0-9-]{10,}")),
    ("slack-webhook", re.compile(r"https://hooks\.slack\.com/services/[A-Za-z0-9/]{20,}")),
    ("stripe-key", re.compile(r"\b(?:sk|rk)_(?:live|test)_[A-Za-z0-9]{20,}")),
    ("google-api-key", re.compile(r"\bAIza[0-9A-Za-z_\-]{35}\b")),
    ("google-oauth-secret", re.compile(r"\bGOCSPX-[A-Za-z0-9_\-]{20,}")),
    ("npm-token", re.compile(r"\bnpm_[A-Za-z0-9]{36}\b")),
    ("vercel-token", re.compile(r"\bvercel_[A-Za-z0-9]{24,}\b")),
    ("jwt", re.compile(r"\beyJ[A-Za-z0-9_-]{10,}\.eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}")),
    ("url-password", re.compile(r"\b[a-z][a-z0-9+.-]*://[^\s:/@'\"]+:[^\s:/@'\"]{6,}@[^\s'\"]+")),
    ("assigned-secret", re.compile(
        r"(?i)\b[A-Za-z0-9_]*(?:api[_-]?key|secret|token|passw(?:or)?d|auth|credential)[A-Za-z0-9_]*"
        r"\s*[:=]\s*['\"]([^'\"\s]{12,})['\"]")),
]
CANDIDATE = re.compile(r"[A-Za-z0-9+/=_\-]{32,}")
ENV_NAME = re.compile(r"(^|/)\.env(\.[^/]*)?$")
ENV_OK = re.compile(r"(^|/)\.env\.(example|sample|template|dist)$")
PLACEHOLDER = re.compile(r"(?i)(example|placeholder|changeme|your[_-]|xxxx|dummy|fake|redacted|<[^>]+>|\$\{)")

DEFAULT_SKIP = [
    "*package-lock.json", "*uv.lock", "*.lock", "*yarn.lock", "*pnpm-lock.yaml",
    "*.png", "*.jpg", "*.jpeg", "*.gif", "*.ico", "*.webp", "*.pdf", "*.zip", "*.gz",
    "*.woff", "*.woff2", "*.ttf", "*.mp4", "*.webm", "*.svg", ".secret-scan-allow",
]
WALK_SKIP_DIRS = {".git", "node_modules", ".venv", "dist", "build", "__pycache__", ".mypy_cache",
                  ".pytest_cache", ".ruff_cache", ".vite", "test-results", "playwright-report"}
MAX_BYTES = 2_000_000


def entropy(s: str) -> float:
    counts = {c: s.count(c) for c in set(s)}
    return -sum(n / len(s) * math.log2(n / len(s)) for n in counts.values())


def high_entropy(token: str) -> bool:
    if re.fullmatch(r"[0-9a-fA-F]+", token):  # hashes and git SHAs are everywhere
        return False
    if not (re.search(r"[a-z]", token) and re.search(r"[A-Z]", token) and re.search(r"[0-9]", token)):
        return False
    if re.fullmatch(r"[A-Za-z]+(?:[_-][A-Za-z0-9]+)+", token):  # identifiers_like_this
        return False
    return entropy(token) >= 4.5


def mask(s: str) -> str:
    return f"{s[:4]}...({len(s)} chars)"


def fingerprint(path: str, rule: str, secret: str) -> str:
    return f"{path}:{rule}:{hashlib.sha256(secret.encode()).hexdigest()[:16]}"


def scan_text(path: str, text: str) -> Iterator[Tuple[int, str, str]]:
    for lineno, line in enumerate(text.splitlines(), 1):
        if len(line) > 5000:
            line = line[:5000]
        found: Set[str] = set()
        for rule, rx in RULES:
            for m in rx.finditer(line):
                secret = m.group(1) if m.groups() else m.group(0)
                if rule == "assigned-secret" and (PLACEHOLDER.search(secret) or entropy(secret) < 3.0):
                    continue
                found.add(secret)
                yield lineno, rule, secret
        if "integrity" in line or "sha512-" in line or "sha256-" in line:
            continue
        for m in CANDIDATE.finditer(line):
            tok = m.group(0).strip("=")
            if tok in found or any(tok in f or f in tok for f in found):
                continue
            if high_entropy(tok) and not PLACEHOLDER.search(tok):
                yield lineno, "high-entropy", tok


def git(root: Path, *args: str) -> Optional[str]:
    try:
        r = subprocess.run(["git", "-C", str(root)] + list(args), capture_output=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout.decode("utf-8", "replace") if r.returncode == 0 else None


def load_allow(root: Path) -> Tuple[List[str], Set[str]]:
    skips, prints = list(DEFAULT_SKIP), set()
    f = root / ".secret-scan-allow"
    if f.is_file():
        for line in f.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("skip:"):
                skips.append(line[5:].strip())
            else:
                prints.add(line)
    return skips, prints


def walk(root: Path) -> List[str]:
    out = []
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in WALK_SKIP_DIRS]
        for name in files:
            out.append(os.path.relpath(os.path.join(d, name), root))
    return sorted(out)


def main(argv: List[str]) -> int:
    staged = False
    root: Optional[Path] = None
    files: List[str] = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--staged":
            staged = True
        elif a == "--root" and i + 1 < len(argv):
            root = Path(argv[i + 1]).resolve()
            i += 1
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        elif a.startswith("-"):
            print(f"secret_scan: unknown option {a}", file=sys.stderr)
            return 2
        else:
            files.append(a)
        i += 1
    if root is None:
        top = git(Path.cwd(), "rev-parse", "--show-toplevel")
        root = Path(top.strip()) if top else Path(__file__).resolve().parent.parent
    in_git = git(root, "rev-parse", "--git-dir") is not None

    if staged:
        if not in_git:
            print("secret_scan: --staged needs a git repo", file=sys.stderr)
            return 2
        listing = git(root, "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z")
        paths = [p for p in (listing or "").split("\0") if p]
    elif files:
        paths = [os.path.relpath(os.path.abspath(f), root) for f in files]
    elif in_git:
        paths = [p for p in (git(root, "ls-files", "-z") or "").split("\0") if p]
    else:
        paths = walk(root)

    skips, prints = load_allow(root)
    findings: List[Tuple[str, int, str, str]] = []
    scanned = 0
    for path in paths:
        if ENV_NAME.search(path) and not ENV_OK.search(path):
            fp = fingerprint(path, "env-file", path)
            if fp not in prints:
                findings.append((path, 0, "env-file", path))
            continue
        if any(fnmatch.fnmatch(path, g) or fnmatch.fnmatch(os.path.basename(path), g) for g in skips):
            continue
        if staged:
            try:
                data = subprocess.run(["git", "-C", str(root), "show", f":{path}"],
                                      capture_output=True, timeout=30).stdout
            except (OSError, subprocess.SubprocessError):
                continue
        else:
            p = root / path
            if not p.is_file() or p.is_symlink():
                continue
            try:
                with open(p, "rb") as fh:
                    data = fh.read(MAX_BYTES)
            except OSError:
                continue
        if b"\0" in data[:8192]:
            continue  # binary
        scanned += 1
        for lineno, rule, secret in scan_text(path, data.decode("utf-8", "replace")):
            if fingerprint(path, rule, secret) not in prints:
                findings.append((path, lineno, rule, secret))

    what = "staged files" if staged else "files"
    if not findings:
        print(f"secret_scan: clean ({scanned} {what} scanned)")
        return 0
    print(f"secret_scan: FOUND {len(findings)} possible secret(s) in {what}:", file=sys.stderr)
    for path, lineno, rule, secret in findings:
        where = f"{path}:{lineno}" if lineno else path
        shown = "tracked .env file" if rule == "env-file" else mask(secret)
        print(f"  {where}  [{rule}]  {shown}", file=sys.stderr)
        print(f"      allowlist fingerprint: {fingerprint(path, rule, secret)}", file=sys.stderr)
    print("Remove the secret (and rotate it if it was real). If it is not a secret, add its "
          "fingerprint to .secret-scan-allow.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
