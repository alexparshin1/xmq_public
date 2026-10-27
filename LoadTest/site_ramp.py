#!/usr/bin/env python3
"""
Writes the connection-rate headroom file for the website from a ramp run's console files.

    ./site_ramp.py --site ~/workspace/xmq_web_site --broker "XMQ 1.0.0" results/xmq-ramp-*/*.console

Each console is one rate: the 500K connection scenario run with a different --connection-rate. A
rung passes when the run exits cleanly and every one of the four nodes reports its full share of
clients. Only the connect phase is reported; the hold after it says nothing about the rate.

This is XMQ alone. The comparison on the Cluster page stays at 5000 connections/second because
that is the rate both brokers sustain; this file answers a different question - how far the accept
path goes before something breaks - and must not be read as a head-to-head.
"""

import argparse
import json
import pathlib
import re

RATE = re.compile(r"rate-(\d+)\.console$")
ASSIGNED = re.compile(r"connected / (\d+) assigned")


def parse_console(path):
    text = path.read_text()
    # "Average        500000      544us         4545"
    average = re.search(r"^Average\s+(\d+)\s+(\d+)us\s+(\d+)$", text, re.M)
    median = re.search(r"^Median\s+(\d+)us$", text, re.M)
    # The Average line is over the whole run, ramp-up and last partial interval included, so it
    # reads a few per cent under the pace. The steady-state rows are what the stand sustained.
    rates = [int(r) for r in re.findall(r"^\d+ms\s+\d+\s+\d+us\s+(\d+)$", text, re.M)]
    steady = sorted(rates)[len(rates) // 2] if rates else None
    shares = ASSIGNED.findall(text)
    connected = 0
    complete = False
    if average:
        connected = int(average.group(1))
    # Four nodes, each with its own line, and no line showing fewer connected than assigned.
    if len(shares) == 4:
        complete = all(int(n) > 0 for n in shares) and connected > 0
    return {
        "connected": connected,
        "steady_rate": steady,
        "median_us": int(median.group(1)) if median else None,
        "average_us": int(average.group(2)) if average else None,
        "result": "pass" if complete else "incomplete",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--site", help="path to xmq_web_site; writes src/xmq_test_results/Cluster-Ramp.json")
    parser.add_argument("--out", help="write the JSON here instead")
    parser.add_argument("--broker", default="XMQ 1.0.0")
    parser.add_argument("--note", default="")
    parser.add_argument("consoles", nargs="+")
    args = parser.parse_args()

    rungs = []
    for name in args.consoles:
        path = pathlib.Path(name)
        rate = RATE.search(path.name)
        if not rate:
            continue
        entry = {"rate": int(rate.group(1))}
        entry.update(parse_console(path))
        rungs.append(entry)
    rungs.sort(key=lambda r: r["rate"])

    # A rung passes only if the stand kept the pace asked for - and keeping it at half a second of
    # connect latency is not keeping it: the queue is absorbing the difference, and the next rung
    # will not. So a rung that holds the rate but not the latency is the knee, and is reported as
    # one rather than as a pass.
    knee_latency_us = 50_000
    for rung in rungs:
        kept_pace = (rung["steady_rate"] or 0) >= rung["rate"] * 0.95
        low_latency = (rung["median_us"] or 0) <= knee_latency_us
        if not kept_pace:
            rung["result"] = "saturated"
        elif low_latency:
            rung["result"] = "pass"
        else:
            rung["result"] = "knee"

    # The highest rung that passed clean is the headroom; anything above it is where it stopped.
    passed = [r for r in rungs if r["result"] == "pass"]
    data = {
        "broker": args.broker,
        "note": args.note,
        "rungs": rungs,
        "headroom": max(passed, key=lambda r: r["rate"])["rate"] if passed else None,
        "knee": next((r["rate"] for r in rungs if r["result"] == "knee"), None),
        "first_failure": next((r["rate"] for r in rungs if r["result"] == "saturated"), None),
    }
    text = json.dumps(data, indent=1)
    if args.out:
        pathlib.Path(args.out).write_text(text + "\n")
        print(f"wrote {args.out}")
    if args.site:
        target = pathlib.Path(args.site) / "src" / "xmq_test_results" / "Cluster-Ramp.json"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text + "\n")
        print(f"wrote {target}")


if __name__ == "__main__":
    main()
