#!/usr/bin/env python3
"""Download the Wine PE builtins (wine-pe.yml artifacts) for a CI job. MANDATORY.

Without the builtins every guest end-to-end test SKIPs and ctest still reports
"100% tests passed" (docs/HANDOFF.md, trap 1), so this exits non-zero rather
than letting a job continue without them. Which run is used:

  1. a wine-pe run for THIS commit (wine-pe.yml runs when wine-pe/** changes):
     if it is still queued or running, wait for it (--wait-minutes); if it
     failed, fail, because the builtins this commit asks for do not exist;
  2. otherwise the newest successful run on this branch, then repository-wide,
     that still holds every requested artifact (artifacts expire after 30 days,
     and older runs do not carry wine-pe-include).

Every GitHub query is retried: a lookup that came back empty for a run that
existed has been seen on the windows runners (ci.yml run 39818491081, windows
Debug: "no successful wine-pe run yet" while the RelWithDebInfo job of the same
run found 39611835197).

  fetch_wine_pe.py --repo OWNER/NAME --branch BRANCH --sha SHA
                   --artifact wine-pe-builtin=wine-pe/out/builtin
                   [--artifact wine-pe-include=wine-pe/build/include] [--wait-minutes 45]

Needs `gh` with GH_TOKEN (actions: read). Prints the run id it used.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

WORKFLOW = "wine-pe.yml"


def gh_json(args, what, attempts=4):
    last = ""
    for i in range(1, attempts + 1):
        p = subprocess.run(["gh"] + args, capture_output=True, text=True)
        if p.returncode == 0 and p.stdout.strip():
            try:
                return json.loads(p.stdout)
            except ValueError as e:
                last = f"unparsable output ({e}): {p.stdout[:200]!r}"
        else:
            last = f"exit {p.returncode}: {(p.stderr or p.stdout).strip()[:300]}"
        print(f"  {what}: attempt {i} failed ({last}); retrying in {5 * i}s", flush=True)
        time.sleep(5 * i)
    raise RuntimeError(f"{what}: {last}")


def runs(repo, query):
    d = gh_json(["api", f"repos/{repo}/actions/workflows/{WORKFLOW}/runs?{query}"], f"list wine-pe runs ({query})")
    return d.get("workflow_runs", [])


def live_artifacts(repo, run_id):
    d = gh_json(["api", f"repos/{repo}/actions/runs/{run_id}/artifacts?per_page=100"], f"artifacts of run {run_id}")
    return {a["name"] for a in d.get("artifacts", []) if not a.get("expired")}


def download(repo, run_id, name, dest):
    # `gh run download` refuses to overwrite ("file exists"), so a rerun on a laptop (tools/dev/prepare-windows.ps1)
    # failed after the first run: reuse the directory when it already holds this very run's artifact (marker file),
    # otherwise start from an empty one.
    marker = os.path.join(dest, ".wine-pe-run")
    if os.path.isdir(dest) and os.listdir(dest):
        try:
            with open(marker) as f:
                have = f.read().strip()
        except OSError:
            have = ""
        if have == f"{run_id} {name}":
            print(f"  {name} from run {run_id} already in {dest}", flush=True)
            return
        shutil.rmtree(dest)
    os.makedirs(dest, exist_ok=True)
    for i in range(1, 4):
        p = subprocess.run(["gh", "run", "download", str(run_id), "--repo", repo, "--name", name, "--dir", dest],
                           capture_output=True, text=True)
        if p.returncode == 0 and os.listdir(dest):
            with open(marker, "w") as f:
                f.write(f"{run_id} {name}\n")
            return
        print(f"  download {name} from run {run_id}: attempt {i} failed "
              f"(exit {p.returncode}: {(p.stderr or p.stdout).strip()[:300]}); retrying in {5 * i}s", flush=True)
        time.sleep(5 * i)
    raise RuntimeError(f"could not download {name} from run {run_id}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--branch", required=True)
    ap.add_argument("--sha", required=True)
    ap.add_argument("--artifact", action="append", required=True, help="NAME=DEST")
    ap.add_argument("--wait-minutes", type=float, default=45)
    a = ap.parse_args()
    wanted = dict(s.split("=", 1) for s in a.artifact)

    # 1. A wine-pe run for this very commit wins, finished or not.
    chosen = None
    same = runs(a.repo, f"head_sha={a.sha}&per_page=10")
    if same:
        run = max(same, key=lambda r: r["run_number"])
        deadline = time.time() + a.wait_minutes * 60
        while run["status"] != "completed":
            if time.time() > deadline:
                print(f"::error::wine-pe run {run['id']} for {a.sha[:7]} still {run['status']} after "
                      f"{a.wait_minutes:g} min; re-run this job once it has finished")
                return 1
            print(f"wine-pe run {run['id']} for this commit is {run['status']}; waiting for it "
                  f"({run['html_url']})", flush=True)
            time.sleep(30)
            run = gh_json(["api", f"repos/{a.repo}/actions/runs/{run['id']}"], f"run {run['id']}")
        if run["conclusion"] != "success":
            print(f"::error::wine-pe run {run['id']} for this commit concluded '{run['conclusion']}': the Wine PE "
                  f"builtins this commit asks for were not built ({run['html_url']})")
            return 1
        missing = set(wanted) - live_artifacts(a.repo, run["id"])
        if missing:
            print(f"::error::wine-pe run {run['id']} for this commit has no {', '.join(sorted(missing))}")
            return 1
        chosen = run
        print(f"using wine-pe run {run['id']}, built from this commit")

    # 2. Newest successful run (branch, then repository) that still has every artifact.
    if not chosen:
        seen = set()
        for scope, query in (("branch " + a.branch, f"branch={a.branch}&status=success&per_page=20"),
                             ("repository", "status=success&per_page=30")):
            for run in sorted(runs(a.repo, query), key=lambda r: r["run_number"], reverse=True):
                if run["id"] in seen:
                    continue
                seen.add(run["id"])
                have = live_artifacts(a.repo, run["id"])
                if set(wanted) <= have:
                    chosen = run
                    print(f"using wine-pe run {run['id']} ({scope}, {run['head_branch']}@{run['head_sha'][:7]}, "
                          f"{run['created_at']})")
                    break
                print(f"  skipping wine-pe run {run['id']}: lacks {', '.join(sorted(set(wanted) - have))}")
            if chosen:
                break
    if not chosen:
        print("::error::no successful wine-pe run holds " + ", ".join(sorted(wanted)) +
              ". Without the Wine PE builtins every guest test would skip. Run the wine-pe workflow "
              "(workflow_dispatch) and re-run this job.")
        return 1

    for name, dest in wanted.items():
        download(a.repo, chosen["id"], name, dest)
        n = sum(1 for _ in os.scandir(dest))
        print(f"downloaded {name} from run {chosen['id']} into {dest} ({n} entries)")
    builtin = wanted.get("wine-pe-builtin")
    if builtin and not os.path.isfile(os.path.join(builtin, "ntdll.dll")):
        print(f"::error::{builtin}/ntdll.dll missing after the download")
        return 1
    if builtin and os.path.isfile(os.path.join(builtin, "MANIFEST.txt")):
        print(open(os.path.join(builtin, "MANIFEST.txt"), encoding="utf-8", errors="replace").read())
    gh_out = os.environ.get("GITHUB_OUTPUT")
    if gh_out:
        with open(gh_out, "a", encoding="utf-8") as f:
            f.write(f"run_id={chosen['id']}\n")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as e:
        print(f"::error::fetch_wine_pe: {e}")
        sys.exit(1)
