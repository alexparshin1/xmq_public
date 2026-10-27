#!/usr/bin/env python3
"""
Writes the website's cluster result files from the per-broker blocks of one campaign.

    ./site_cluster.py --site ~/workspace/xmq_web_site \
        --xmq results/xmq-20261027-101421 --emqx results/emqx-20261027-105742

Each input directory is a run set whose per-test files were written by format_cluster.py
(site-<scenario>.txt): a conditions line, a date, and one broker block. This joins the two brokers'
blocks for a test into the one file the Cluster page reads, with a single campaign preamble - the
conditions are the stand's, not the broker's, and repeating them per broker would make the page
print them twice.

The comparison keeps both brokers at the same pace on purpose. Anything that only one of them can
sustain belongs in its own section, not in a table that reads as head-to-head.
"""

import argparse
import pathlib

TESTS = {
    "Cluster-4-500K-Connections": "Cluster-Connections.txt",
    "Cluster-4-Fan-In-50K-500-50K-50K": "Cluster-Fan-In.txt",
    "Cluster-4-Fan-Out-5-1000-5-250K-5min": "Cluster-Fan-Out.txt",
    "Cluster-4-P2P-50K-50K-50K-100K": "Cluster-P2P.txt",
}

PREAMBLE = (
    "Four-node cluster on AWS: 4 x c5n.2xlarge in us-east-1a, one broker installed at a time,\n"
    "          MQTT 5 on port 1884, QoS 1, 16-byte payload, cluster restarted and shared store\n"
    "          flushed before every scenario. Latency is end-to-end delivery."
)


def broker_block(path):
    """The part of a site-<scenario>.txt that starts at Server: and runs to the end."""
    lines = pathlib.Path(path).read_text().splitlines()
    start = next((i for i, line in enumerate(lines) if line.startswith("Server:")), None)
    if start is None:
        raise SystemExit(f"no Server: block in {path}")
    block = lines[start:]
    # One Date line per file is enough; the block repeats it below Host/Settings.
    seen = 0
    for i, line in enumerate(block):
        if line.startswith("Date:"):
            seen += 1
            if seen > 1:
                block[i] = None
    return [line for line in block if line is not None]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--site", help="path to xmq_web_site; writes src/xmq_test_results/")
    parser.add_argument("--out", help="write the files here instead")
    parser.add_argument("--xmq", required=True)
    parser.add_argument("--emqx", required=True)
    args = parser.parse_args()

    target = pathlib.Path(args.out) if args.out else pathlib.Path(args.site) / "src" / "xmq_test_results"
    target.mkdir(parents=True, exist_ok=True)

    for scenario, name in TESTS.items():
        parts = [PREAMBLE, ""]
        parts += broker_block(pathlib.Path(args.xmq) / f"site-{scenario}.txt")
        parts += [""]
        parts += broker_block(pathlib.Path(args.emqx) / f"site-{scenario}.txt")
        (target / name).write_text("\n".join(parts) + "\n")
        print(f"wrote {target / name}")


if __name__ == "__main__":
    main()
