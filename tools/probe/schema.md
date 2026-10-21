# xbox-probe/1 report schema

Each probe run writes one JSON report with schema `xbox-probe/1`. The emitter is
`probe-core/reporter.cpp`. The comparison tool is `tools/diff_reports.py`, documented
at the end of this file.

## Top-level fields

| field         | type            | meaning |
|---------------|-----------------|---------|
| `schema`      | string          | Always `"xbox-probe/1"`. Check it before reading the rest. |
| `environment` | object          | Where and on what the probe ran. See below. |
| `records`     | array of object | One record per probe check. Unique per (`category`, `name`). |

```json
{
  "schema": "xbox-probe/1",
  "environment": { "host": "uwp", "environment": "xbox-uwp", "...": "..." },
  "records": [ { "category": "C", "name": "...", "...": "..." } ]
}
```

## environment object

| field         | type   | meaning |
|---------------|--------|---------|
| `host`        | string | `win32` (desktop harness) or `uwp` (packaged app). |
| `environment` | string | Label for the run, e.g. `laptop-uwp` or `xbox-uwp`. The diff tool uses it as the column header. |
| `device`      | string | Device name for the run. |
| `os_build`    | string | OS build number. |
| `arch`        | string | CPU architecture. |
| `note`        | string | Free text. |
| `started_utc` | string | Start time, ISO-8601 UTC. |

## record object

| field       | type             | meaning |
|-------------|------------------|---------|
| `category`  | string           | One letter, `A` to `N`. See the legend below. |
| `name`      | string           | Probe name. Unique within its category. |
| `status`    | string           | One of the status values below. |
| `errorcode` | integer (uint32) | Native error from the call: `GetLastError` (Win32), `NTSTATUS`, or `HRESULT`, depending on the probe. `0` when the call succeeded or no code applies. |
| `detail`    | string           | Human-readable explanation: the error text, or what was attempted. |
| `value`     | string           | The measured value. Free-form, often `k=v;k=v`. Empty when there is none. |

Records are keyed by the pair (`category`, `name`). That pair is the identity used
for comparison. A report should not repeat a key. If it does, `diff_reports.py`
warns and keeps the first occurrence.

## Status values

| status    | meaning |
|-----------|---------|
| `OK`      | Capability present, or the call succeeded. |
| `DENIED`  | Blocked by the sandbox, e.g. `ACCESS_DENIED` or `E_ACCESSDENIED`. |
| `NOTIMPL` | Present but not implemented (`STATUS_NOT_IMPLEMENTED`). |
| `MISSING` | Symbol, DLL or API not found at all. |
| `CRASH`   | The probe body faulted. The harness caught it (SEH or C++ exception) and recorded it. |
| `INFO`    | A measurement with no pass/fail meaning. |
| `WARN`    | Succeeded, with a caveat worth reading in `detail`. |
| `SKIPPED` | Deliberately not attempted in this environment. |

The statuses that count as walls in the diff tool's regression list are `DENIED`,
`MISSING`, `NOTIMPL` and `CRASH`, plus a key that is absent from a report. `WARN`,
`INFO` and `SKIPPED` are not walls.

## Category legend

| letter | category |
|--------|----------|
| A | dynamic code execution |
| B | DLL inventory |
| C | NT syscalls |
| D | VEH / trap-and-emulate |
| E | CPU identity |
| F | filesystem |
| G | registry |
| H | token / sandbox |
| I | graphics |
| J | audio |
| K | networking |
| L | processes |
| M | system / memory |
| N | WinRT |

## Example record

A denied NT call. `errorcode` is the NTSTATUS `STATUS_ACCESS_DENIED` (`0xC0000022`)
written as an unsigned 32-bit integer.

```json
{"category":"C","name":"NtQuerySystemInformation","status":"DENIED","errorcode":3221225506,"detail":"STATUS_ACCESS_DENIED (0xC0000022)","value":""}
```

## Comparing reports: tools/diff_reports.py

The tool is stdlib-only Python 3. Run it with `python -I` so a stray module in the
working directory cannot shadow the standard library.

```
python -I tools\diff_reports.py BASELINE.json OTHER.json [MORE.json ...]
       [--out FILE] [--format text|md|csv] [--only-changed] [--values]
```

- The first report is the baseline. Columns follow argument order. Each column is
  labelled by `environment.environment`, or by the file name if that is empty.
- A row is **changed** when its status differs between reports, or the key is absent
  from some report (shown as `absent`).
- `--only-changed` keeps only changed rows. These are the capabilities the sandbox strips.
- `--values` adds a value column per environment for rows that are `OK` everywhere,
  and notes `value differs` where the values disagree. Use it for numbers such as a
  memory budget or a feature level.
- `--format` chooses `text` (fixed width, the default), `md` (GitHub table) or `csv`.
  With `csv` written to stdout, the summary goes to stderr so stdout stays clean CSV.
- The **summary** lists status counts per environment, the number of baseline-`OK`
  keys walled in each later environment, and **REGRESSIONS**: keys that are `OK` in the
  baseline but `DENIED`, `MISSING`, `NOTIMPL`, `CRASH` or absent later, grouped by category.
- Exit status is `0` on success and `2` on bad arguments or an unreadable or malformed
  report. Missing files and malformed JSON produce a clear message on stderr.

Example, for the three real reports:

```
python -I C:\Users\10harbad\Documents\win10xbox\tools\diff_reports.py baseline.json laptop-uwp.json xbox-uwp.json --only-changed --format md --out matrix.md
```
