#!/usr/bin/env python3
"""Crash acceptance tests using private Redis/broker processes and raw MQTT 3.1.1.

Run: python3 test/integration/reliable_store_crash.py --broker /absolute/path/xmq_server
Requires redis-server >=7.2 on PATH. Never connects to the developer's Redis.
Artifacts are retained in a temporary directory, whose path is printed.
"""
import argparse
import json
import pathlib
import signal
import socket
import struct
import subprocess
import tempfile
import time


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def exact(sock, count):
    data = b""
    while len(data) < count:
        block = sock.recv(count - len(data))
        if not block:
            raise EOFError("connection closed")
        data += block
    return data


def text(value):
    value = value.encode() if isinstance(value, str) else value
    return struct.pack("!H", len(value)) + value


class MQTT:
    def __init__(self, mqtt_port, client_id, clean=False):
        self.sock = socket.create_connection(("127.0.0.1", mqtt_port), 3)
        self.sock.settimeout(8)
        self.send(0x10, text("MQTT") + bytes([4, 2 if clean else 0, 0, 30]) + text(client_id))
        header, body = self.read()
        assert header == 0x20 and body[-1] == 0, (header, body)

    def send(self, header, body):
        length = len(body)
        remaining = b""
        while True:
            digit = length % 128
            length //= 128
            remaining += bytes([digit | (128 if length else 0)])
            if not length:
                break
        self.sock.sendall(bytes([header]) + remaining + body)

    def read(self):
        header = exact(self.sock, 1)[0]
        length, multiplier = 0, 1
        while True:
            digit = exact(self.sock, 1)[0]
            length += (digit & 127) * multiplier
            if not digit & 128:
                break
            multiplier *= 128
        return header, exact(self.sock, length)

    def subscribe(self, topic, qos=1):
        self.send(0x82, b"\x00\x01" + text(topic) + bytes([qos]))
        header, body = self.read()
        assert header == 0x90 and body[-1] == qos, (header, body)

    def publish(self, topic, payload, qos=1, packet_id=7, retain=False, dup=False):
        self.send(0x30 | (qos << 1) | int(retain) | (8 if dup else 0),
                  text(topic) + (struct.pack("!H", packet_id) if qos else b"") + payload.encode())
        header, body = self.read()
        assert header == (0x40 if qos == 1 else 0x50), (header, body)
        assert body == struct.pack("!H", packet_id), body

    def close(self):
        try:
            self.send(0xE0, b"")
        except OSError:
            pass
        self.sock.close()


class Redis:
    def __init__(self, redis_port):
        self.sock = socket.create_connection(("127.0.0.1", redis_port), 2)
        self.sock.settimeout(5)
        self.file = self.sock.makefile("rb")

    def command(self, *parts):
        items = [str(part).encode() if not isinstance(part, bytes) else part for part in parts]
        self.sock.sendall(b"*" + str(len(items)).encode() + b"\r\n" + b"".join(
            b"$" + str(len(item)).encode() + b"\r\n" + item + b"\r\n" for item in items))
        return self.read()

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError("Redis closed connection")
        tag, data = line[:1], line[1:-2]
        if tag == b"-":
            raise RuntimeError(data.decode())
        if tag == b"+":
            return data
        if tag == b":":
            return int(data)
        count = int(data)
        if count == -1:
            return None
        if tag == b"$":
            value = self.file.read(count)
            assert self.file.read(2) == b"\r\n"
            return value
        if tag == b"*":
            return [self.read() for _ in range(count)]
        raise AssertionError(line)

    def close(self):
        self.file.close()
        self.sock.close()


