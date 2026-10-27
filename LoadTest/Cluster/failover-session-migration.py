#!/usr/bin/env python3
"""Measure queued QoS 1 delivery after a broker process dies and its session migrates."""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
import uuid
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
XMQ_BIN = Path(os.environ.get("XMQ_BIN", Path.home() / ".local" / "bin"))
if not (XMQ_BIN / "xmq_sub").exists():
    XMQ_BIN = Path(shutil.which("xmq_sub") or XMQ_BIN).parent


def run(command: list[str], *, input_text: str | None = None, timeout: int = 60,
        output: Path | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, input=input_text, text=True, capture_output=True, timeout=timeout)
    if output:
        output.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {shlex.join(command)}\n{result.stdout}{result.stderr}")
    return result


def redis(args: argparse.Namespace, *command: str) -> str:
    return run(["redis-cli", "-h", args.redis_host, "-p", str(args.redis_port), "-n", str(args.redis_db),
                "--raw", *command]).stdout.strip()


def scan_redis(args: argparse.Namespace, pattern: str) -> list[str]:
    result = run(["redis-cli", "-h", args.redis_host, "-p", str(args.redis_port), "-n", str(args.redis_db),
                  "--scan", "--pattern", pattern])
    return result.stdout.splitlines()


def remote(args: argparse.Namespace, command: str) -> str:
    return run(["ssh", "-F", "/dev/null", args.target_host, command], timeout=30).stdout.strip()


def stop_target(args: argparse.Namespace) -> str:
    if args.target_adapter:
        return remote(args, f"{args.target_adapter} crash {args.target_node}").strip()
    config = f"/home/{os.environ.get('USER', 'alexeyp')}/.local/etc/xmq{args.target_port - 1882}/xmq_server.conf"
    script = '''
for pid in $(pgrep -x xmq_server); do
    arguments=$(ps -p "$pid" -o args= 2>/dev/null || true)
    case "$arguments" in
        *"-c CONFIG_PATH"*) kill -KILL "$pid" && echo "$pid" && exit 0 ;;
    esac
done
echo "no xmq_server found for CONFIG_PATH" >&2
exit 1
'''.replace("CONFIG_PATH", config)
    if args.target_host in {"localhost", "127.0.0.1", "thinker10"}:
        return run(["bash", "-lc", script]).stdout.strip()
    return remote(args, "bash -lc " + shlex.quote(script))


def restart_target(args: argparse.Namespace) -> str:
    if args.target_adapter:
        return remote(args, f"{args.target_adapter} start {args.target_node}").strip()
    command = f"~/.local/bin/xmq_start_instances {args.target_port - 1882}"
    if args.target_host in {"localhost", "127.0.0.1", "thinker10"}:
        return run(["bash", "-lc", command], timeout=45).stdout.strip()
    return remote(args, command)


