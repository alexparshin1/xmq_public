#!/usr/bin/env python3
"""Measure how long N persistent sessions take to leave a failed or leaving cluster node.

Two questions, one run of each mode:

  crash  - the target node is killed with SIGKILL, as in a real failure. Its lease has to expire
           before another node may take its sessions; the coordinator then hands them out. The
           result is the time from the kill, and from lease expiry, until every session is owned
           by a live survivor.

  leave  - the target node is asked to leave its cluster through its own control API. A graceful
           leave removes the node and its lease at once. The result shows whether the cluster
           adopts its sessions by itself, or whether they stay offline until their clients
           reconnect; a sample of clients then reconnects to a survivor and times the claim.

The script uses the shared Redis as the source of truth: session_<client id>_owner names the node
that serves a session, and cluster:alive:<node id> says whether that node is running. It does not
need to reconnect every session to observe a migration, and it leaves the cluster in the state it
found it.

Local test cluster by default (three nodes on one host). Run it from a machine that can reach the
shared Redis and the MQTT ports, with the cluster's user account:

    XMQ_ADMIN_PASSWORD=... python3 LoadTest/Cluster/session_migration_time.py --mode both

The admin password is needed only for leave/join and is read from the environment, never from the
command line.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

REDIS_CLI = shutil.which("redis-cli") or "redis-cli"


def run(command: list[str], *, input_text: str | None = None, timeout: float = 60) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, input=input_text, text=True, capture_output=True, timeout=timeout)


class Redis:
    def __init__(self, host: str, port: int, db: int):
        self.host = host
        self.port = port
        self.db = db

    def command(self, *args: str) -> str:
        result = run([REDIS_CLI, "-h", self.host, "-p", str(self.port), "-n", str(self.db), "--raw", *args])
        if result.returncode:
            raise RuntimeError(f"redis-cli failed: {result.stderr.strip()}")
        return result.stdout.rstrip("\n")

    def values(self, keys: list[str]) -> list[str]:
        if not keys:
            return []
        result = run([REDIS_CLI, "-h", self.host, "-p", str(self.port), "-n", str(self.db),
                      "--raw", "MGET", *keys])
        if result.returncode:
            raise RuntimeError(f"redis-cli MGET failed: {result.stderr.strip()}")
        # One line per key; a nil is an empty line.
        lines = result.stdout.split("\n")
        if lines and lines[-1] == "":
            lines.pop()
        return lines

    def hgetall(self, key: str) -> dict[str, str]:
        lines = self.command("HGETALL", key).splitlines()
        return dict(zip(lines[0::2], lines[1::2]))

    def scan(self, pattern: str) -> list[str]:
        cursor = "0"
        found: list[str] = []
        while True:
            answer = run([REDIS_CLI, "-h", self.host, "-p", str(self.port), "-n", str(self.db),
                          "--raw", "SCAN", cursor, "MATCH", pattern, "COUNT", "1000"])
            if answer.returncode:
                raise RuntimeError(f"redis-cli SCAN failed: {answer.stderr.strip()}")
            lines = answer.stdout.splitlines()
            if not lines:
                break
            cursor = lines[0]
            found.extend(lines[1:])
            if cursor == "0":
                break
        return found

    def pipeline_delete(self, keys: list[str]) -> None:
        for start in range(0, len(keys), 500):
            batch = keys[start:start + 500]
            self.command("DEL", *batch)


@dataclass
class Phase:
    name: str
    started: float
    command_seconds: float | None = None
    lease_lost_seconds: float | None = None
    complete_seconds: float | None = None
    owned_by_target_after: int = 0
    unowned_after: int = 0
    other_after: int = 0
    per_session: dict[str, float] = field(default_factory=dict)
    reconnect_seconds: dict[str, float] = field(default_factory=dict)
    reconnect_offsets: dict[str, float] = field(default_factory=dict)

    def summary(self) -> dict:
        values = sorted(self.per_session.values())
        def quantile(q: float) -> float | None:
            if not values:
                return None
            index = min(len(values) - 1, max(0, round(q * (len(values) - 1))))
            return round(values[index], 3)
        reconnects = sorted(self.reconnect_seconds.values())
        reconnect_offsets = sorted(self.reconnect_offsets.values())
        return {
            "phase": self.name,
            "command_seconds": round(self.command_seconds, 3) if self.command_seconds is not None else None,
            "lease_lost_seconds": round(self.lease_lost_seconds, 3) if self.lease_lost_seconds is not None else None,
            "complete_seconds": round(self.complete_seconds, 3) if self.complete_seconds is not None else None,
            "owned_by_target_after": self.owned_by_target_after,
            "unowned_after": self.unowned_after,
            "other_after": self.other_after,
            "sessions": len(self.per_session),
            "first_session_seconds": round(values[0], 3) if values else None,
            "p50_seconds": quantile(0.50),
            "p95_seconds": quantile(0.95),
            "last_session_seconds": round(values[-1], 3) if values else None,
            "sample_reconnects": len(reconnects),
            "first_reconnect_seconds": round(reconnects[0], 3) if reconnects else None,
            "last_reconnect_seconds": round(reconnects[-1], 3) if reconnects else None,
            "first_reconnect_from_start_seconds": round(reconnect_offsets[0], 3) if reconnect_offsets else None,
            "last_reconnect_from_start_seconds": round(reconnect_offsets[-1], 3) if reconnect_offsets else None,
        }


class Probe:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.cluster_command = [os.path.expanduser(part) for part in args.cluster_command]
        self.redis = Redis(args.redis_host, args.redis_port, args.redis_db)
        self.prefix = args.prefix or f"session-migration-{int(time.time())}"
        self.ids = [f"{self.prefix}-{index}" for index in range(1, args.sessions + 1)]
        self.target_id = ""
        self.node_names: dict[str, str] = {}
        self.subscriber: subprocess.Popen[str] | None = None
        self.subscriber_log: Path | None = None
        self.sessions_started = False
        self.cleaned = True
        self.results: dict[str, object] = {
            "target": args.target,
            "survivor": args.survivor,
            "sessions": args.sessions,
            "prefix": self.prefix,
            "redis": f"{args.redis_host}:{args.redis_port}/{args.redis_db}",
            "phases": [],
        }

    def cluster_names(self) -> dict[str, str]:
        return self.redis.hgetall("cluster:names")

    def alive(self, node_id: str) -> bool:
        return self.redis.command("EXISTS", f"cluster:alive:{node_id}") == "1"

    def owners(self) -> dict[str, str]:
        keys = [f"session_{client_id}_owner" for client_id in self.ids]
        values = self.redis.values(keys)
        if len(values) < len(keys):
            values += [""] * (len(keys) - len(values))
        return {client_id: (values[index] if index < len(values) else "")
                for index, client_id in enumerate(self.ids)}

    def wait_until(self, predicate, description: str, timeout: float, interval: float = 0.05) -> float:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return time.monotonic()
            time.sleep(interval)
        raise TimeoutError(f"Timed out after {timeout:.0f}s waiting for {description}")

    def start_sessions(self) -> None:
        xmq_sub = shutil.which("xmq_sub")
        if not xmq_sub:
            raise RuntimeError("xmq_sub is not on PATH")
        self.subscriber_log = self.args.output_dir / "subscribers.log"
        command = [xmq_sub, "-h", self.args.target_host, "-p", str(self.args.target_port),
                   "-u", self.args.username, "-P", self.args.password, "-V", "5", "-q", "1",
                   "-c", "-i", self.prefix, "-n", str(self.args.sessions),
                   "-t", self.args.topic, "-W", "3600", "--quiet"]
        self.subscriber_log.write_text(" ".join(command) + "\n")
        handle = self.subscriber_log.open("a")
        self.subscriber = subprocess.Popen(command, stdout=handle, stderr=subprocess.STDOUT, text=True)
        target_set = f"node_{self.args.target}_sessions"
        def target_has_sessions() -> bool:
            value = self.redis.command("SCARD", target_set)
            return value.isdigit() and int(value) >= self.args.sessions
        self.wait_until(target_has_sessions,
                        f"{self.args.sessions} sessions in {target_set}", self.args.setup_timeout)
        self.wait_until(lambda: sum(1 for owner in self.owners().values() if owner == self.target_id) == self.args.sessions,
                        "all sessions owned by the target node", self.args.setup_timeout)
        self.sessions_started = True
        self.cleaned = False
        print(f"{self.args.sessions} persistent sessions are on {self.args.target}.")

    def stop_sessions(self) -> None:
        if self.subscriber and self.subscriber.poll() is None:
            self.subscriber.terminate()
            try:
                self.subscriber.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.subscriber.kill()
                self.subscriber.wait(timeout=5)

    def observe(self, phase: Phase, timeout: float) -> None:
        """Poll until every session is owned by a live node other than the target."""
        started = phase.started
        while True:
            snapshot = self.owners()
            live = {node_id for node_id in self.node_names.values() if self.alive(node_id)}
            now = time.monotonic()
            for client_id, owner in snapshot.items():
                if client_id not in phase.per_session and owner in live and owner != self.target_id:
                    phase.per_session[client_id] = now - started
            if len(phase.per_session) == len(self.ids):
                phase.complete_seconds = now - started
                return
            if now - started > timeout:
                final = self.owners()
                phase.owned_by_target_after = sum(1 for owner in final.values() if owner == self.target_id)
                phase.unowned_after = sum(1 for owner in final.values() if not owner)
                phase.other_after = sum(1 for owner in final.values() if owner and owner not in live)
                return
            time.sleep(0.05)

    def reconnect_sample(self, phase: Phase, count: int) -> None:
        """Connect clean copies of sample client ids to a survivor and time the claim.

        A clean CONNECT with a client id of a persistent session takes the session over and then
        ends it, which both proves the session is reachable and cleans it up. It is not part of the
        bulk migration measurement: it is the client coming back.
        """
        xmq_sub = shutil.which("xmq_sub")
        if not xmq_sub:
            raise RuntimeError("xmq_sub is not on PATH")
        sample = self.ids[:count]
        processes = []
        started = time.monotonic()
        for client_id in sample:
            command = [xmq_sub, "-h", self.args.survivor_host, "-p", str(self.args.survivor_port),
                       "-u", self.args.username, "-P", self.args.password, "-V", "5", "-q", "1",
                       "-i", client_id, "-t", self.args.topic, "-W", "1"]
            processes.append((client_id, started, subprocess.Popen(command, stdout=subprocess.PIPE,
                                                                   stderr=subprocess.STDOUT, text=True)))
        for client_id, launched, process in processes:
            try:
                output, _ = process.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                output, _ = process.communicate()
            connect_line = re.search(r"^Connect 1 clients:.*$", output or "", re.MULTILINE)
            if connect_line and not re.search(r"failed [1-9]", connect_line.group(0)):
                elapsed = re.search(r"Elapsed ([\d.]+)(ms|sec)", connect_line.group(0))
                connect_seconds = 0.0
                if elapsed:
                    connect_seconds = float(elapsed.group(1)) * (0.001 if elapsed.group(2) == "ms" else 1.0)
                phase.reconnect_seconds[client_id] = connect_seconds
                phase.reconnect_offsets[client_id] = (launched - phase.started) + connect_seconds
        print(f"Reconnect sample: {len(phase.reconnect_seconds)}/{len(sample)} claimed by {self.args.survivor}.")

    def wait_live(self, node_id: str, timeout: float = 60) -> float:
        started = time.monotonic()
        self.wait_until(lambda: self.alive(node_id), f"{self.args.target} lease", timeout, interval=0.02)
        return time.monotonic() - started

    def wait_lease_gone(self, node_id: str, timeout: float = 60) -> float:
        started = time.monotonic()
        self.wait_until(lambda: not self.alive(node_id), f"{self.args.target} lease to expire",
                        timeout, interval=0.02)
        return time.monotonic() - started

    def crash(self) -> Phase:
        phase = Phase("crash", time.monotonic())
        result = run(self.cluster_command + ["crash", self.args.target], timeout=60)
        phase.command_seconds = time.monotonic() - phase.started
        if result.returncode:
            raise RuntimeError(f"crash command failed: {result.stderr.strip()}")
        # The lease is what keeps another node from taking the sessions over.
        self.wait_lease_gone(self.target_id, timeout=self.args.lease_timeout)
        phase.lease_lost_seconds = time.monotonic() - phase.started
        print(f"{self.args.target} lease gone {phase.lease_lost_seconds:.3f}s after the kill.")
        self.observe(phase, self.args.migration_timeout)
        print(f"Adoption complete {phase.complete_seconds:.3f}s after the kill "
              f"({len(phase.per_session)}/{len(self.ids)} sessions)." if phase.complete_seconds is not None
              else f"Adoption incomplete after {self.args.migration_timeout}s.")
        return phase

    def leave(self) -> Phase:
        password = os.environ.get("XMQ_ADMIN_PASSWORD", "")
        if not password:
            raise RuntimeError("XMQ_ADMIN_PASSWORD is required for --mode leave")
        phase = Phase("leave", time.monotonic())
        command = [self.args.server_binary, "-c", self.args.target_config, "--leave-cluster",
                   "--admin-user", self.args.admin_user]
        result = run(command, input_text=password + "\n", timeout=60)
        phase.command_seconds = time.monotonic() - phase.started
        if result.returncode:
            raise RuntimeError(f"leave-cluster failed: {result.stdout.strip()}{result.stderr.strip()}")
        print(f"{self.args.target} left the cluster in {phase.command_seconds:.3f}s.")
        self.observe(phase, self.args.leave_adopt_timeout)
        if phase.complete_seconds is not None:
            print(f"All sessions were adopted {phase.complete_seconds:.3f}s after the leave started.")
        else:
            print(f"After {self.args.leave_adopt_timeout}s, {phase.owned_by_target_after} session(s) "
                  f"still name the node that left and {phase.unowned_after} have no owner; "
                  f"the cluster did not adopt them by itself.")
            self.reconnect_sample(phase, min(self.args.sample_reconnects, len(self.ids)))
        return phase

    def restart_target(self) -> None:
        result = run(self.cluster_command + ["start", self.args.target], timeout=90)
        if result.returncode:
            raise RuntimeError(f"start command failed: {result.stderr.strip()}")
        self.wait_until(lambda: self.alive(self.target_id), f"{self.args.target} lease after restart", 60)

    def rejoin_target(self) -> None:
        password = os.environ.get("XMQ_ADMIN_PASSWORD", "")
        if not password:
            raise RuntimeError("XMQ_ADMIN_PASSWORD is required to rejoin the target")
        command = [self.args.server_binary, "-c", self.args.target_config, "--join-cluster",
                   "--admin-user", self.args.admin_user]
        result = run(command, input_text=password + "\n", timeout=90)
        if result.returncode:
            # The test cluster writes configurations directly; its xmq_node rows may not carry the
            # UUID the running node has, and the CLI join checks that row. Enable the cluster in the
            # configuration and restart instead, which is how the test cluster joins its nodes.
            print(f"CLI join failed ({result.stdout.strip()}{result.stderr.strip()}); "
                  f"enabling cluster mode in {self.args.target_config} and restarting.")
            path = Path(self.args.target_config)
            data = json.loads(path.read_text())
            data.setdefault("cluster", {})["enabled"] = True
            path.write_text(json.dumps(data, indent=4) + "\n")
            stopped = run(self.cluster_command + ["stop", self.args.target], timeout=60)
            if stopped.returncode:
                raise RuntimeError(f"stop command failed: {stopped.stderr.strip()}")
            self.wait_until(lambda: not self.alive(self.target_id), f"{self.args.target} lease after stop", 30)
            result = run(self.cluster_command + ["start", self.args.target], timeout=90)
            if result.returncode:
                raise RuntimeError(f"start command failed: {result.stderr.strip()}")
        self.wait_until(lambda: self.alive(self.target_id), f"{self.args.target} lease after rejoin", 60)

    def cleanup(self) -> None:
        """End the test sessions that are still in the cluster, and remove their records.

        A clean CONNECT with the same client id is a takeover: it removes the persistent session
        and its owner marker wherever the cluster holds them. One utility process does all of them
        through the cluster, after which any leftover keys are deleted directly.
        """
        xmq_sub = shutil.which("xmq_sub")
        if xmq_sub and self.args.sessions:
            command = [xmq_sub, "-h", self.args.survivor_host, "-p", str(self.args.survivor_port),
                       "-u", self.args.username, "-P", self.args.password, "-V", "5", "-q", "1",
                       "-i", self.prefix, "-n", str(self.args.sessions), "-t", self.args.topic,
                       "-W", "2", "--quiet"]
            try:
                run(command, timeout=60)
            except Exception as error:
                print(f"Clean-session cleanup did not finish: {error}", file=sys.stderr)
        keys = []
        for client_id in self.ids:
            keys.extend([f"session_{client_id}", f"session_{client_id}_messages",
                         f"session_{client_id}_qos2", f"session_{client_id}_owner"])
        try:
            self.redis.pipeline_delete(keys)
            for key in self.redis.scan("node_*_sessions"):
                for start in range(0, len(self.ids), 500):
                    self.redis.command("SREM", key, *self.ids[start:start + 500])
        except Exception as error:
            print(f"Redis cleanup failed: {error}", file=sys.stderr)
        self.cleaned = True

    def run(self) -> int:
        self.args.output_dir.mkdir(parents=True, exist_ok=False)
        try:
            self.node_names = self.cluster_names()
            if self.args.target not in self.node_names:
                raise RuntimeError(f"{self.args.target} is not in cluster:names: {self.node_names}")
            self.target_id = self.node_names[self.args.target]
            if not self.alive(self.target_id):
                raise RuntimeError(f"{self.args.target} has no live lease")
            print(f"Target {self.args.target} = {self.target_id}; "
                  f"live nodes: {[name for name, node_id in self.node_names.items() if self.alive(node_id)]}")

            self.start_sessions()
            if self.args.mode in {"crash", "both"}:
                phase = self.crash()
                self.results["phases"].append(phase.summary())
                self.cleanup()
                self.restart_target()
            if self.args.mode in {"leave", "both"}:
                phase = self.leave()
                self.results["phases"].append(phase.summary())
                self.cleanup()
                self.rejoin_target()
            self.results["result"] = "PASS"
            return 0
        except Exception as error:
            self.results["result"] = "FAIL"
            self.results["error"] = str(error)
            print(error, file=sys.stderr)
            return 1
        finally:
            self.stop_sessions()
            if self.sessions_started and not self.cleaned:
                self.cleanup()
            summary = self.results
            (self.args.output_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
            lines = [f"Session migration timing - {self.args.mode}", f"Target: {self.args.target}",
                     f"Sessions: {self.args.sessions}", f"Result: {summary.get('result')}"]
            def show(value):
                return "not reached" if value is None else f"{value:.3f}s"

            for phase in summary.get("phases", []):
                lines.append("")
                lines.append(f"{phase['phase']}:")
                lines.append(f"  command: {show(phase['command_seconds'])}")
                lines.append(f"  lease lost: {show(phase['lease_lost_seconds'])}")
                lines.append(f"  complete: {show(phase['complete_seconds'])}")
                lines.append(f"  sessions migrated: {phase['sessions']}")
                lines.append(f"  first / p50 / p95 / last: {show(phase['first_session_seconds'])} / "
                             f"{show(phase['p50_seconds'])} / {show(phase['p95_seconds'])} / {show(phase['last_session_seconds'])}")
                lines.append(f"  still owned by target: {phase['owned_by_target_after']}")
                lines.append(f"  unowned: {phase['unowned_after']}; owned by another non-live node: {phase['other_after']}")
                if phase["sample_reconnects"]:
                    lines.append(f"  sample reconnects: {phase['sample_reconnects']} "
                                 f"(connect latency {show(phase['first_reconnect_seconds'])}..{show(phase['last_reconnect_seconds'])}, "
                                 f"from leave {show(phase['first_reconnect_from_start_seconds'])}..{show(phase['last_reconnect_from_start_seconds'])})")
                else:
                    lines.append("  sample reconnects: none")
            (self.args.output_dir / "summary.txt").write_text("\n".join(lines) + "\n")
            print("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mode", choices=["crash", "leave", "both"], default="both")
    parser.add_argument("--sessions", type=int, default=1000)
    parser.add_argument("--prefix", default="")
    parser.add_argument("--topic", default="cluster/session-migration")
    parser.add_argument("--target", default="node3")
    parser.add_argument("--target-host", default="thinker10")
    parser.add_argument("--target-port", type=int, default=2886)
    parser.add_argument("--survivor", default="node1")
    parser.add_argument("--survivor-host", default="thinker10")
    parser.add_argument("--survivor-port", type=int, default=2884)
    parser.add_argument("--username", default="user")
    parser.add_argument("--password", default=os.environ.get("XMQ_CLIENT_PASSWORD", "User-Secret1"))
    parser.add_argument("--redis-host", default="theater")
    parser.add_argument("--redis-port", type=int, default=6379)
    parser.add_argument("--redis-db", type=int, default=4)
    parser.add_argument("--cluster-command", nargs="+", default=["~/cluster/test_cluster.sh"],
                        help="command taking 'crash <node>' and 'start <node>'")
    parser.add_argument("--target-config", default=os.path.expanduser("~/cluster.run/node3/xmq_server.conf"))
    parser.add_argument("--server-binary", default="xmq_server")
    parser.add_argument("--admin-user", default="admin")
    parser.add_argument("--setup-timeout", type=float, default=120)
    parser.add_argument("--lease-timeout", type=float, default=60)
    parser.add_argument("--migration-timeout", type=float, default=120)
    parser.add_argument("--leave-adopt-timeout", type=float, default=15)
    parser.add_argument("--sample-reconnects", type=int, default=20)
    parser.add_argument("--output-dir", type=Path,
                        default=Path.home() / "session-migration-results" / time.strftime("%Y-%m-%d-%H%M%S"))
    args = parser.parse_args()
    if args.sessions <= 0:
        parser.error("--sessions must be positive")
    return Probe(args).run()


if __name__ == "__main__":
    raise SystemExit(main())
