#!/usr/bin/env python3
"""
Writes the website's result files (xmq_web_site/src/xmq_test_results/*.txt) from scenario-set
records, one file per test with one block per broker. The file names are the tests' and never
change; dates, broker versions and conditions are inside - and nothing else: no earlier runs, no notes
carried over from older files. What a test page shows is exactly what the records of one campaign
measured.

    ./site_results.py --site ~/workspace/xmq_web_site --env Bench record-XMQ-0.9.20.txt record-FlashMQ-1.27.2.txt ...
    ./site_results.py --out results/Bench/2026-10-07 --env Bench record-*.txt     a campaign kept for comparison

The records are run_scenario_set.sh --record files. Each broker block carries the meta the site
reads (Version, CPU Load, Max RAM), the broker's settings, a Status line when the scenario did not
finish, and the interval table with the broker's RSS in each interval as its last column.
"""

import argparse
import pathlib
import re
import sys

# Scenario in the records -> (site file, what the test is, for the file's preamble).
TESTS = {
    "Fan-In-50K-500-50K-50K": (
        "Fan-In.txt",
        "Fan-In 50K (shared subscription) - Fan-In-50K-500-50K-50K.json\n"
        "          50000 publishers / 50000 topics / 500 subscribers on one shared subscription\n"
        "          ($share/benchmark/test/#), 1 msg/s per publisher = 50k/s aggregate."),
    "Fan-Out-5-1000-5-250K-5min": (
        "Fan-Out.txt",
        "Fan-Out 250K - Fan-Out-5-1000-5-250K-5min.json\n"
        "          5 publishers / 5 topics / 1000 subscribers, 50 msg/s per publisher = 250k/s delivered,\n"
        "          5 minutes."),
    "Point-To-Point-50K-50K-50K-100K": (
        "Point-To-Point.txt",
        "Point-To-Point 50K - Point-To-Point-50K-50K-50K-100K.json\n"
        "          50000 publishers / 50000 subscribers, one topic each, 2 msg/s per publisher =\n"
        "          100k/s aggregate, 100000 connections."),
    "Point-To-Point-60K-persistent": (
        "Persistence.txt",
        "Point-To-Point 60K, persistent sessions - Point-To-Point-60K-persistent.json\n"
        "          60000 publishers / 60000 subscribers with persistent sessions, 60k/s aggregate.\n"
        "          Each broker with the persistence it offers, empty at the start: XMQ writes every\n"
        "          message to Redis; HiveMQ file persistence; EMQX durable sessions; FlashMQ saves its\n"
        "          sessions to storage_dir periodically and at stop; Mosquitto saves an interval snapshot."),
    "500K-Connections-5000-rate": (
        "Connections.txt",
        "500K Connections - 500K-Connections-5000-rate.json\n"
        "          500000 clients connecting at 5000/s; latency is the connect time."),
    # The same test before 2026-10-08, at half the connection rate.
    "500K-Connections-2500-rate": (
        "Connections.txt",
        "500K Connections - 500K-Connections-2500-rate.json\n"
        "          500000 clients connecting at 2500/s; latency is the connect time."),
}

# The names the site charts, from the records' Server lines.
BROKERS = {"XMQ": "XMQ", "FlashMQ": "FlashMQ", "Mosquitto": "Mosquitto", "EMQX": "EMQX", "HiveMQ CE": "HiveMQ"}


def parse_record(path):
    text = path.read_text()
    header = {}
    for line in text.splitlines():
        m = re.match(r"^(Server|Version|Host|Client|Date):\s+(.*)$", line)
        if m and m.group(1) not in header:
            header[m.group(1)] = m.group(2).strip()
        if line.startswith("Summary -"):
            break

    blocks = {}
    current = None
    for line in text.splitlines():
        m = re.match(r"^Scenario:\s+(\S+)", line)
        if m:
            current = m.group(1)
            blocks[current] = []
            continue
        if current is not None:
            blocks[current].append(line)
    for lines in blocks.values():
        while lines and not lines[-1].strip():
            lines.pop()
    return header, blocks


def broker_block(header, lines):
    name = BROKERS.get(header.get("Server", ""), header.get("Server", ""))
    host = header.get("Host", "")
    settings = host.split("; ", 1)[1] if "; " in host else ""

    cpu = ram = status = None
    table, memory = [], []
    in_memory = False
    for line in lines:
        stripped = line.strip()
        if m := re.match(r"server CPU\s+mean (\S+), peak (\S+)", stripped):
            cpu = f"{m.group(1)} mean / {m.group(2)} peak"
        elif m := re.match(r"server RSS\s+(\d+) Mb peak", stripped):
            mb = int(m.group(1))
            ram = f"{mb / 1024:.2f} Gb" if mb >= 1024 else f"{mb} Mb"
        elif re.match(r"^(FAILED|INCOMPLETE|NO RESULT)", stripped):
            status = stripped
        elif stripped == "Memory":
            in_memory = True
        elif in_memory:
            if re.match(r"^\d+ms\s", stripped) or stripped.startswith("Interval"):
                memory.append(line)
        elif re.match(r"^(Interval|\d+ms\s|─|Average|Median)", stripped):
            table.append(line)

    out = [f"Server:   {name}", f"Version:  {header.get('Version', '')}"]
    if settings:
        out.append(f"Settings: {settings}")
    if cpu:
        out.append(f"CPU Load: {cpu}")
    if ram:
        out.append(f"Max RAM:  {ram}")
    if status:
        out.append(f"Status:   {status}")
    out += with_rss_column(table, memory)
    return "\n".join(out)


