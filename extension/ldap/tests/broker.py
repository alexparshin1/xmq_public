"""Full MQTT 5 acceptance checks against an isolated broker and accounts database."""
import concurrent.futures
import json
import os
import pathlib
import socket
import sqlite3
import struct
import subprocess
import time


def string(value):
    data = value.encode()
    return struct.pack("!H", len(data)) + data


def send(sock, header, payload):
    length = len(payload)
    encoded = bytearray()
    while True:
        digit = length % 128
        length //= 128
        encoded.append(digit | (128 if length else 0))
        if not length:
            break
    sock.sendall(bytes([header]) + encoded + payload)


def receive(sock):
    def exact(count):
        data = b""
        while len(data) < count:
            part = sock.recv(count - len(data))
            if not part:
                raise RuntimeError("broker closed MQTT connection")
            data += part
        return data
    header = exact(1)[0]
    length, multiplier = 0, 1
    for _ in range(4):
        digit = exact(1)[0]
        length += (digit & 127) * multiplier
        if not digit & 128:
            return header, exact(length)
        multiplier *= 128
    raise RuntimeError("invalid MQTT remaining length")


def connect(port, username, password, expected=0):
    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    send(sock, 0x10, string("MQTT") + bytes([5, 0xC2, 0, 30, 0]) +
         string("ldap-test-" + str(time.monotonic_ns())) + string(username) + string(password))
    header, answer = receive(sock)
    accepted = expected if isinstance(expected, tuple) else (expected,)
    assert header == 0x20 and answer[1] in accepted, ("CONNACK", header, answer, expected)
    return sock


def subscribe(sock, topic, expected):
    send(sock, 0x82, b"\0\1\0" + string(topic) + b"\0")
    header, answer = receive(sock)
    assert header == 0x90 and answer[-1] == expected, ("SUBACK", header, answer, expected)


def publish(sock, topic, expected, retained=False):
    send(sock, 0x33 if retained else 0x32, string(topic) + b"\0\2\0test-payload")
    header, answer = receive(sock)
    assert header == 0x40, ("PUBACK header", header, answer)
    reason = answer[2] if len(answer) > 2 else 0
    assert reason == expected, ("PUBACK", reason, expected)


def publish_subscribed(sock, topic):
    """Publish from a client that is also subscribed: its own delivery may precede the PUBACK."""
    send(sock, 0x32, string(topic) + b"\0\2\0test-payload")
    deliveries = 0
    for _ in range(10):
        header, answer = receive(sock)
        if header == 0x40:
            assert len(answer) < 3 or answer[2] == 0, ("PUBACK reason", answer)
            assert deliveries == 1, ("expected exactly one echoed delivery", deliveries)
            return
        assert header >> 4 == 3, ("unexpected packet", header)
        deliveries += 1
    raise AssertionError("no PUBACK after a subscribed client's PUBLISH")


def connect_once(port, username, password):
    """One CONNECT and DISCONNECT, as a load generator sees it: TCP, CONNECT, CONNACK, close."""
    with socket.create_connection(("127.0.0.1", port), timeout=30) as sock:
        send(sock, 0x10, string("MQTT") + bytes([5, 0xC2, 0, 60, 0]) +
             string("bench-" + str(time.monotonic_ns())) + string(username) + string(password))
        header, answer = receive(sock)
        if header != 0x20 or answer[1] != 0:
            raise RuntimeError(("CONNECT refused", header, answer))
        send(sock, 0xE0, b"\0")  # DISCONNECT, so the broker closes cleanly.


def connect_round(port, users, password, concurrency):
    """Time `users` CONNECTs at a fixed concurrency; returns (elapsed, connects/second)."""
    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
        futures = [pool.submit(connect_once, port, user, password) for user in users]
        for future in futures:
            future.result()
    elapsed = time.monotonic() - started
    return elapsed, len(users) / elapsed


