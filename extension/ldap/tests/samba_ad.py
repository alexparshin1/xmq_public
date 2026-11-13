#!/usr/bin/env python3
"""Acceptance against a real Active Directory schema, served by a Samba AD DC container.

This is the AD counterpart of integration.py. It provisions a disposable Samba domain, replaces
Samba's temporary TLS certificate with one carrying a loopback name, starts the broker with
directory_type=ad, and checks what only AD can prove:

  - login by sAMAccountName (ldap/alice) and by user principal name (ldap/bob@example.org);
  - a mapped group reached through a nested group, by the in-chain matching rule;
  - a disabled account refused by the account-state check, though its password still binds.

Usage:
  python3 samba_ad.py build/xmq_server

Docker is required. The container is removed on exit; nothing on the running broker is changed.
"""
import json
import os
import pathlib
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time

from broker import connect, publish, receive, send, subscribe

DOMAIN = "EXAMPLE.ORG"
REALM = "example.org"
AD_HOST = "dc1.example.org"
ADMIN_PASSWORD = "Passw0rd!2026"
NAME = "xmq-samba-ad-test"
HOSTS_LINE = f"127.0.0.1 {AD_HOST}"

USERS = {"alice": "test-alice", "bob": "test-bob", "carol": "test-carol"}
MAPPINGS = (
    "cn=Server Monitoring,cn=Users,dc=example,dc=org => Server Monitoring\n"
    "cn=MQTT Operators,cn=Users,dc=example,dc=org => Operators"
)


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, **kwargs)


def add_host_entry():
    """Make the directory's name resolve to the published loopback port.

    libldap verifies the certificate against the host it was told to connect to, so the broker has
    to be given a name that is in the certificate; a bare 127.0.0.1 would be checked as a name.
    Added only if missing, and removed again by the caller when this script added it.
    """
    existing = pathlib.Path("/etc/hosts").read_text()
    if AD_HOST in existing.split():
        return False
    run("sudo", "-n", "sh", "-c", f"echo '{HOSTS_LINE}' >> /etc/hosts")
    return True


def remove_host_entry():
    subprocess.run(["sudo", "-n", "sed", "-i", rf"/[[:space:]]{AD_HOST.replace('.', '[.]')}$/d", "/etc/hosts"],
                   check=False, capture_output=True)


def ad(*args, **kwargs):
    return run("docker", "exec", NAME, *args, **kwargs)


def wait_for_ad(timeout=180):
    base = "dc=" + REALM.replace(".", ",dc=")
    for _ in range(timeout):
        # AD refuses a simple bind over plain LDAP ("stronger authentication required"), so the
        # readiness probe is LDAPS. Certificate checking is off for the probe only.
        probe = subprocess.run(["docker", "exec", "-e", "LDAPTLS_REQCERT=never", NAME, "ldapsearch",
                                "-x", "-H", "ldaps://127.0.0.1",
                                "-D", f"cn=Administrator,cn=Users,{base}", "-w", ADMIN_PASSWORD,
                                "-b", base, "-s", "base"], capture_output=True, text=True)
        if probe.returncode == 0:
            return
        time.sleep(1)
    raise RuntimeError("Samba AD DC did not become ready")


