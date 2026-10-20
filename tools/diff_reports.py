#!/usr/bin/env python3
"""diff_reports.py - side-by-side capability matrix for xbox-probe/1 reports.

Usage:
  python -I diff_reports.py BASELINE.json OTHER.json [MORE.json ...]
         [--out FILE] [--format text|md|csv] [--only-changed] [--values]

Records are keyed by (category, name). The first report is the baseline; columns
follow argument order and are labelled by environment.environment (else file name).
A row is "changed" when its status differs or the key is absent from some report
(--only-changed keeps just those: the capabilities the sandbox strips).
--values adds per-environment values for rows OK everywhere and flags differences.
The summary gives status counts per environment, then REGRESSIONS: keys OK in the
baseline but DENIED, MISSING, NOTIMPL, CRASH or absent later, grouped by category
(WARN, INFO and SKIPPED are not walls). With csv to stdout, the summary goes to stderr.

Stdlib only; run with `python -I` so a planted module cannot shadow the stdlib.
Exit 0 on success, 2 on bad arguments or unreadable/malformed reports.
"""

import argparse
import csv
import io
import json
import sys
from collections import Counter
from pathlib import Path

SCHEMA = "xbox-probe/1"
ABSENT = "absent"
KNOWN_STATUSES = ["OK", "WARN", "INFO", "DENIED", "NOTIMPL", "MISSING", "CRASH", "SKIPPED"]
WALL_STATUSES = {"DENIED", "MISSING", "NOTIMPL", "CRASH", ABSENT}
VALUE_WIDTH = 48  # text-format truncation for value cells
CATEGORY_NAMES = {
    "A": "dynamic code exec", "B": "DLL inventory", "C": "NT syscalls",
    "D": "VEH/trap-emulate", "E": "CPU identity", "F": "filesystem",
    "G": "registry", "H": "token/sandbox", "I": "graphics", "J": "audio",
    "K": "networking", "L": "processes", "M": "system/memory", "N": "WinRT",
}


class ReportError(Exception):
    """A report that cannot be read or is not an xbox-probe report."""


def warn(msg):
    print(f"diff_reports: warning: {msg}", file=sys.stderr)


def load_report(path):
    """Parse one report. Returns (label, {(category, name): record})."""
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except FileNotFoundError:
        raise ReportError(f"{path}: file not found")
    except json.JSONDecodeError as exc:
        raise ReportError(f"{path}: malformed JSON ({exc.msg}, line {exc.lineno}, column {exc.colno})")
    except UnicodeDecodeError as exc:
        raise ReportError(f"{path}: not UTF-8 text ({exc.reason})")
    except OSError as exc:
        raise ReportError(f"{path}: cannot read ({exc.strerror or exc})")

    if not isinstance(data, dict) or not isinstance(data.get("records"), list):
        raise ReportError(f"{path}: not an xbox-probe report (no top-level 'records' list)")
    if data.get("schema") != SCHEMA:
        warn(f"{path}: schema is {data.get('schema')!r}, expected {SCHEMA!r}")

    # Column label: environment.environment, falling back to the file name.
    env = data.get("environment")
    label = str(env.get("environment") or "").strip() if isinstance(env, dict) else ""
    label = label or Path(path).stem

    records = {}
    for i, rec in enumerate(data["records"]):
        if not isinstance(rec, dict) or "category" not in rec or "name" not in rec:
            warn(f"{path}: record #{i} has no category/name; skipped")
            continue
        key = (str(rec["category"]), str(rec["name"]))
        if key in records:
            warn(f"{path}: duplicate record {key[0]}/{key[1]}; keeping the first")
            continue
        records[key] = rec
    return label, records


def unique_labels(labels):
    """Disambiguate repeated environment labels as name#2, name#3, ..."""
    totals = Counter(labels)
    seen = Counter()
    result = []
    for lbl in labels:
        seen[lbl] += 1
        result.append(f"{lbl}#{seen[lbl]}" if totals[lbl] > 1 else lbl)
    return result


