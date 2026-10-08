#!/usr/bin/env python3
"""
Files a scenario-set record into the results tree, one file per test:

    LoadTest/results/{AWS,Bench}/{version}/<test>.txt

{version} is the XMQ version as it is ("0.9.19"), and "<Broker>-<version>" for any other broker
("FlashMQ-1.27.1"). Each file holds the record's header - broker, version, host, client, date and
notes - followed by that test's own table, so it can be read and compared on its own.

The tree is what a release is checked against: release.sh refuses a version that has no results
here for the AWS set and the Bench set. A record written by run_scenario_set.sh --record is the
input; the raw per-run logs stay in results/raw/, which is not kept in git.

    ./file_results.py --env Bench results/raw/2026-10-04-xmq-0.9.19-bench/record.txt
"""

import argparse
import pathlib
import re
import sys

RESULTS = pathlib.Path(__file__).resolve().parent / "results"


def parse(record_text):
    """Split a record into its header lines and {test: table lines}."""
    lines = record_text.splitlines()

    header = []
    for line in lines:
        if line.startswith("Summary -"):
            break
        header.append(line)
    while header and not header[-1].strip():
        header.pop()

    tests = {}
    current = None
    for line in lines:
        match = re.match(r"^Scenario:\s+(\S+)", line)
        if match:
            current = match.group(1)
            # A test run twice in one record keeps its last table, which is the one that counted.
            tests[current] = [line]
            continue
        if current is not None:
            tests[current].append(line)

    # A test that failed has no table, only its line in the summary - and a failure is a result
    # worth keeping as much as a latency is.
    for line in lines:
        match = re.match(r"^\s+(\S+)\s+(failed.*)$", line)
        if match and match.group(1) not in tests:
            tests[match.group(1)] = [f"Scenario: {match.group(1)}", match.group(2)]

    for name, table in tests.items():
        while table and not table[-1].strip():
            table.pop()
    return header, tests


def field(header, name):
    for line in header:
        if line.startswith(name + ":"):
            return line.split(":", 1)[1].strip()
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--env", required=True, choices=["AWS", "Bench"], help="Where the set ran.")
    parser.add_argument("--force", action="store_true", help="Replace results already filed.")
    parser.add_argument("--broker", help="Broker name, when the record's Server: line is wrong - "
                                         "records of other brokers were once written as XMQ.")
    parser.add_argument("record", type=pathlib.Path, help="Record written by run_scenario_set.sh --record.")
    args = parser.parse_args()

    header, tests = parse(args.record.read_text())
    if args.broker:
        header = [f"Server:   {args.broker}" if line.startswith("Server:") else line for line in header]
    broker = field(header, "Server")
    version = field(header, "Version")
    if not broker or not version:
        sys.exit(f"{args.record}: no Server: or Version: line in the header")
    if not tests:
        sys.exit(f"{args.record}: no test tables")

    # "XMQ (anything)" is still XMQ; any other broker carries its name into the directory.
    broker = broker.split("(")[0].strip().replace(" ", "-")
    directory = RESULTS / args.env / (version if broker == "XMQ" else f"{broker}-{version}")
    directory.mkdir(parents=True, exist_ok=True)

    # Every target is checked before any is written. Checked one at a time inside the write loop,
    # the first file that already existed ended the run with the files before it already replaced:
    # a version filed twice left a directory of tables from two different campaigns, and the
    # release check cannot see that - it only asks that each file is not empty.
    targets = []
    for name, table in tests.items():
        target = directory / f"{name}.txt"
        if target.exists() and not args.force:
            sys.exit(f"{target} exists; --force replaces it")
        targets.append((target, table))

    for target, table in targets:
        # Written beside the target and moved onto it, so a filing interrupted partway leaves the
        # old file or the new one and never half a table - which the release check, asking only
        # that the file is not empty, would take for a result.
        staged = target.with_name(target.name + ".tmp")
        staged.write_text("\n".join(header) + "\n\n" + "\n".join(table) + "\n")
        staged.replace(target)
        print(target.relative_to(RESULTS.parent))


if __name__ == "__main__":
    main()
