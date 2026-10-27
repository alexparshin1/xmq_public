#!/usr/bin/env python3
"""
Writes the website's chaos-test file from one chaos run's log.

    ./site_chaos.py --site ~/workspace/xmq_web_site results/Cluster/chaos-2026-10-27/chaos.log

The log is what chaos_run.sh printed: the plan, the load, the timeline, every check with the
window it held for, and the summary line. The output keeps the run's shape - one entry per crash
cycle, the checks it made, and the verdict - and leaves out the publish-phase table: this run
answers whether the cluster keeps working while a node is killed, not how fast it delivers, and a
latency column taken across a 20-second outage would say nothing either way.

The file is written to the site as JSON because the page renders a table, not a benchmark curve.
"""

import argparse
import json
import pathlib
import re

# "  PASS  node2 serves: held from 35s to 45s, 3 look(s)"
CHECK = re.compile(r"^\s*(PASS|FAIL)\s+(.*?)(?::\s*(.*))?$")
# "+45.0s  PASS  node-b serves: held from 35s to 45s, 3 look(s)" - the same check as it happens,
# stamped with the run's clock. The plain form appears again in the closing summary, where the
# cycle it belongs to has to be inferred; the stamped one does not.
TIMED_CHECK = re.compile(r"^\+(\d+(?:\.\d+)?)s\s+(PASS|FAIL)\s+(.*?)(?::\s*(.*))?$")
# "+30.0s  node1 stop (0.0s)"
# The cluster names its nodes node-a..node-d in the AWS runs and node1..nodeN on the home stand;
# both are the same process, and the page only needs the name to match the checks.
EVENT = re.compile(r"^\+(\d+\.\d+)s\s+(node[\w-]+) (stop|start)\b")


def parse(log_text):
    lines = log_text.splitlines()
    run = {
        "title": lines[0].strip() if lines else "",
        "nodes": [],
        "load": {},
        "cycles": [],
        "final_checks": [],
        "passed": 0,
        "failed": 0,
        "logs_clean": False,
        "raw_summary": "",
    }
    for line in lines:
        if line.startswith("nodes:"):
            run["nodes"] = line.split(":", 1)[1].split()

    # "chaos: 6 crashes over 600s, one node down 20s per 90s cycle"
    shape = re.search(r"(\d+) crashes over (\d+)s, one node down (\d+)s per (\d+)s cycle", run["title"])
    if shape:
        run["crashes_planned"] = int(shape.group(1))
        run["duration"] = int(shape.group(2))
        run["down"] = int(shape.group(3))
        run["cycle"] = int(shape.group(4))

    # The crash cycles: every "stop" opens one and its "start" closes it. The checks that follow
    # a start belong to the same cycle - they are what proves the victim came back.
    # The final four checks (cross-node delivery, coordinator, membership) come after the last
    # cycle and are kept apart.
    cycle = None
    in_checks = False
    for line in lines:
        timed = TIMED_CHECK.match(line)
        if timed:
            check = {"result": timed.group(2), "text": timed.group(3).strip(),
                     "detail": (timed.group(4) or "").strip()}
            # In-cycle checks are the windowed ones, including the ones that prove the victim is
            # back in service. The closing checks are one-shot: the members, the coordinator and a
            # message still crossing, made after the last cycle.
            if "held from" in check["detail"] and cycle is not None:
                cycle["checks"].append(check)
            else:
                run["final_checks"].append(check)
            continue

        event = EVENT.match(line)
        if event:
            seconds, node, what = float(event.group(1)), event.group(2), event.group(3)
            if what == "stop":
                cycle = {"victim": node, "stop_s": seconds, "start_s": None, "checks": []}
                run["cycles"].append(cycle)
            elif cycle is not None and cycle["victim"] == node:
                cycle["start_s"] = seconds
            continue

        if line.startswith("Checks:"):
            in_checks = True
            continue
        if not in_checks:
            continue

        match = CHECK.match(line)
        if match:
            result, text, detail = match.group(1), match.group(2).strip(), (match.group(3) or "").strip()
            # The closing summary repeats every check. The ones made inside a window are already
            # recorded, with their cycle, from the stamped lines above; what is left here are the
            # one-shot closing checks, which belong to no cycle.
            if not check_belongs_to_cycle(cycle, text, detail):
                check = {"result": result, "text": text, "detail": detail}
                if check not in run["final_checks"]:
                    run["final_checks"].append(check)

        summary = re.match(r"^Cluster test '.*':\s*(\d+) passed,\s*(\d+) failed\.", line)
        if summary:
            run["passed"], run["failed"] = int(summary.group(1)), int(summary.group(2))
            run["raw_summary"] = line.strip()

        if line.startswith("node logs:"):
            run["logs_clean"] = "no crash" in line
            run["node_logs"] = line.split(":", 1)[1].strip()

    for index, cycle in enumerate(run["cycles"], start=1):
        cycle["number"] = index
        cycle["down_s"] = round(cycle["start_s"] - cycle["stop_s"]) if cycle["start_s"] else None
        cycle["passed"] = sum(1 for c in cycle["checks"] if c["result"] == "PASS")
        cycle["failed"] = sum(1 for c in cycle["checks"] if c["result"] == "FAIL")
    return run


def check_belongs_to_cycle(cycle, text, detail):
    """Whether a check from the closing summary was already recorded from the live timeline.

    The checks made inside a window carry the window ("held from 35s to 45s") and were attached to
    their cycle as they happened; the closing checks are one-shot and carry the fact instead
    ("'chaos' arrived on ...", "ZCARD ... = '3'").
    """
    return "held from" in detail


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--site", help="path to xmq_web_site; writes src/xmq_test_results/Chaos.json")
    parser.add_argument("--out", help="write the JSON here instead of into the site")
    parser.add_argument("--server", default="", help="what ran, e.g. 'XMQ 1.0.0, commit 4919dd6'")
    parser.add_argument("log")
    args = parser.parse_args()

    run = parse(pathlib.Path(args.log).read_text())
    run["server"] = args.server
    text = json.dumps(run, indent=1)

    if args.out:
        pathlib.Path(args.out).write_text(text + "\n")
        print(f"wrote {args.out}")
    if args.site:
        target = pathlib.Path(args.site) / "src" / "xmq_test_results" / "Chaos.json"
        target.write_text(text + "\n")
        print(f"wrote {target}")


if __name__ == "__main__":
    main()