def status_of(rec):
    return str(rec.get("status") or "?")  # "?" flags a record with no status


def value_of(rec):
    value = rec.get("value")
    return "" if value is None else " ".join(str(value).split())


def shorten(text, fmt):
    """Truncate long values in text mode only; md and csv keep them whole."""
    if fmt != "text" or len(text) <= VALUE_WIDTH:
        return text
    return text[: VALUE_WIDTH - 3] + "..."


def wall_list(walls):
    return ", ".join(f"{env}={st}" for env, st in walls)


def build_rows(records_list):
    """One dict per (category, name) key across all reports, sorted by key."""
    rows = []
    for key in sorted(set().union(*records_list)):
        cells = [recs.get(key) for recs in records_list]
        statuses = [ABSENT if c is None else status_of(c) for c in cells]
        values = [value_of(c) for c in cells] if all(s == "OK" for s in statuses) else None
        rows.append({
            "category": key[0],
            "name": key[1],
            "statuses": statuses,
            "changed": len(set(statuses)) > 1,  # a differing status, or absent somewhere
            "values": values,  # compared only for rows OK everywhere
            "values_differ": values is not None and len(set(values)) > 1,
        })
    return rows


def find_regressions(rows, labels):
    """{category: [(name, [(env, status), ...]), ...]} for baseline-OK keys that wall later."""
    grouped = {}
    for row in rows:
        if row["statuses"][0] != "OK":
            continue
        walls = [(labels[i], st) for i, st in enumerate(row["statuses"])
                 if i > 0 and st in WALL_STATUSES]
        if walls:
            grouped.setdefault(row["category"], []).append((row["name"], walls))
    return grouped


def status_counts(records_list):
    counters = [Counter(status_of(r) for r in recs.values()) for recs in records_list]
    seen = set().union(*counters)
    order = [s for s in KNOWN_STATUSES if s in seen] + sorted(seen - set(KNOWN_STATUSES))
    return order, counters


def text_table(headers, rows):
    """Fixed-width table: columns sized to their widest cell."""
    widths = [max(len(str(x)) for x in col) for col in zip(headers, *rows)]

    def line(cells):
        return "  ".join(c.ljust(w) for c, w in zip(cells, widths)).rstrip()

    return [line(headers), line(["-" * w for w in widths])] + [line(r) for r in rows]


def md_table(headers, rows):
    def cell(text):
        return str(text).replace("|", "\\|")

    out = ["| " + " | ".join(cell(h) for h in headers) + " |", "|" + "---|" * len(headers)]
    out += ["| " + " | ".join(cell(c) for c in r) + " |" for r in rows]
    return out


def matrix_table(rows, labels, show_values, fmt):
    headers = ["changed", "category", "name"] + labels
    if show_values:
        headers += [f"value@{lbl}" for lbl in labels] + ["note"]
    body = []
    for row in rows:
        line = ["yes" if row["changed"] else "", row["category"], row["name"]] + row["statuses"]
        if show_values:
            if row["values"] is None:
                line += [""] * (len(labels) + 1)
            else:
                line += [shorten(v, fmt) for v in row["values"]]
                line.append("value differs" if row["values_differ"] else "")
        body.append(line)
    return headers, body


def heading(text, md, level):
    return ("#" * level + " " + text) if md else f"== {text} =="


