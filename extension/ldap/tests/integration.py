#!/usr/bin/env python3
"""Isolated disposable OpenLDAP test; no changes to the running XMQ or host trust store."""
import pathlib
import subprocess
import sys
import tempfile
import time
import uuid
import signal


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


def option(name, default, cast=int):
    if name not in sys.argv:
        return default
    index = sys.argv.index(name)
    value = cast(sys.argv[index + 1])
    del sys.argv[index:index + 2]
    return value


binary = str(pathlib.Path(sys.argv[1]).resolve())
serve = "--serve" in sys.argv
standard_ports = "--standard-ports" in sys.argv
benchmark = "--benchmark" in sys.argv
benchmark_users = option("--benchmark-users", 1000)
benchmark_rounds = option("--benchmark-rounds", 2)
benchmark_concurrency = option("--benchmark-concurrency", 32)
cache_ttl = option("--cache-ttl", 60)
for flag in ("--serve", "--standard-ports", "--benchmark"):
    if flag in sys.argv:
        sys.argv.remove(flag)
if serve:
    def stop_stand(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop_stand)
name = "xmq-ldap-test-" + uuid.uuid4().hex[:10]
with tempfile.TemporaryDirectory(prefix="ldap-", dir="/tmp/opencode") as directory:
    certs = pathlib.Path(directory)
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
        "-subj", "/CN=XMQ LDAP test CA", "-keyout", str(certs / "ca.key"),
        "-out", str(certs / "ca.crt"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=ldap-test.invalid",
        "-keyout", str(certs / "ldap.key"), "-out", str(certs / "ldap.csr"),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    (certs / "extensions").write_text("subjectAltName=IP:127.0.0.1\n"
                                      "extendedKeyUsage=serverAuth\nbasicConstraints=CA:FALSE\n")
    run("openssl", "x509", "-req", "-in", str(certs / "ldap.csr"), "-CA", str(certs / "ca.crt"),
        "-CAkey", str(certs / "ca.key"), "-CAcreateserial", "-days", "1",
        "-extfile", str(certs / "extensions"), "-out", str(certs / "ldap.crt"),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # Mount only server credentials and CA certificate, never the CA signing key.
    mount = certs / "server"
    mount.mkdir()
    for file in ("ldap.crt", "ldap.key", "ca.crt"):
        (mount / file).write_bytes((certs / file).read_bytes())
    try:
        run("docker", "run", "-d", "--rm", "--name", name, "--hostname", "localhost",
            "-p", "127.0.0.1::389", "-p", "127.0.0.1::636",
            "-v", f"{mount}:/container/service/slapd/assets/certs:ro",
            "-e", "LDAP_DOMAIN=example.org", "-e", "LDAP_ADMIN_PASSWORD=test-admin",
            "-e", "LDAP_TLS_CRT_FILENAME=ldap.crt", "-e", "LDAP_TLS_KEY_FILENAME=ldap.key",
            "-e", "LDAP_TLS_CA_CRT_FILENAME=ca.crt", "-e", "LDAP_TLS_VERIFY_CLIENT=never",
            "osixia/openldap:1.5.0", "--copy-service")
        for attempt in range(90):
            probe = subprocess.run(["docker", "exec", "-e", "LDAPTLS_REQCERT=never", name, "ldapwhoami", "-x", "-H", "ldaps://localhost",
                                    "-D", "cn=admin,dc=example,dc=org", "-w", "test-admin"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if probe.returncode == 0:
                break
            time.sleep(1)
        else:
            raise RuntimeError("OpenLDAP failed to become ready")
        ldif = """dn: uid=alice,dc=example,dc=org
objectClass: inetOrgPerson
cn: Alice
sn: Test
uid: alice
userPassword: test-alice

dn: uid=bob,dc=example,dc=org
objectClass: inetOrgPerson
cn: Bob
sn: Test
uid: bob
userPassword: test-bob

dn: uid=nogroup,dc=example,dc=org
objectClass: inetOrgPerson
cn: No Group
sn: Test
uid: nogroup
userPassword: test-nogroup

dn: cn=first,dc=example,dc=org
objectClass: inetOrgPerson
cn: first
sn: Test
uid: duplicate
userPassword: test-duplicate

dn: cn=second,dc=example,dc=org
objectClass: inetOrgPerson
cn: second
sn: Test
uid: duplicate
userPassword: test-duplicate

dn: cn=publishers,dc=example,dc=org
objectClass: groupOfNames
cn: publishers
member: uid=alice,dc=example,dc=org

dn: uid=unseen,dc=example,dc=org
objectClass: inetOrgPerson
cn: Unseen
sn: Test
uid: unseen
userPassword: test-unseen

dn: cn=Server Monitoring,dc=example,dc=org
objectClass: groupOfNames
cn: Server Monitoring
member: uid=alice,dc=example,dc=org
member: uid=bob,dc=example,dc=org

dn: cn=readers,dc=example,dc=org
objectClass: groupOfNames
cn: readers
member: uid=bob,dc=example,dc=org

"""
        if benchmark:
            # Distinct users, so a decision cache keyed by user would have to hold each answer.
            ldif += "".join(
                f"""dn: uid=bench{index:04d},dc=example,dc=org
objectClass: inetOrgPerson
cn: Bench {index}
sn: Bench
uid: bench{index:04d}
userPassword: bench-pass

""" for index in range(1, benchmark_users + 1))
        run("docker", "exec", "-i", name, "ldapadd", "-x", "-H", "ldap://localhost",
            "-D", "cn=admin,dc=example,dc=org", "-w", "test-admin", input=ldif)
        for internal, encryption in ((636, "ldaps"), (389, "starttls")):
            port = run("docker", "port", name, f"{internal}/tcp", capture_output=True).stdout.strip().split(":")[-1]
            run(binary, "127.0.0.1", port, encryption, str(certs / "ca.crt"))
        print("OpenLDAP integration passed over LDAPS and StartTLS")
        if len(sys.argv) == 3:
            from broker import run_broker
            server = pathlib.Path(sys.argv[2]).resolve()
            library = pathlib.Path(__file__).resolve().parents[3] / "lib/libxmq_ldap.so"
            port = run("docker", "port", name, "636/tcp", capture_output=True).stdout.strip().split(":")[-1]
            run_broker(server, library, "127.0.0.1", port, "ldaps", certs / "ca.crt", directory,
                       lambda: run("docker", "stop", name, stdout=subprocess.DEVNULL), serve=serve,
                       standard_ports=standard_ports,
                       benchmark=({"count": benchmark_users, "password": "bench-pass",
                                   "rounds": benchmark_rounds,
                                   "concurrency": benchmark_concurrency} if benchmark else None),
                       cache_ttl=cache_ttl)
    except Exception:
        subprocess.run(["docker", "logs", "--tail", "25", name])
        raise
    finally:
        subprocess.run(["docker", "rm", "-f", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
