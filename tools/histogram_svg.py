#!/usr/bin/env python3
"""Draw the per-order latency histogram from `me bench --json` as an SVG.

Python standard library only (no installs). Midnight colorway, the same as
market-maker-sim: background #02070A, bars #0297DE, text #C9D6DF.

Usage: python3 tools/histogram_svg.py <bench.json> <out.svg>
Exit codes: 0 written, 2 bad input.
"""

from __future__ import annotations

import json
import sys
from html import escape

BG = "#02070A"
PANEL = "#0A141B"
GRID = "#1B2A35"
TEXT = "#C9D6DF"
MUTED = "#7F93A3"
BLUE = "#0297DE"
ORANGE = "#FC734D"  # marks the p50 / p99 / p99.9 buckets


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


# The buckets me bench writes are centred on whole timer steps (DECISIONS.md D9).
STEP_LABELS = ["0", "1", "2", "3", "4-5", "6-8", "9-12", "13-24", "25-48", "49-120", "121-1200", ">1200"]


def bucket_of(buckets: list[dict], ns: float) -> int:
    for i, b in enumerate(buckets):
        hi = b["hi_ns"]
        if ns >= b["lo_ns"] and (hi is None or ns < hi):
            return i
    return len(buckets) - 1


def render(data: dict) -> str:
    buckets = data["histogram"]
    lat = data["latency_ns"]
    total = sum(b["count"] for b in buckets) or 1
    step = data["timer"]["resolution_ns"]
    w, h = 880, 500
    left, right, top, bottom = 70, 30, 92, 140
    pw, ph = w - left - right, h - top - bottom
    n = len(buckets)
    slot = pw / n
    bar_w = slot * 0.72
    # Log-ish scale so the tail stays visible: share of orders on a sqrt axis.
    shares = [b["count"] / total for b in buckets]
    top_share = max(shares) or 1.0

    def y(share: float) -> float:
        return top + ph - ph * (share / top_share) ** 0.5

    marks = {
        bucket_of(buckets, lat["p50"]): "p50",
        bucket_of(buckets, lat["p99"]): "p99",
        bucket_of(buckets, lat["p99.9"]): "p99.9",
    }

    out: list[str] = []
    out.append(
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
        f'font-family="ui-monospace, SFMono-Regular, Menlo, monospace" role="img" '
        f'aria-label="Per-order latency histogram">'
    )
    out.append(f'<rect width="{w}" height="{h}" fill="{BG}"/>')
    out.append(f'<rect x="{left}" y="{top}" width="{pw}" height="{ph}" fill="{PANEL}"/>')
    title = "Per-order latency, matching-engine"
    sub = (
        f'{data["orders"]:,} orders, seed {data["seed"]} · median {data["orders_per_sec"]["median"] / 1e6:.1f} M orders/sec · '
        f'timer step {data["timer"]["resolution_ns"]:.1f} ns'
    )
    out.append(f'<text x="{left}" y="38" fill="{TEXT}" font-size="20" font-weight="600">{escape(title)}</text>')
    out.append(f'<text x="{left}" y="62" fill="{MUTED}" font-size="13">{escape(sub)}</text>')
    for frac in (0.01, 0.1, 0.25, 0.5, 1.0):
        if frac > top_share * 1.0001:
            continue
        gy = y(frac)
        out.append(f'<line x1="{left}" x2="{left + pw}" y1="{gy:.1f}" y2="{gy:.1f}" stroke="{GRID}"/>')
        out.append(
            f'<text x="{left - 8}" y="{gy + 4:.1f}" fill="{MUTED}" font-size="11" text-anchor="end">{frac:.0%}</text>'
        )
    for i, b in enumerate(buckets):
        x = left + i * slot + (slot - bar_w) / 2
        share = shares[i]
        by = y(share)
        color = ORANGE if i in marks else BLUE
        if b["count"] > 0:
            out.append(
                f'<rect x="{x:.1f}" y="{by:.1f}" width="{bar_w:.1f}" height="{top + ph - by:.1f}" fill="{color}">'
                f'<title>{escape(b["label"])}: {b["count"]:,} orders ({share:.3%})</title></rect>'
            )
            out.append(
                f'<text x="{x + bar_w / 2:.1f}" y="{by - 6:.1f}" fill="{TEXT}" font-size="10" '
                f'text-anchor="middle">{b["count"]:,}</text>'
            )
        out.append(
            f'<text x="{x + bar_w / 2:.1f}" y="{top + ph + 18}" fill="{MUTED}" font-size="11" '
            f'text-anchor="middle">{escape(STEP_LABELS[i] if i < len(STEP_LABELS) else "")}</text>'
        )
    out.append(
        f'<text x="{left + pw / 2:.1f}" y="{top + ph + 36}" fill="{MUTED}" font-size="11" text-anchor="middle">'
        f'timer steps per order (1 step = {step:.1f} ns; a reading includes ~{data["timer"]["overhead_ns"]:.0f} ns '
        f"of timer overhead)</text>"
    )
    # One row per percentile tag, so neighbouring buckets never overlap.
    for row, (name, key) in enumerate((("p50", "p50"), ("p99", "p99"), ("p99.9", "p99.9"))):
        i = bucket_of(buckets, lat[key])
        x = left + i * slot + slot / 2
        out.append(
            f'<text x="{x:.1f}" y="{top + ph + 56 + 15 * row}" fill="{ORANGE}" font-size="12" '
            f'text-anchor="middle">{name} {lat[key]:.0f} ns</text>'
        )
    foot1 = f'{data["machine"]} · {data["compiler"]}'
    foot2 = f'{data["flags"]} · {data["warmup"]} warm-up + {data["runs"]} timed runs · checksum {data["checksum"]}'
    out.append(f'<text x="{left}" y="{h - 28}" fill="{MUTED}" font-size="10">{escape(foot1)}</text>')
    out.append(f'<text x="{left}" y="{h - 14}" fill="{MUTED}" font-size="10">{escape(foot2)}</text>')
    out.append(
        f'<text x="{w - right}" y="{top - 10}" fill="{MUTED}" font-size="11" text-anchor="end">'
        f"share of orders (square-root scale)</text>"
    )
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        fail("usage: histogram_svg.py <bench.json> <out.svg>")
    try:
        with open(argv[1], encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        fail(f"cannot read {argv[1]}: {e}")
    for key in ("histogram", "latency_ns", "orders_per_sec", "timer", "machine", "orders", "seed"):
        if key not in data:
            fail(f"{argv[1]} has no '{key}' (is it from me bench --json?)")
    svg = render(data)
    try:
        with open(argv[2], "w", encoding="utf-8") as f:
            f.write(svg)
    except OSError as e:
        fail(f"cannot write {argv[2]}: {e}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