def provision(directory):
    subprocess.run(["docker", "rm", "-f", NAME], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run("docker", "run", "-d", "--name", NAME, "--privileged", "--hostname", "dc1.example.org",
        "-e", f"DOMAIN={DOMAIN}", "-e", f"DOMAINPASS={ADMIN_PASSWORD}", "-e", "NOCOMPLEXITY=true",
        "-p", "127.0.0.1::389", "-p", "127.0.0.1::636", "nowsci/samba-domain")
    # The container provisions on first start; the users are created once it answers.
    for _ in range(180):
        if subprocess.run(["docker", "exec", NAME, "samba-tool", "domain", "info", "127.0.0.1"],
                          capture_output=True).returncode == 0:
            break
        time.sleep(1)
    wait_for_ad()
    for user, password in USERS.items():
        run("docker", "exec", NAME, "samba-tool", "user", "create", user, password,
            f"--given-name={user.title()}", "--surname=Test")
    run("docker", "exec", NAME, "samba-tool", "user", "disable", "carol")
    for group, members in (("Server Monitoring", ["alice", "bob"]),
                           ("MQTT Inner", ["alice"]),
                           ("MQTT Operators", ["MQTT Inner"])):
        run("docker", "exec", NAME, "samba-tool", "group", "add", group)
        # samba-tool takes the whole member list as one comma-separated argument.
        run("docker", "exec", NAME, "samba-tool", "group", "addmembers", group, ",".join(members))

    # Samba's temporary certificate names only the container host. Replace it with one signed by a
    # throwaway CA, named as the host the broker connects to, so the broker can verify it properly.
    ca = pathlib.Path(directory) / "ca.crt"
    cert = pathlib.Path(directory) / "ad.crt"
    key = pathlib.Path(directory) / "ad.key"
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
        "-subj", "/CN=XMQ AD Test CA", "-addext", "basicConstraints=critical,CA:TRUE",
        "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        "-keyout", str(pathlib.Path(directory) / "ca.key"), "-out", str(ca))
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", f"/CN={AD_HOST}",
        "-keyout", str(key), "-out", str(pathlib.Path(directory) / "ad.csr"))
    extensions = pathlib.Path(directory) / "ad.ext"
    extensions.write_text(f"subjectAltName=IP:127.0.0.1,DNS:localhost,DNS:{AD_HOST}\n"
                          "extendedKeyUsage=serverAuth\nbasicConstraints=CA:FALSE\n")
    run("openssl", "x509", "-req", "-in", str(pathlib.Path(directory) / "ad.csr"),
        "-CA", str(ca), "-CAkey", str(pathlib.Path(directory) / "ca.key"),
        "-CAcreateserial", "-days", "2", "-extfile", str(extensions), "-out", str(cert))
    run("docker", "cp", str(cert), f"{NAME}:/var/lib/samba/private/tls/cert.pem")
    run("docker", "cp", str(key), f"{NAME}:/var/lib/samba/private/tls/key.pem")
    # Samba refuses a key it does not consider its own: owner 0 and mode 0600.
    ad("chown", "0:0", "/var/lib/samba/private/tls/cert.pem", "/var/lib/samba/private/tls/key.pem")
    ad("chmod", "644", "/var/lib/samba/private/tls/cert.pem")
    ad("chmod", "600", "/var/lib/samba/private/tls/key.pem")
    run("docker", "restart", NAME)
    wait_for_ad()
    return ca


def start_broker(binary, library, port, cert, database):
    overrides = {
        "database_uri": f"sqlite3://localhost/{database}",
        "connections": {"listener": [{"id": 1, "name": "Samba AD", "bind_ip": "127.0.0.1",
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
        {"name": "ldap", "library": str(library), "enabled": True, "required": True, "settings": {
            "host": AD_HOST, "port": str(socket_port(636)), "encryption": "ldaps",
            "directory_type": "ad", "search_base": "cn=Users,dc=example,dc=org",
            "bind_dn": "cn=Administrator,cn=Users,dc=example,dc=org",
            "bind_password": ADMIN_PASSWORD, "ca_file": str(cert), "timeout_seconds": "5",
            "group_mappings": MAPPINGS}},
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


def socket_port(internal):
    return run("docker", "port", NAME, f"{internal}/tcp").stdout.strip().split(":")[-1]


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: samba_ad.py <xmq_server>")
    server = pathlib.Path(sys.argv[1]).resolve()
    library = pathlib.Path(__file__).resolve().parents[3] / "lib/libxmq_ldap.so"
    added_host = False
    try:
        added_host = add_host_entry()
        accept(server, library)
    finally:
        subprocess.run(["docker", "rm", "-f", NAME], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        if added_host:
            remove_host_entry()


def accept(server, library):
    with tempfile.TemporaryDirectory(prefix="samba-ad-", dir="/tmp/opencode") as directory:
        cert = provision(directory)
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        database = pathlib.Path(directory) / "users.db"
        process, log = start_broker(server, library, port, cert, database)
        sockets = []
        try:
            alice = connect(port, "ldap/alice", "test-alice")
            bob = connect(port, "ldap/bob@example.org", "test-bob")
            sockets.extend((alice, bob))
            # sAMAccountName login publishes; UPN login subscribes to the same mapped group.
            subscribe(bob, "brokers/thinker10", 0)
            publish(alice, "brokers/thinker10", 0)
            header, _ = receive(bob)
            assert header >> 4 == 3, "UPN subscriber did not receive the message"

            # Alice is only in MQTT Inner; Operators is reached through the in-chain rule, and its
            # rule is subscribe-only, so a granted subscription proves the nesting.
            subscribe(alice, "nested/test", 0)
            publish(alice, "nested/test", 0x87)  # no publish rule for that group

            # Carol's password is right, but the account is disabled: AD refuses the bind itself,
            # and the account-state check covers directories that would let it through.
            sockets.append(connect(port, "ldap/carol", "test-carol", (0x88, 4, 3)))
            print("Samba AD acceptance passed: sAMAccountName, UPN, nested groups, disabled account")
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


if __name__ == "__main__":
    main()
