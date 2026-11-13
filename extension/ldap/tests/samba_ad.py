#!/usr/bin/env python3
"""Acceptance against a real Active Directory schema, served by a Samba AD DC container.

This is the AD counterpart of integration.py. It provisions a disposable Samba domain, replaces
Samba's temporary TLS certificate with one signed by a throwaway CA, starts the broker with
directory_type=ad, and checks what only AD can prove:

  - login by sAMAccountName (ldap/alice) and by user principal name (ldap/bob@example.org);
  - a mapped group reached through a nested group, by the in-chain matching rule;
  - a disabled account refused by the directory.

Usage:
  python3 samba_ad.py build/xmq_server

Docker is required, and passwordless sudo for the temporary /etc/hosts entry. The container is
removed on exit; nothing on the running broker is changed.
"""
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

import directory
from broker import connect, publish, receive, send, subscribe

DOMAIN = "EXAMPLE.ORG"
REALM = "example.org"
AD_HOST = "dc1.example.org"
ADMIN_PASSWORD = "Passw0rd!2026"
NAME = "xmq-samba-ad-test"

USERS = {"alice": "Xmq-Accept-1!", "bob": "Xmq-Accept-2!", "carol": "Xmq-Accept-3!"}
MAPPINGS = (
    "cn=Server Monitoring,cn=Users,dc=example,dc=org => Server Monitoring\n"
    "cn=MQTT Operators,cn=Users,dc=example,dc=org => Operators"
)


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, **kwargs)


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


def provision(workdir):
    subprocess.run(["docker", "rm", "-f", NAME], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run("docker", "run", "-d", "--name", NAME, "--privileged", "--hostname", AD_HOST,
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
    # throwaway CA, carrying the address the broker connects to, so it can be verified properly.
    ca = workdir / "ca.crt"
    cert = workdir / "ad.crt"
    key = workdir / "ad.key"
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
        "-subj", "/CN=XMQ AD Test CA", "-addext", "basicConstraints=critical,CA:TRUE",
        "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        "-keyout", str(workdir / "ca.key"), "-out", str(ca))
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", f"/CN={AD_HOST}",
        "-keyout", str(key), "-out", str(workdir / "ad.csr"))
    extensions = workdir / "ad.ext"
    extensions.write_text(f"subjectAltName=IP:127.0.0.1,DNS:localhost,DNS:{AD_HOST}\n"
                          "extendedKeyUsage=serverAuth\nbasicConstraints=CA:FALSE\n")
    run("openssl", "x509", "-req", "-in", str(workdir / "ad.csr"),
        "-CA", str(ca), "-CAkey", str(workdir / "ca.key"),
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


def socket_port(internal):
    return run("docker", "port", NAME, f"{internal}/tcp").stdout.strip().split(":")[-1]


def settings(ca):
    return {
        "host": AD_HOST, "port": socket_port(636), "encryption": "ldaps",
        "directory_type": "ad", "search_base": "cn=Users,dc=example,dc=org",
        "bind_dn": "cn=Administrator,cn=Users,dc=example,dc=org",
        "bind_password": ADMIN_PASSWORD, "ca_file": str(ca), "timeout_seconds": "5",
        "group_mappings": MAPPINGS,
    }


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: samba_ad.py <xmq_server>")
    server = pathlib.Path(sys.argv[1]).resolve()
    library = pathlib.Path(__file__).resolve().parents[3] / "lib/libxmq_ldap.so"
    added_host = False
    try:
        added_host = directory.add_host_entry(AD_HOST)
        accept(server, library)
    finally:
        subprocess.run(["docker", "rm", "-f", NAME], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        if added_host:
            directory.remove_host_entry(AD_HOST)


def accept(server, library):
    with tempfile.TemporaryDirectory(prefix="samba-ad-", dir="/tmp/opencode") as workdir:
        ca = provision(pathlib.Path(workdir))
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        database = pathlib.Path(workdir) / "users.db"
        process, log = directory.start_broker(server, library, port, settings(ca), database, "Samba AD")
        try:
            directory.verify(port, "ldap/bob@example.org", "carol")
            print("Samba AD acceptance passed: sAMAccountName, UPN, nested groups, disabled account")
        except Exception:
            log.flush()
            log.seek(0)
            print(log.read())
            raise
        finally:
            process.terminate()
            process.wait(timeout=30)


if __name__ == "__main__":
    main()
