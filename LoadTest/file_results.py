#!/usr/bin/env python3
"""
Files a scenario-set record into the results tree, one file per test:

    LoadTest/results/{AWS,Bench}/{broker}/{version}/<test>.txt

The broker and the version come from the record's Server: and Version: lines, so FlashMQ 1.27.1
is filed under Bench/FlashMQ/1.27.1/ and XMQ 1.2.0 under Bench/XMQ/1.2.0/. Each file holds the
record's header - broker, version, host, client, date and
notes - followed by that test's own table, so it can be read and compared on its own.

The tree is what a release is checked against: release.sh refuses a version that has no results
here for the AWS set and the Bench set. A record written by run_scenario_set.sh --record is the
input; the raw per-run logs stay in results/raw/, which is not kept in git.

Filing a successful campaign replaces its directory whole rather than the files this record
happens to hold: one directory per version, never two campaigns mixed in one. Review the results
for regression against the previous version before filing; if there is a noticeable regression,
fix it and run the campaign again instead.

A campaign of XMQ that says a scenario failed is refused, whether or not a directory is there.
XMQ results are read for two things - a regression check against the previous version, and the
baseline the next version is measured against - and a campaign missing a scenario serves neither:
the missing number would read as "no change" in a comparison and as "not measured" in a baseline.
Nothing is lost by refusing it: the record and its logs stay in results/raw/<run>/, which is where
a failure is looked at. A record of another broker is filed as it stands, because there a failed
scenario is the finding - the broker could not run it - and that is what the comparison needs.

    ./file_results.py --env Bench results/raw/2026-10-04-xmq-0.9.19-bench/record.txt
"""

import argparse
import pathlib
import re
import shutil
import sys

RESULTS = pathlib.Path(__file__).resolve().parent / "results"

FAILURE_STATUS = (
    r"(?:setup |restart )?failed\b|broker did not come back\b|cannot create log directory\b"
    r"|invalid:|incomplete\b|no result\b"
)


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
    # worth keeping as much as a latency is. The reason is "failed (exit 1)", "setup failed" or
    # "restart failed", and every one of them says the scenario produced nothing.
    for line in lines:
        match = re.match(rf"^\s+(\S+)\s+((?:{FAILURE_STATUS}).*)$", line, re.IGNORECASE)
        if match:
            name, status = match.groups()
            if name not in tests:
                tests[name] = [f"Scenario: {name}", status]
            elif not any(re.match(FAILURE_STATUS, row.strip(), re.IGNORECASE) for row in tests[name][1:]):
                tests[name].append(status)

    for name, table in tests.items():
        while table and not table[-1].strip():
            table.pop()
    return header, tests


def failures(tests):
    """Tests whose block or summary marks the measurement as unsuccessful."""
    return [name for name, table in tests.items()
            if any(re.match(FAILURE_STATUS, row.strip(), re.IGNORECASE) for row in table[1:])]


def field(header, name):
    for line in header:
        if line.startswith(name + ":"):
            return line.split(":", 1)[1].strip()
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--env", required=True, choices=["AWS", "Bench"], help="Where the set ran.")
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

    # XMQ (anything) is still XMQ. Every broker has its own directory under the environment,
    # and each version has one directory inside it: Bench/FlashMQ/1.27.1/.
    broker = broker.split("(")[0].strip().replace(" ", "-")
    directory = RESULTS / args.env / broker / version

    # Current records are rewritten after each scenario. An interrupted set may contain only
    # successful tables; the completion marker is what distinguishes it from a complete set.
    if broker == "XMQ" and (field(header, "Elapsed so far") is not None or
                            (field(header, "Started") is not None and
                             not field(header, "Total execution time"))):
        sys.exit(f"{args.record}: campaign did not finish; existing results were not replaced")

    # One directory per version, and a re-test of that version replaces it whole. Replacing file
    # by file left a directory that mixed two campaigns: a scenario the new run did not have kept
    # its old table, and the release check cannot see that - it only asks that each file is not
    # empty.
    #
    # An XMQ campaign that lost a scenario is not filed at all, not even over an older directory of
    # the same version: its numbers are compared with the previous version and become the baseline
    # for the next one, and a hole in them is wrong in both. The record stays in results/raw/ with
    # its logs, which is where it is read. Another broker is filed as it stands: "cannot run this
    # scenario" is a result of its own, and the comparison tables are built from it.
    failed = failures(tests)
    if broker == "XMQ" and (failed or field(header, "Campaign status") == "failed"):
        detail = f"{', '.join(failed)} failed" if failed else "campaign failed"
        sys.exit(f"{args.record}: {detail}; an XMQ campaign missing a scenario is "
                 f"not filed - fix it and run the set again. The record and its logs stay in "
                 f"{args.record.parent}")
    if directory.is_dir() and any(directory.iterdir()):
        shutil.rmtree(directory)
        print(f"replaced {directory.relative_to(RESULTS.parent)}")
    directory.mkdir(parents=True, exist_ok=True)

    targets = [(directory / f"{name}.txt", table) for name, table in tests.items()]

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