def render_summary(rows, labels, records_list, md):
    """Summary lines: key totals, status counts per environment, regressions."""
    order, counters = status_counts(records_list)
    base_ok = [row for row in rows if row["statuses"][0] == "OK"]
    body = [[s] + [str(c[s]) for c in counters] for s in order]
    body.append(["records"] + [str(len(recs)) for recs in records_list])
    body.append(["walls vs baseline", "-"] + [
        str(sum(row["statuses"][i] in WALL_STATUSES for row in base_ok))
        for i in range(1, len(labels))])
    headers = ["status"] + labels
    table = md_table(headers, body) if md else [f"  {t}" for t in text_table(headers, body)]

    regress = find_regressions(rows, labels)
    changed = sum(row["changed"] for row in rows)
    out = [
        heading("SUMMARY", md, 2),
        f"Keys: {len(rows)} total, {changed} changed (status differs or absent).",
        "",
        heading("Status counts per environment", md, 3),
        "",
        *table,
        "",
        heading(f"Regressions vs baseline '{labels[0]}'", md, 3),
        "OK in the baseline but DENIED, MISSING, NOTIMPL, CRASH or absent in a later report:",
        "",
    ]
    if not regress:
        out.append("(none)")
    for cat, items in regress.items():
        title = f"[{cat}] {CATEGORY_NAMES.get(cat, '')}".rstrip()
        if md:
            out += [f"**{title}**"] + [f"- `{n}`: {wall_list(w)}" for n, w in items] + [""]
        else:
            out += [f"  {title}"] + [f"    {n}: {wall_list(w)}" for n, w in items]
    total = sum(len(items) for items in regress.values())
    out.append(f"Total: {total} key(s) walled in at least one later environment.")
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Compare xbox-probe/1 reports: status matrix, status counts and regressions.",
        epilog="example: python -I diff_reports.py baseline.json laptop-uwp.json xbox-uwp.json "
               "--only-changed --format md --out matrix.md",
    )
    parser.add_argument("reports", nargs="+", metavar="REPORT",
                        help="report JSON files; the first one is the baseline (give 2 or more)")
    parser.add_argument("--out", metavar="FILE", help="write output to FILE instead of stdout")
    parser.add_argument("--format", choices=["text", "md", "csv"], default="text",
                        help="matrix format (default: text)")
    parser.add_argument("--only-changed", action="store_true",
                        help="show only rows whose status differs or that are absent somewhere")
    parser.add_argument("--values", action="store_true",
                        help="append per-environment values for rows OK everywhere; flag differing values")
    args = parser.parse_args(argv)
    if len(args.reports) < 2:
        parser.error("need at least two reports (the first is the baseline)")

    for stream in (sys.stdout, sys.stderr):  # odd device-name characters must not crash a cp1252 console
        getattr(stream, "reconfigure", lambda **_: None)(errors="replace")

    loaded, failed = [], False
    for path in args.reports:
        try:
            loaded.append(load_report(path))
        except ReportError as exc:
            print(f"diff_reports: error: {exc}", file=sys.stderr)
            failed = True
    if failed:
        return 2

    labels = unique_labels([label for label, _ in loaded])
    records_list = [recs for _, recs in loaded]
    rows = build_rows(records_list)
    shown = [row for row in rows if row["changed"]] if args.only_changed else rows
    md = args.format == "md"

    headers, body = matrix_table(shown, labels, args.values, args.format)
    if args.format == "csv":
        buf = io.StringIO()
        csv.writer(buf, lineterminator="\n").writerows([headers, *body])
        matrix_text = buf.getvalue()
    else:
        table = md_table(headers, body) if md else text_table(headers, body)
        if not body:
            table.append("(no changed rows)" if args.only_changed else "(no records)")
        matrix_text = "\n".join(table) + "\n"
    summary_text = "\n".join(render_summary(rows, labels, records_list, md)) + "\n"

    # CSV keeps stdout pure: the summary goes to stderr (or stdout when --out is used).
    if args.format == "csv":
        main_text, side_text = matrix_text, summary_text
    else:
        main_text, side_text = matrix_text + "\n" + summary_text, ""

    if args.out:
        try:
            with open(args.out, "w", encoding="utf-8", newline="") as fh:
                fh.write(main_text)
        except OSError as exc:
            print(f"diff_reports: error: cannot write {args.out}: {exc.strerror or exc}", file=sys.stderr)
            return 2
        if side_text:
            sys.stdout.write(side_text)
        print(f"diff_reports: wrote {args.out}", file=sys.stderr)
    else:
        sys.stdout.write(main_text)
        sys.stderr.write(side_text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