def wait_for(predicate, description: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.25)
    raise TimeoutError(f"Timed out waiting for {description}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-host", default="thinker11", help="host of the broker instance to stop")
    parser.add_argument("--target-port", type=int, default=1883, choices=range(1883, 1887))
    parser.add_argument("--target-node", default="",
                        help="cluster node name in Redis (default: derived from host and port)")
    parser.add_argument("--target-adapter", default="",
                        help="remote command that takes 'crash <node>' and 'start <node>'; "
                             "for the AWS stand, ~/cluster/test_cluster.sh")
    parser.add_argument("--publisher-host", default="thinker10")
    parser.add_argument("--publisher-port", type=int, default=1883)
    parser.add_argument("--survivor-host", default="theater")
    parser.add_argument("--survivor-port", type=int, default=1883)
    parser.add_argument("--observer-host", default="thinker10")
    parser.add_argument("--observer-port", type=int, default=1884)
    parser.add_argument("--redis-host", default="theater")
    parser.add_argument("--redis-port", type=int, default=6379)
    parser.add_argument("--redis-db", type=int, default=8)
    parser.add_argument("--message-count", type=int, default=10000)
    parser.add_argument("--publish-rate", type=int, default=100,
                        help="messages per second; rate-limit the publisher during the outage")
    parser.add_argument("--receive-timeout", type=int, default=180,
                        help="seconds to wait for migrated messages before reporting undelivered messages")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()

    username = os.environ.get("XMQ_CLIENT_USER", "")
    password = os.environ.get("XMQ_CLIENT_PASSWORD", "")
    if not username or not password:
        parser.error("set XMQ_CLIENT_USER and XMQ_CLIENT_PASSWORD")
    if args.message_count <= 0:
        parser.error("--message-count must be positive")

    suffix = uuid.uuid4().hex[:12]
    instance_index = args.target_port - 1882
    node_name = args.target_node or (args.target_host if instance_index == 1
                                     else f"{args.target_host}-{instance_index}")
    node_id = redis(args, "HGET", "cluster:names", node_name)
    if not node_id or int(redis(args, "PTTL", f"cluster:alive:{node_id}") or "-2") <= 0:
        raise RuntimeError(f"Target {node_name} has no live Redis lease")

    output_dir = args.output_dir or ROOT / "LoadTest" / "results" / "raw" / (
        "Cluster-" + datetime.now().strftime("%Y-%m-%d-%H%M%S") + "-failover-migration"
    )
    output_dir.mkdir(parents=True, exist_ok=False)

    client_id = f"failover-migration-{suffix}"
    topic = f"codex/failover/{suffix}"
    payload_prefix = f"FM{suffix.upper()}-"
    expected = {f"{payload_prefix}{i:08d}" for i in range(args.message_count)}
    node_sessions = f"node_{node_name}_sessions"
    client_bin = str(XMQ_BIN)
    credentials = ["-u", username, "-P", password, "-V", "5", "-q", "1"]

    subscriber_log = output_dir / "subscriber-before-failure.log"
    publisher_log = output_dir / "publish-during-outage.log"
    observer_log = output_dir / "live-observer.log"
    recovery_log = output_dir / "subscriber-after-migration.log"
    sequence_file = output_dir / "expected-sequences.txt"
    sequence_file.write_text("\n".join(sorted(expected)) + "\n")

    sub_before = None
    sub_log_handle = None
    observer = None
    observer_log_handle = None
    target_stopped = False
    errors: list[str] = []
    sent = 0
    observer_payloads: list[str] = []
    received_payloads: list[str] = []
    try:
        # The same persistent MQTT client id must survive the broker and reconnect elsewhere.
        first_subscriber_command = [client_bin + "/xmq_sub", "-h", args.target_host, "-p", str(args.target_port),
                                    *credentials, "-i", client_id, "-t", topic, "-c", "-x", "3600", "-q", "1"]
        sub_log_handle = subscriber_log.open("w")
        sub_before = subprocess.Popen(first_subscriber_command, stdout=sub_log_handle,
                                      stderr=subprocess.STDOUT, text=True)
        wait_for(lambda: redis(args, "SISMEMBER", node_sessions, client_id) == "1",
                 f"persistent subscription on {node_name}", 15)
        print(f"Persistent subscriber {client_id} subscribed on {node_name}.")

        stopped_pid = stop_target(args)
        target_stopped = True
        print(f"Killed {node_name} process {stopped_pid}; waiting for its Redis lease to expire.")
        if sub_before.poll() is None:
            try:
                sub_before.wait(timeout=15)
            except subprocess.TimeoutExpired:
                sub_before.terminate()
                sub_before.wait(timeout=5)

        wait_for(lambda: int(redis(args, "PTTL", f"cluster:alive:{node_id}") or "-2") <= 0,
                 f"lease expiry for {node_name}", 45)
        time.sleep(2)

        # A second, clean subscriber on a live node shows whether the test publisher's full stream
        # reached the cluster while the persistent subscriber's owner was down.
        observer_command = [client_bin + "/xmq_sub", "-h", args.observer_host, "-p", str(args.observer_port),
                            *credentials, "-i", f"failover-observer-{suffix}", "-t", topic,
                            "-C", str(args.message_count), "-W", str(args.receive_timeout), "-v"]
        observer_log_handle = observer_log.open("w")
        observer = subprocess.Popen(observer_command, stdout=observer_log_handle,
                                    stderr=subprocess.STDOUT, text=True)
        wait_for(lambda: "Subscribe 1 clients" in observer_log.read_text(errors="replace"),
                 "the live observer's subscription", 15)

        publish_command = [client_bin + "/xmq_pub", "-h", args.publisher_host, "-p", str(args.publisher_port),
                           *credentials, "-i", f"failover-publisher-{suffix}", "-t", topic,
                           "--repeat", str(args.message_count), "--stdin", "--message-rate", str(args.publish_rate)]
        run(publish_command, input_text="\n".join(sorted(expected)) + "\n", timeout=180, output=publisher_log)
        sent = args.message_count
        print(f"Published {sent} unique QoS 1 messages at {args.publish_rate}/s while {node_name} was offline.")

        if observer is not None:
            try:
                observer.wait(timeout=args.receive_timeout + 15)
            except subprocess.TimeoutExpired:
                observer.terminate()
                observer.wait(timeout=5)
            observer_log_handle.close()
            observer_log_handle = None
            observer_text = observer_log.read_text(errors="replace")
            observer_payloads = re.findall(re.escape(payload_prefix) + r"\d{8}", observer_text)

        recovery_command = [client_bin + "/xmq_sub", "-h", args.survivor_host, "-p", str(args.survivor_port),
                            *credentials, "-i", client_id, "-t", topic, "-c", "-x", "3600", "-q", "1",
                            "-C", str(args.message_count), "-W", str(args.receive_timeout), "-v"]
        result = run(recovery_command, timeout=args.receive_timeout + 15, output=recovery_log)
        received_payloads = re.findall(re.escape(payload_prefix) + r"\d{8}", result.stdout + result.stderr)
    except Exception as exc:
        errors.append(str(exc))
    finally:
        if observer is not None and observer.poll() is None:
            observer.send_signal(signal.SIGTERM)
            try:
                observer.wait(timeout=5)
            except subprocess.TimeoutExpired:
                observer.kill()
                observer.wait(timeout=5)
        if observer_log_handle is not None:
            observer_log_handle.close()
        if sub_before is not None and sub_before.poll() is None:
            sub_before.send_signal(signal.SIGTERM)
            try:
                sub_before.wait(timeout=5)
            except subprocess.TimeoutExpired:
                sub_before.kill()
                sub_before.wait(timeout=5)
        if sub_log_handle is not None:
            sub_log_handle.close()
        if target_stopped:
            try:
                print(restart_target(args))
                target_stopped = False
            except Exception as exc:
                errors.append(f"failed to restart {node_name}: {exc}")
        # Remove only this run's session and client id from the session sets on every member.
        try:
            redis(args, "DEL", f"session_{client_id}", f"session_{client_id}_messages",
                  f"session_{client_id}_qos2", f"session_{client_id}_owner")
            for key in scan_redis(args, "node_*_sessions"):
                redis(args, "SREM", key, client_id)
        except Exception as exc:
            errors.append(f"Redis cleanup failed: {exc}")

    received = len(received_payloads)
    unique = set(received_payloads)
    missing = expected - unique
    observer_unique = set(observer_payloads)
    observer_missing = expected - observer_unique
    unexpected = unique - expected
    duplicates = received - len(unique)
    observer_duplicates = len(observer_payloads) - len(observer_unique)
    loss_pct = (100.0 * len(missing) / sent) if sent else 0.0
    observer_loss_pct = (100.0 * len(observer_missing) / sent) if sent else 0.0
    passed = sent and not missing and not unexpected and not observer_missing and not errors
    report = [
        "Cluster session migration result",
        f"Target broker: {node_name} ({args.target_host}:{args.target_port})",
        f"Recovery broker: {args.survivor_host}:{args.survivor_port}",
        f"Expected and published: {sent}",
        f"Live observer unique: {len(observer_unique)}",
        f"Live observer not received by deadline ({args.receive_timeout}s): {len(observer_missing)} ({observer_loss_pct:.4f}%)",
        f"Received payloads: {received}",
        f"Unique received: {len(unique)}",
        f"Not received by deadline ({args.receive_timeout}s): {len(missing)} ({loss_pct:.4f}%)",
        f"Duplicates: {duplicates}",
        f"Observer duplicates: {observer_duplicates}",
        f"Unexpected: {len(unexpected)}",
        f"Result: {'PASS' if passed else 'FAIL'}",
    ]
    if errors:
        report.extend(["Errors:", *errors])
    summary = "\n".join(report) + "\n"
    (output_dir / "summary.txt").write_text(summary)
    print(summary, end="")
    print(f"Detailed logs: {output_dir}")
    return 0 if passed else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
