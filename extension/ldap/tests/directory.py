"""Shared plumbing for the directory acceptance harnesses.

`samba_ad.py` provisions its own disposable domain; `windows_ad.py` runs against a domain controller
that already exists. Both then do the same thing: start the broker with an LDAP extension pointing
at that directory and check the behaviour that only a real directory can prove. That part lives
here so the two harnesses cannot drift apart.
"""
import json
import os
import pathlib
import socket
import sqlite3
import subprocess
import time

from broker import connect, publish, receive, subscribe

# A refusal from the extension is "not authorized" (0x87); a disabled account at the directory is
# either a bad user name or password (0x86) or "banned" (0x88), depending on what the directory says.
REFUSED = (0x86, 0x87, 0x88, 4, 3)


def add_host_entry(host, address="127.0.0.1"):
    """Make the directory's name resolve to the address the broker will connect to.

    libldap verifies the certificate against the name it was given, so the broker has to be pointed
    at a name that appears in the certificate. Added only if missing; the caller removes it.
    """
    if host in pathlib.Path("/etc/hosts").read_text().split():
        return False
    subprocess.run(["sudo", "-n", "sh", "-c", f"echo '{address} {host}' >> /etc/hosts"], check=True)
    return True


def remove_host_entry(host):
    subprocess.run(["sudo", "-n", "sed", "-i", rf"/[[:space:]]{host.replace('.', '[.]')}$/d", "/etc/hosts"],
                   check=False, capture_output=True)


def start_broker(binary, library, port, settings, database, listener="Directory"):
    """Start a broker whose LDAP extension uses `settings`, with the group rules the checks need."""
    overrides = {
        "database_uri": f"sqlite3://localhost/{database}",
        "connections": {"listener": [{"id": 1, "name": listener, "bind_ip": "127.0.0.1",
                                      "port": port, "threads": 4, "protocol": "MQTT", "enable": True}]},
        "authentication": {"database_uri": f"sqlite3://localhost/{database}", "allow_anonymous": False},
        "persistence": {"enabled": False},
        "web_service": {"listener_port": 0},
        "logging": {"log_to": str(database.parent / "events.log")},
        "server_limits": {"send_threads": "1", "receive_threads": "1", "delivery_threads": "1"},
    }
    template = json.loads((pathlib.Path(__file__).resolve().parents[3] / "xmq_server.conf.template").read_text())

    def merge(target, values):
        for key, value in values.items():
            if isinstance(value, dict) and isinstance(target.get(key), dict):
                merge(target[key], value)
            else:
                target[key] = value

    merge(template, overrides)
    config = database.parent / "xmq_server.conf"
    config.write_text(json.dumps(template))
    config.chmod(0o600)
    (database.parent / "xmq_extensions.conf").write_text(json.dumps({"extensions": [
        {"name": "ldap", "library": str(library), "enabled": True, "required": True,
         "settings": settings},
        {"name": "user-database", "library": str(library.parent / "libxmq_user_database.so"),
         "enabled": True, "required": True}]}))
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(library.parent) + ":" + env.get("LD_LIBRARY_PATH", "")
    subprocess.run([str(binary), "-c", str(config), "--set-password", "admin"],
                   input="Test-local-admin7!\n", text=True, env=env, check=True,
                   stdout=subprocess.DEVNULL)
    with sqlite3.connect(database) as db:
        for group in ("Server Monitoring", "Operators"):
            db.execute("INSERT INTO xmq_user_group(name) VALUES (?)", (group,))
        for group, action, topic in (("Server Monitoring", "publish", "brokers/thinker10"),
                                     ("Server Monitoring", "subscribe", "brokers/thinker10"),
                                     ("Operators", "subscribe", "nested/test")):
            db.execute("INSERT INTO xmq_acl_rule(group_name, action, deny, topic_filter) VALUES (?, ?, 0, ?)",
                       (group, action, topic))
    log = (database.parent / "server.log").open("w+")
    process = subprocess.Popen([str(binary), "-c", str(config)], env=env, stdin=subprocess.DEVNULL,
                               stdout=log, stderr=log)
    for _ in range(100):
        if process.poll() is not None:
            log.flush()
            log.seek(0)
            print(log.read())
            raise RuntimeError("broker exited")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=.1):
                return process, log
        except OSError:
            time.sleep(.1)
    raise RuntimeError("broker did not open its port")


def cache_rounds(port, username, password, rounds=2, per_round=200, concurrency=8):
    """Time repeated CONNECTs for one identity; the first round reaches the directory.

    The cache is keyed by the login and a hash of the password, so the same identity and password
    only reach the directory once per lifetime. A later round much faster than the first means the
    cache answered instead.
    """
    import concurrent.futures
    import time

    from broker import connect_once

    rates = []
    for number in range(1, rounds + 1):
        started = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
            for future in [pool.submit(connect_once, port, username, password)
                           for _ in range(per_round)]:
                future.result()
        elapsed = time.monotonic() - started
        rates.append(per_round / elapsed)
        print(f"cache round {number}: {per_round} connects, concurrency {concurrency}, "
              f"{elapsed:.2f} s, {rates[-1]:.1f} CONNECT/s", flush=True)
    return rates


def verify(port, upn, disabled):
    """Run the checks that only an AD schema can prove, against a broker already listening."""
    sockets = []
    try:
        # alice logs in by sAMAccountName and publishes; bob logs in by user principal name and
        # receives it through the same mapped group.
        alice = connect(port, "ldap/alice", "Xmq-Accept-1!")
        bob = connect(port, upn, "Xmq-Accept-2!")
        sockets.extend((alice, bob))
        subscribe(bob, "brokers/thinker10", 0)
        publish(alice, "brokers/thinker10", 0)
        header, _ = receive(bob)
        assert header >> 4 == 3, "UPN subscriber did not receive the message"

        # alice is only in MQTT Inner; Operators is reached through the in-chain rule, and its rule
        # is subscribe-only, so a granted subscription proves the nesting.
        subscribe(alice, "nested/test", 0)
        publish(alice, "nested/test", 0x87)  # no publish rule for that group

        # The disabled account's password is right; the directory refuses it.
        sockets.append(connect(port, f"ldap/{disabled}", "Xmq-Accept-3!", REFUSED))
    finally:
        for sock in sockets:
            sock.close()