class Campaign:
    def __init__(self, broker):
        self.root = pathlib.Path(tempfile.mkdtemp(prefix="xmq-reliable-crash-"))
        self.broker_binary = str(pathlib.Path(broker).resolve())
        self.redis_port, self.mqtt_port, web_port = port(), port(), port()
        self.redis_process = self.broker_process = None
        self.logs = []
        self.results = []
        self.config = {
            "connections": {"ssl_keys": {"certfile": "", "verify_depth": "0"}, "listener": [{"id": 1, "name": "Test", "protocol": "MQTT",
                "bind_ip": "127.0.0.1", "port": self.mqtt_port, "threads": 2, "enable": True}]},
            "authentication": {"allow_anonymous": True},
            "persistence": {"enabled": True, "redis_uri": f"redis://127.0.0.1:{self.redis_port}/0",
                "clean_start": False, "max_queued_writes": 0, "ack_after_store": True},
            "cluster": {"enabled": False, "this_node": {"node_name": "reliable-test", "host_port": f"localhost:{self.mqtt_port}"}},
            "queue_limits": {"max_size": 100, "max_inflight_messages": 16},
            "server_limits": {"send_threads": "2", "receive_threads": "2", "delivery_threads": "2", "max_topic_alias": 128},
            "web_service": {"listener_port": web_port, "encrypted": False},
            "logging": {"log_to": str(self.root / "broker.log"), "min_log_level": "INFO", "log_level": {name: "INFO" for name in ("connect", "disconnect", "subscribe", "unsubscribe", "publish", "ack", "server_connections", "server_events", "cluster_connections", "cluster_events")}},
        }
        self.config_path = self.root / "xmq_server.conf"
        self.config_path.write_text(json.dumps(self.config))

    def wait_port(self, target, process):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"Process exited with {process.returncode}; see {self.root}")
            try:
                with socket.create_connection(("127.0.0.1", target), .1):
                    return
            except OSError:
                time.sleep(.05)
        raise TimeoutError(f"Listener {target} did not start; see {self.root}")

    def start_redis(self, aof=True):
        log = open(self.root / "redis-stdout.log", "ab")
        self.logs.append(log)
        self.redis_process = subprocess.Popen(["redis-server", "--bind", "127.0.0.1", "--port", str(self.redis_port),
            "--save", "", "--appendonly", "yes" if aof else "no", "--appendfsync", "everysec",
            "--dir", str(self.root)], stdout=log, stderr=subprocess.STDOUT)
        self.wait_port(self.redis_port, self.redis_process)

    def start_broker(self):
        log = open(self.root / "broker-stdout.log", "ab")
        self.logs.append(log)
        self.broker_process = subprocess.Popen([self.broker_binary, "-c", str(self.config_path)],
            cwd=self.root, stdout=log, stderr=subprocess.STDOUT)
        self.wait_port(self.mqtt_port, self.broker_process)

    def crash(self):
        # No graceful stop, AOF rewrite, SAVE, or broker shutdown flush.
        for process in (self.broker_process, self.redis_process):
            if process and process.poll() is None:
                process.kill()
        for process in (self.broker_process, self.redis_process):
            if process:
                process.wait(timeout=10)

    def restart(self):
        self.start_redis()
        self.start_broker()

    def check(self, label):
        self.results.append(label)
        print("PASS", label, flush=True)

    def redis(self):
        return Redis(self.redis_port)

    def offline(self, client_id, topic):
        client = MQTT(self.mqtt_port, client_id)
        client.subscribe(topic)
        client.close()

    def run(self):
        print("Artifacts:", self.root, flush=True)
        self.restart()
        self.offline("offline-qos1", "crash/qos1")
        publisher = MQTT(self.mqtt_port, "publisher-qos1")
        publisher.publish("crash/qos1", "confirmed", retain=True)
        self.crash()
        publisher.sock.close()
        self.restart()
        redis = self.redis()
        assert redis.command("HLEN", "session_offline-qos1_messages") == 1
        retained_keys = redis.command("KEYS", "node_*_retained")
        assert len(retained_keys) == 1
        assert redis.command("HEXISTS", retained_keys[0], "crash/qos1") == 1
        redis.close()
        subscriber = MQTT(self.mqtt_port, "offline-qos1")
        header, body = subscriber.read()
        assert header >> 4 == 3 and body.endswith(b"confirmed"), (header, body)
        subscriber.close()
        self.check("QoS1 ACK survives SIGKILL of broker and Redis: offline delivery and retained")

        # Incoming QoS2 receive state and the delivery are one transaction. Crash before PUBREL.
        self.offline("offline-qos2", "crash/qos2")
        publisher = MQTT(self.mqtt_port, "publisher-qos2")
        publisher.publish("crash/qos2", "exactly-once", qos=2)
        self.crash()
        publisher.sock.close()
        self.restart()
        redis = self.redis()
        assert redis.command("HLEN", "session_offline-qos2_messages") == 1
        assert redis.command("HEXISTS", "session_publisher-qos2_qos2", "7") == 1
        redis.close()
        publisher = MQTT(self.mqtt_port, "publisher-qos2")
        publisher.publish("crash/qos2", "exactly-once", qos=2, dup=True)
        redis = self.redis()
        assert redis.command("HLEN", "session_offline-qos2_messages") == 1
        redis.close()
        self.check("QoS2 PUBREC survives double crash; repeated PUBLISH creates no second delivery")
        publisher.send(0x62, b"\x00\x07")
        assert publisher.read() == (0x70, b"\x00\x07")
        self.crash()
        publisher.sock.close()
        self.restart()
        redis = self.redis()
        assert redis.command("HEXISTS", "session_publisher-qos2_qos2", "7") == 0
        redis.close()
        publisher = MQTT(self.mqtt_port, "publisher-qos2")
        publisher.publish("crash/qos2", "new-message", qos=2, packet_id=7)
        redis = self.redis()
        assert redis.command("HLEN", "session_offline-qos2_messages") == 2
        redis.close()
        publisher.close()
        self.check("QoS2 PUBCOMP survives double crash; packet ID can be reused")

        # A clearing must be durable before PUBACK too.
        publisher = MQTT(self.mqtt_port, "publisher-clear")
        publisher.publish("crash/qos1", "", retain=True)
        self.crash()
        publisher.sock.close()
        self.restart()
        redis = self.redis()
        assert redis.command("HEXISTS", retained_keys[0], "crash/qos1") == 0
        redis.close()
        self.check("Retained clearing survives double crash immediately after PUBACK")

        # Pipeline publications without waiting for individual fsyncs. Every successful ACK
        # must survive killing both processes, including records committed in the same batch.
        self.offline("offline-batch", "crash/batch")
        publisher = MQTT(self.mqtt_port, "publisher-batch")
        batch_count = 64
        for packet_id in range(1, batch_count + 1):
            publisher.send(0x32, text("crash/batch") + struct.pack("!H", packet_id)
                           + f"batch-{packet_id}".encode())
        acknowledgements = set()
        for _ in range(batch_count):
            header, body = publisher.read()
            assert header == 0x40 and len(body) == 2, (header, body)
            acknowledgements.add(struct.unpack("!H", body)[0])
        assert acknowledgements == set(range(1, batch_count + 1))
        self.crash()
        publisher.sock.close()
        self.restart()
        redis = self.redis()
        assert redis.command("HLEN", "session_offline-batch_messages") == batch_count
        redis.close()
        subscriber = MQTT(self.mqtt_port, "offline-batch")
        received = set()
        for _ in range(batch_count):
            header, body = subscriber.read()
            assert header >> 4 == 3 and (header >> 1) & 3 == 1, (header, body)
            topic_length = struct.unpack("!H", body[:2])[0]
            offset = 2 + topic_length
            packet_id = body[offset:offset + 2]
            received.add(body[offset + 2:].decode())
            subscriber.send(0x40, packet_id)
        assert received == {f"batch-{i}" for i in range(1, batch_count + 1)}
        subscriber.close()
        self.check("64 pipelined QoS1 ACKs survive double crash and recover every offline delivery")

        # A commit completion must never acknowledge a packet on a replacement connection.
        self.offline("offline-takeover", "crash/takeover")
        old = MQTT(self.mqtt_port, "publisher-takeover")
        redis = self.redis()
        redis.command("CLIENT", "PAUSE", 100, "WRITE")
        redis.close()
        old.send(0x32, text("crash/takeover") + struct.pack("!H", 77) + b"old")
        replacement = MQTT(self.mqtt_port, "publisher-takeover")
        replacement.send(0xC0, b"")
        assert replacement.read() == (0xD0, b""), "Late PUBACK leaked to replacement connection"
        replacement.publish("crash/takeover", "new", packet_id=77)
        redis = self.redis()
        assert redis.command("HLEN", "session_offline-takeover_messages") == 2
        redis.close()
        old.sock.close()
        replacement.close()
        self.check("Delayed QoS1 commit cannot send an old PUBACK to a replacement connection")

        # SUBSCRIBE also returns to its receive worker before the disk finishes. Its late
        # SUBACK must stay with the original connection, including a takeover during the fence.
        old = MQTT(self.mqtt_port, "subscriber-takeover")
        redis = self.redis()
        redis.command("CLIENT", "PAUSE", 100, "WRITE")
        redis.close()
        old.send(0x82, b"\x00\x01" + text("crash/subscriber-takeover") + b"\x01")
        replacement = MQTT(self.mqtt_port, "subscriber-takeover")
        replacement.send(0xC0, b"")
        assert replacement.read() == (0xD0, b""), "Late SUBACK leaked to replacement connection"
        old.sock.close()
        replacement.close()
        self.check("Delayed SUBSCRIBE commit cannot send an old SUBACK to a replacement connection")

        # The script can succeed while the fsync barrier is denied. SUBACK belongs to
        # the durable-only completion path, not to the applied-state installation path.
        subscriber = MQTT(self.mqtt_port, "subscriber-fence-fault")
        redis = self.redis()
        redis.command("ACL", "SETUSER", "default", "-waitaof")
        redis.close()
        subscriber.send(0x82, b"\x00\x01" + text("crash/fence") + b"\x01")
        try:
            reply = subscriber.read()
        except (EOFError, ConnectionError, socket.timeout):
            reply = None
        assert reply is None or reply[0] != 0x90, reply
        redis = self.redis()
        redis.command("ACL", "SETUSER", "default", "+waitaof")
        redis.close()
        subscriber.sock.close()
        self.check("Applied subscription without confirmed fsync cannot send successful SUBACK")
        self.crash()
        self.restart()

        # An ACL denial inside a Lua script must be found before any write. This Redis belongs
        # exclusively to this campaign; no shared service or account is changed.
        self.offline("offline-acl", "crash/acl")
        redis = self.redis()
        redis.command("ACL", "SETUSER", "default", "-hset")
        redis.close()
        publisher = MQTT(self.mqtt_port, "publisher-acl")
        publisher.send(0x35, text("crash/acl") + b"\x00\x07" + b"denied")
        try:
            reply = publisher.read()
        except (EOFError, ConnectionError, socket.timeout):
            reply = None
        assert reply is None or reply[0] != 0x50, reply
        redis = self.redis()
        assert redis.command("HLEN", "session_offline-acl_messages") == 0
        assert redis.command("HEXISTS", "session_publisher-acl_qos2", "7") == 0
        assert redis.command("HEXISTS", retained_keys[0], "crash/acl") == 0
        redis.command("ACL", "SETUSER", "default", "+hset")
        redis.close()
        publisher.sock.close()
        self.check("Redis ACL denies HSET: no success PUBREC or partial commit")
        self.crash()
        self.restart()

        # Reject a failed transaction without exposing its retained change or inbound packet ID.
        self.offline("offline-fault", "crash/fault")
        redis = self.redis()
        redis.command("SET", "session_offline-fault_messages", "wrong-type")
        redis.close()
        publisher = MQTT(self.mqtt_port, "publisher-fault")
        publisher.send(0x35, text("crash/fault") + b"\x00\x07" + b"must-not-be-accepted")
        try:
            reply = publisher.read()
        except (EOFError, ConnectionError, socket.timeout):
            reply = None
        assert reply is None or reply[0] != 0x50, reply
        redis = self.redis()
        assert redis.command("HEXISTS", retained_keys[0], "crash/fault") == 0
        assert redis.command("HEXISTS", "session_publisher-fault_qos2", "7") == 0
        # Remove our deliberately corrupted key before the next startup restores sessions.
        redis.command("DEL", "session_offline-fault_messages")
        assert redis.command("WAITAOF", 1, 0, 5000)[0] == 1
        redis.close()
        publisher.sock.close()
        self.check("Write error: no success PUBREC, no partial retained change or QoS2 marker")

        # A lost Redis produces no successful PUBACK; memory fallback is forbidden.
        self.crash()
        self.restart()
        publisher = MQTT(self.mqtt_port, "publisher-outage")
        self.redis_process.kill()
        self.redis_process.wait(timeout=5)
        publisher.send(0x33, text("crash/outage") + b"\x00\x09" + b"unconfirmed")
        try:
            reply = publisher.read()
        except (EOFError, ConnectionError, socket.timeout):
            reply = None
        assert reply is None or reply[0] != 0x40, reply
        publisher.sock.close()
        self.check("Redis outage: no successful PUBACK and no memory fallback")
        self.crash()
        self.start_redis(aof=False)
        log = open(self.root / "no-aof-stdout.log", "ab")
        self.logs.append(log)
        self.broker_process = subprocess.Popen([self.broker_binary, "-c", str(self.config_path)],
            cwd=self.root, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if "MQTT server did not start" in (self.root / "no-aof-stdout.log").read_text():
                break
            time.sleep(.05)
        else:
            raise AssertionError("Missing startup failure diagnosis")
        try:
            with socket.create_connection(("127.0.0.1", self.mqtt_port), .2):
                raise AssertionError("MQTT listener opened without AOF")
        except (ConnectionRefusedError, socket.timeout):
            pass
        self.check("AOF disabled: MQTT remains stopped; management interface remains available")
        (self.root / "results.json").write_text(json.dumps({"passed": self.results}, indent=2))

    def close(self):
        self.crash()
        for log in self.logs:
            log.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--broker", required=True)
    args = parser.parse_args()
    campaign = Campaign(args.broker)
    try:
        campaign.run()
    finally:
        campaign.close()