def run_benchmark(port, count, password, rounds, concurrency, user_format="ldap/bench{:04d}"):
    """repeat rounds of `count` LDAP CONNECTs over distinct users, printing the rate of each."""
    users = [user_format.format(index) for index in range(1, count + 1)]
    rates = []
    for number in range(1, rounds + 1):
        elapsed, rate = connect_round(port, users, password, concurrency)
        rates.append(rate)
        print(f"LDAP CONNECT benchmark round {number}: {count} connects, concurrency "
              f"{concurrency}, {elapsed:.2f} s, {rate:.1f} CONNECT/s", flush=True)
    if len(rates) > 1:
        verdict = ("a cache is in effect" if rates[-1] > rates[0] * 1.5
                   else "no decision cache is being used")
        print("LDAP CONNECT benchmark: first round {:.1f}, last round {:.1f} CONNECT/s - {}"
              .format(rates[0], rates[-1], verdict), flush=True)
    return rates


def run_broker(binary, library, directory_host, directory_port, directory_encryption, ca_file,
               directory, outage, serve=False, standard_ports=False,
               benchmark=None, cache_ttl=60):
    root = pathlib.Path(directory) / "broker"
    root.mkdir()
    if standard_ports:
        # One host, one broker: the installed XMQ must be stopped, and this stand takes its ports.
        port, web_port, bind_address, whitelist = 1883, 18883, "0.0.0.0", ""
    else:
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        web_port = 0
        bind_address, whitelist = "127.0.0.1", "127.0.0.1"
        if serve:
            with socket.socket() as reservation:
                reservation.bind(("0.0.0.0", 0))
                web_port = reservation.getsockname()[1]
        bind_address = "0.0.0.0" if serve else "127.0.0.1"
    database = root / "users.db"
    config = root / "xmq_server.conf"
    # LDAP is first; local accounts remain available when the directory is offline.
    overrides = {
        "database_uri": f"sqlite3://localhost/{database}",
        "connections": {"listener": [{"id": 1, "name": "LDAP stand", "bind_ip": bind_address,
                                      "port": port, "threads": 4, "protocol": "MQTT", "enable": True}]},
        "authentication": {"database_uri": f"sqlite3://localhost/{database}", "allow_anonymous": False},
        "persistence": {"enabled": False},
        "web_service": {"listener_port": web_port, "encrypted": False, "whitelist": whitelist},
        "logging": {"log_to": str(root / "events.log")},
        "server_limits": {"send_threads": "1", "receive_threads": "1", "delivery_threads": "1"},
        "extensions": [
            {"name": "ldap", "library": str(library), "enabled": True, "required": True, "settings": {
                "host": directory_host, "port": directory_port,
                "encryption": directory_encryption, "search_base": "dc=example,dc=org", "bind_dn": "cn=admin,dc=example,dc=org",
                "bind_password": "test-admin", "ca_file": str(ca_file), "timeout_seconds": "1",
                "cache_ttl_seconds": str(cache_ttl),
                "group_mappings": "cn=publishers,dc=example,dc=org => Publishers\n"
                                  "cn=readers,dc=example,dc=org => Readers\n"
                                  "cn=Server Monitoring,dc=example,dc=org => Server Monitoring"}},
            {"name": "user-database", "library": str(library.parent / "libxmq_user_database.so"),
             "enabled": True, "required": True}
        ]
    }
    template = json.loads((pathlib.Path(__file__).resolve().parents[3] / "xmq_server.conf.template").read_text())
    def merge(target, values):
        for key, value in values.items():
            if isinstance(value, dict) and isinstance(target.get(key), dict):
                merge(target[key], value)
            else:
                target[key] = value
    merge(template, overrides)
    extensions = template.pop("extensions")
    extension_config = root / "xmq_extensions.conf"
    extension_config.write_text(json.dumps({"extensions": extensions}))
    extension_config.chmod(0o600)
    config.write_text(json.dumps(template))
    config.chmod(0o600)
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(library.parent) + ":" + env.get("LD_LIBRARY_PATH", "")
    subprocess.run([str(binary), "-c", str(config), "--set-password", "admin"], input="Test-local-admin7!\n",
                   text=True, env=env, check=True, stdout=subprocess.DEVNULL)
    with sqlite3.connect(database) as db:
        for group in ("Publishers", "Readers", "Server Monitoring"):
            db.execute("INSERT INTO xmq_user_group(name) VALUES (?)", (group,))
        for group, action, topic in (
                ("Publishers", "publish", "allowed/#"),
                ("Readers", "subscribe", "allowed/#"),
                ("Server Monitoring", "publish", "brokers/thinker10"),
                ("Server Monitoring", "subscribe", "brokers/thinker10")):
            db.execute("INSERT INTO xmq_acl_rule(group_name, action, deny, topic_filter) VALUES (?, ?, 0, ?)",
                       (group, action, topic))
    sockets = []
    with (root / "server.log").open("w+") as log:
        process = subprocess.Popen([str(binary), "-c", str(config), "-l", str(root / "events.log")],
                                   env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError("test broker exited")
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.1):
                        break
                except OSError:
                    time.sleep(.1)
            else:
                raise RuntimeError("test broker did not open its port")
            writer = connect(port, "ldap/alice", "test-alice")
            reader = connect(port, "ldap/bob", "test-bob")
            sockets.extend((writer, reader))
            subscribe(reader, "allowed/#", 0)
            subscribe(writer, "allowed/#", 0x87)
            subscribe(reader, "forbidden/#", 0x87)
            publish(writer, "allowed/test", 0)
            header, message = receive(reader)
            assert header >> 4 == 3 and message.endswith(b"test-payload"), "message delivery"
            publish(writer, "forbidden/test", 0x87)
            publish(reader, "allowed/test", 0x87)
            publish(writer, "allowed/retained", 0, retained=True)
            receive(reader)
            # The acceptance scenario: one LDAP group grants both directions on one exact topic.
            monitor = connect(port, "ldap/bob", "test-bob")
            sockets.append(monitor)
            subscribe(monitor, "brokers/thinker10", 0)
            publish(writer, "brokers/thinker10", 0)
            header, message = receive(monitor)
            assert header >> 4 == 3 and message.endswith(b"test-payload"), "Server Monitoring delivery"
            publish_subscribed(monitor, "brokers/thinker10")
            # The group grants exactly brokers/thinker10, not other broker topics.
            subscribe(reader, "brokers/thinker11", 0x87)
            publish(writer, "brokers/thinker11", 0x87)
            unmapped = connect(port, "ldap/nogroup", "test-nogroup")
            sockets.append(unmapped)
            subscribe(unmapped, "allowed/#", 0x87)
            unmapped.settimeout(.25)
            try:
                leaked = unmapped.recv(1)
            except socket.timeout:
                pass
            else:
                raise AssertionError(f"refused subscription leaked retained data or disconnected: {leaked!r}")
            finally:
                unmapped.settimeout(10)
            publish(unmapped, "allowed/test", 0x87)
            # The broker currently encodes some CONNECT failures with legacy return codes.
            sockets.append(connect(port, "ldap/alice", "wrong", (0x86, 4, 5)))
            sockets.append(connect(port, "admin", "Test-local-admin7!"))
            if benchmark:
                run_benchmark(port, **benchmark)
            if serve:
                host = socket.gethostname()
                print(f"Manual LDAP stand: MQTT mqtt://{host}:{port} (also 127.0.0.1:"
                      f"{port})", flush=True)
                print(f"Web GUI: http://{host}:{web_port} - admin / Test-local-admin7!", flush=True)
                print("Scenario: ldap/bob / test-bob subscribes brokers/thinker10, "
                      "ldap/alice / test-alice publishes brokers/thinker10 - both are in LDAP and XMQ "
                      "group 'Server Monitoring'. ldap/nogroup / test-nogroup has no topic access. "
                      "Other checks use allowed/#.", flush=True)
                print(f"Configuration/database/logs: {root}. Ctrl+C stops broker and LDAP container.", flush=True)
                while True:
                    if process.poll() is not None:
                        raise RuntimeError("test broker exited")
                    time.sleep(1)
            outage()
            if cache_ttl > 0:
                # A decision made before the outage is still served from the cache.
                sockets.append(connect(port, "ldap/alice", "test-alice"))
            else:
                sockets.append(connect(port, "ldap/alice", "test-alice", (0x88, 3)))
            # An identity never seen before cannot be cached, so an outage must refuse it either way.
            sockets.append(connect(port, "ldap/unseen", "test-unseen", (0x88, 3)))
            sockets.append(connect(port, "admin", "Test-local-admin7!"))
            print(f"MQTT LDAP + mapped groups + native Permissions acceptance passed "
                  f"(cache_ttl={cache_ttl})")
        except Exception:
            log.flush()
            log.seek(0)
            print(log.read())
            raise
        finally:
            for sock in sockets:
                sock.close()
            process.terminate()
            process.wait(timeout=30)