def with_rss_column(table, memory):
    """The interval table with the broker's RSS as its fifth column, from a record's Memory block."""
    rss = {}
    for line in memory:
        if m := re.match(r"^(\d+)ms\s+(\d+ (?:Mb|Gb))$", line.strip()):
            rss[m.group(1)] = m.group(2)
    rows = [line.rstrip() for line in table]
    # Newer records carry the RSS in the table already; it is summarised all the same.
    in_table = {m.group(1): m.group(2) for line in rows
                if (m := re.match(r"^(\d+)ms\s+\d+\s+\d+us\s+\d+\s+([\d.]+ (?:Mb|Gb))$", line))}
    if not rss and not in_table:
        return table
    # Where the RSS column begins: the header's width without it.
    width = max((len(line) - (11 if line.endswith("RSS") else 0) for line in rows if line.startswith("Interval")),
                default=0)
    # The Average and Median rows summarise the column like the others: over the intervals it has.
    mb = sorted(rss_mb(value) for interval, value in {**in_table, **rss}.items()
                if any(re.match(rf"^{interval}ms\s", line) for line in rows))
    summary = {}
    if mb:
        middle = len(mb) // 2
        median = mb[middle] if len(mb) % 2 else (mb[middle - 1] + mb[middle]) / 2
        summary = {"Average": f"{round(sum(mb) / len(mb))} Mb", "Median": f"{round(median)} Mb"}
    out = []
    for stripped in rows:
        if in_table and not rss:
            if (label := stripped.split(" ", 1)[0]) in summary:
                stripped = f"{stripped.ljust(width)}{summary[label]:>11}"
            out.append(stripped)
        elif stripped.startswith("Interval"):
            out.append(f"{stripped}{'RSS':>11}")
        elif m := re.match(r"^(\d+)ms\s", stripped):
            out.append(f"{stripped}{rss.get(m.group(1), '-'):>11}" if m.group(1) in rss else stripped)
        elif stripped.startswith("─"):
            out.append(stripped + "─" * 11)
        elif (label := stripped.split(" ", 1)[0]) in summary:
            out.append(f"{stripped.ljust(width)}{summary[label]:>11}")
        else:
            out.append(stripped)
    return out


def rss_mb(value):
    """'5204 Mb' or '1.2 Gb' in Mb."""
    number, unit = value.split()
    return float(number) * (1024 if unit == "Gb" else 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    where = parser.add_mutually_exclusive_group(required=True)
    where.add_argument("--site", type=pathlib.Path, help="The xmq_web_site checkout: writes its src/xmq_test_results.")
    where.add_argument("--out", type=pathlib.Path,
                       help="A directory of its own instead, e.g. results/Bench/2026-10-07, kept for comparing the next run.")
    parser.add_argument("--env", required=True, help="Where it ran, for the preamble: Bench or AWS.")
    parser.add_argument("--conditions", default="QoS 1, 16B payload, MQTT 5, 10 minutes per scenario unless stated.",
                        help="One line on the run's common conditions.")
    parser.add_argument("records", nargs="+", type=pathlib.Path)
    args = parser.parse_args()

    parsed = [parse_record(path) for path in args.records]
    first = parsed[0][0]
    target = args.site / "src" / "xmq_test_results" if args.site else args.out
    target.mkdir(parents=True, exist_ok=True)

    for scenario, (file_name, description) in TESTS.items():
        blocks = [broker_block(header, blocks[scenario]) for header, blocks in parsed if scenario in blocks]
        if not blocks:
            print(f"{scenario}: in none of the records, {file_name} left alone", file=sys.stderr)
            continue
        server_host = first.get("Host", "").split("; ", 1)[0]
        preamble = "\n".join([
            f"Scenario: {description}",
            f"          {args.conditions}",
            f"Date:     {first.get('Date', '')}",
            f"Server:   {args.env}: {server_host}",
            f"Client:   {first.get('Client', '')}",
            "",
            "Every broker restarted before each scenario, the others stopped. CPU percentages are of one",
            "hardware thread. A scenario that did not finish shows the intervals it completed.",
        ])
        (target / file_name).write_text(preamble + "\n\n\n" + "\n\n\n".join(blocks) + "\n")
        print(f"{file_name}: {len(blocks)} broker(s)")


if __name__ == "__main__":
    main()
