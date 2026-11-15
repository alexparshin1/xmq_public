#!/usr/bin/env python3
"""Disposable Keycloak + isolated MQTT 5 acceptance stand. Never uses port 1883.

Run from a matching SPTK/XMQ build:
  LD_LIBRARY_PATH=/path/to/sptk/lib python3 extension/jwt/tests/keycloak.py ./xmq_server ./lib
"""
import base64
import importlib.util
import json
import os
import pathlib
import secrets
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

REPO = pathlib.Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("mqtt_acceptance", REPO / "extension/ldap/tests/broker.py")
mqtt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mqtt)


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, **kwargs)


def request(url, data=None, token=None, method=None):
    headers = {}
    if token:
        headers["Authorization"] = "Bearer " + token
    if isinstance(data, dict):
        data = json.dumps(data).encode()
        headers["Content-Type"] = "application/json"
    elif isinstance(data, str):
        data = data.encode()
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    with urllib.request.urlopen(urllib.request.Request(
            url, data=data, headers=headers, method=method), timeout=5) as response:
        body = response.read()
        return json.loads(body) if body else None


def kid(token):
    header = token.split(".")[0]
    return json.loads(base64.urlsafe_b64decode(header + "=" * (-len(header) % 4)))["kid"]


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    libraries = pathlib.Path(sys.argv[2]).resolve()
    root = pathlib.Path(tempfile.mkdtemp(prefix="jwt-keycloak-", dir="/tmp/opencode"))
    root.chmod(0o755)
    name = "xmq-jwt-" + secrets.token_hex(5)
    admin_password = secrets.token_urlsafe(20)
    password = "Acceptance7!" + secrets.token_urlsafe(20)
    process = None
    sockets = []
    created = False
    print("Artifacts:", root, flush=True)
    realm = {
        "realm": "xmq-acceptance", "enabled": True, "sslRequired": "none",
        "accessTokenLifespan": 5,
        "roles": {"realm": [{"name": "Publishers"}, {"name": "Readers"}]},
        "clients": [{
            "clientId": "xmq", "publicClient": True, "directAccessGrantsEnabled": True,
            "protocol": "openid-connect",
            "protocolMappers": [{
                "name": "xmq-audience", "protocol": "openid-connect",
                "protocolMapper": "oidc-audience-mapper",
                "config": {"included.client.audience": "xmq", "access.token.claim": "true"}
            }]
        }],
        "users": [{
            "username": username, "enabled": True,
            "firstName": username, "lastName": "Acceptance",
            "email": username + "@example.invalid", "emailVerified": True,
            "credentials": [{"type": "password", "value": password, "temporary": False}],
            "realmRoles": roles
        } for username, roles in [
            ("alice", ["Publishers"]), ("bob", ["Readers"]), ("nogroup", [])
        ]]
    }
    realm_file = root / "realm.json"
    realm_file.write_text(json.dumps(realm))
    realm_file.chmod(0o644)
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(libraries) + ":" + env.get("LD_LIBRARY_PATH", "")
    try:
        run("docker", "run", "-d", "--name", name,
            "-p", "127.0.0.1::8080",
            "-v", f"{realm_file}:/opt/keycloak/data/import/realm.json:ro",
            "-e", "KC_BOOTSTRAP_ADMIN_USERNAME=admin",
            "-e", f"KC_BOOTSTRAP_ADMIN_PASSWORD={admin_password}",
            "quay.io/keycloak/keycloak:26.4.7", "start-dev", "--import-realm",
            stdout=subprocess.DEVNULL)
        created = True
        endpoint = run("docker", "port", name, "8080/tcp", capture_output=True).stdout.strip()
        base = "http://" + endpoint
        issuer = base + "/realms/xmq-acceptance"
        for _ in range(120):
            try:
                request(issuer + "/.well-known/openid-configuration")
                break
            except (OSError, ValueError):
                time.sleep(1)
        else:
            raise RuntimeError("Keycloak did not become ready")

        def token_for(username):
            return request(issuer + "/protocol/openid-connect/token",
                           urllib.parse.urlencode({"grant_type": "password", "client_id": "xmq",
                                                   "username": username, "password": password}))["access_token"]

        admin = request(base + "/realms/master/protocol/openid-connect/token",
                        urllib.parse.urlencode({"grant_type": "password", "client_id": "admin-cli",
                                                "username": "admin", "password": admin_password}))["access_token"]
        realm_id = request(base + "/admin/realms/xmq-acceptance", token=admin)["id"]

        def rotate(priority):
            request(base + "/admin/realms/xmq-acceptance/components", {
                "name": f"acceptance-rsa-{priority}", "parentId": realm_id,
                "providerId": "rsa-generated", "providerType": "org.keycloak.keys.KeyProvider",
                "config": {"priority": [str(priority)], "enabled": ["true"],
                           "active": ["true"], "algorithm": ["RS256"], "keySize": ["2048"]}
            }, token=admin, method="POST")

        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        database = root / "users.db"
        config = json.loads((REPO / "xmq_server.conf.template").read_text())
        overrides = {
            "database_uri": f"sqlite3://localhost/{database}",
            "connections": {"listener": [{"id": 1, "name": "JWT acceptance",
                "bind_ip": "127.0.0.1", "port": port, "threads": 4, "protocol": "MQTT", "enable": True}]},
            "authentication": {"database_uri": f"sqlite3://localhost/{database}", "allow_anonymous": False},
            "persistence": {"enabled": False},
            "web_service": {"listener_port": 0, "encrypted": False, "whitelist": "127.0.0.1"},
            "logging": {"log_to": str(root / "events.log")},
            "server_limits": {"send_threads": "1", "receive_threads": "1", "delivery_threads": "1"}
        }

        def merge(target, values):
            for key, value in values.items():
                if isinstance(value, dict) and isinstance(target.get(key), dict):
                    merge(target[key], value)
                else:
                    target[key] = value
        merge(config, overrides)
        config.pop("extensions", None)
        config_file = root / "xmq_server.conf"
        config_file.write_text(json.dumps(config))
        extension_config = {"extensions": [
            {"name": "jwt", "library": str(libraries / "libxmq_jwt.so"),
             "enabled": True, "required": True, "settings": {
                 "algorithm": "RS256", "jwks_url": issuer + "/protocol/openid-connect/certs",
                 "issuer": issuer, "audience": "xmq", "groups_claim": "realm_access.roles",
                 "group_mappings": "Publishers => Publishers\nReaders => Readers",
                 "clock_skew_seconds": "0", "timeout_seconds": "2",
                 "cache_ttl_seconds": "0", "negative_cache_ttl_seconds": "0"}},
            {"name": "user-database", "library": str(libraries / "libxmq_user_database.so"),
             "enabled": True, "required": True}
        ]}
        (root / "xmq_extensions.conf").write_text(json.dumps(extension_config))
        config_file.chmod(0o600)
        (root / "xmq_extensions.conf").chmod(0o600)
        run(str(binary), "-c", str(config_file), "--set-password", "admin",
            input=password + "\n", env=env, stdout=subprocess.DEVNULL)
        with sqlite3.connect(database) as db:
            for group in ("Publishers", "Readers"):
                db.execute("INSERT INTO xmq_user_group(name) VALUES (?)", (group,))
            for group, action in (("Publishers", "publish"), ("Readers", "subscribe")):
                db.execute("INSERT INTO xmq_acl_rule(group_name,action,deny,topic_filter) VALUES (?,?,0,?)",
                           (group, action, "allowed/#"))
        with (root / "server.log").open("w") as log:
            process = subprocess.Popen([str(binary), "-c", str(config_file)],
                                       env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
        for _ in range(100):
            if process.poll() is not None:
                raise RuntimeError("broker exited; see server.log")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.1):
                    break
            except OSError:
                time.sleep(.1)
        else:
            raise RuntimeError("broker did not listen")

        def connect(username, token, expected=0):
            sock = mqtt.connect(port, username, token, expected)
            sockets.append(sock)
            return sock

        alice = token_for("alice")
        writer = connect("jwt/alice", alice)
        reader = connect("jwt/bob", token_for("bob"))
        mqtt.subscribe(reader, "allowed/#", 0)
        mqtt.subscribe(writer, "allowed/#", 0x87)
        mqtt.publish(writer, "allowed/test", 0)
        header, message = mqtt.receive(reader)
        assert header >> 4 == 3 and message.endswith(b"test-payload")
        mqtt.publish(writer, "forbidden/test", 0x87)
        mqtt.publish(reader, "allowed/test", 0x87)
        unmapped = connect("jwt/nogroup", token_for("nogroup"))
        mqtt.publish(unmapped, "allowed/test", 0x87)
        mqtt.subscribe(unmapped, "allowed/#", 0x87)
        refused = (0x86,)
        connect("jwt/bob", alice, refused)
        with socket.create_connection(("127.0.0.1", port), timeout=10) as legacy:
            mqtt.send(legacy, 0x10, mqtt.string("MQTT") + bytes([4, 0xC2, 0, 30]) +
                      mqtt.string("jwt-legacy-refusal") + mqtt.string("jwt/bob") + mqtt.string(alice))
            header, answer = mqtt.receive(legacy)
            assert header == 0x20 and answer == b"\x00\x04", ("MQTT 3.1.1 CONNACK", header, answer)
        print("PASS real Keycloak login, identity binding, groups, ACL and delivery", flush=True)
        print("PASS protocol-specific refusal: MQTT 5 uses 0x86, MQTT 3.1.1 uses 0x04", flush=True)
        time.sleep(6)
        connect("jwt/alice", alice, refused)
        print("PASS expired token refused (decision cache disabled)", flush=True)
        # Subsequent checks exercise rotation/outage, not short token expiry. Leave enough time
        # for a loaded development machine to stop the provider and perform all MQTT handshakes.
        request(base + "/admin/realms/xmq-acceptance", {"accessTokenLifespan": 120},
                token=admin, method="PUT")
        rotate(200)
        rotated = token_for("alice")
        assert kid(rotated) != kid(alice), "Keycloak did not rotate signing key"
        connect("jwt/alice", rotated)
        print("PASS new kid refreshes JWKS and authenticates", flush=True)
        rotate(300)
        unknown = token_for("alice")
        assert kid(unknown) != kid(rotated)
        run("docker", "stop", "--timeout", "2", name, stdout=subprocess.DEVNULL)
        connect("jwt/alice", rotated)
        connect("jwt/alice", unknown, refused + (0x88,))
        connect("admin", password)
        print("PASS provider outage: cached signing key works, unknown kid refused, local admin works",
              flush=True)
        (root / "result.txt").write_text("PASS: real Keycloak MQTT acceptance; cache disabled\n")
        print("JWT KEYCLOAK ACCEPTANCE PASSED", flush=True)
    finally:
        for sock in sockets:
            sock.close()
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if created:
            with (root / "keycloak.log").open("w") as log:
                subprocess.run(["docker", "logs", name], stdout=log, stderr=log)
            subprocess.run(["docker", "rm", "-f", name], stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
