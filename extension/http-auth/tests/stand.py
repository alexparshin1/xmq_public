#!/usr/bin/env python3
"""Isolated MQTT 5 acceptance stand for xmq_http_auth. Never uses port 1883.

The service is a local stand-in for a company authentication endpoint; group permissions are
checked through XMQ's native ACL rules, not through the service.

Run from a matching build:
  LD_LIBRARY_PATH=/path/to/sptk/lib python3 extension/http-auth/tests/stand.py ./xmq_server ./lib
"""
import http.server
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
import threading

REPO = pathlib.Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("mqtt_acceptance", REPO / "extension/ldap/tests/broker.py")
mqtt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mqtt)

PASSWORD = "Acceptance7!" + secrets.token_urlsafe(16)
LOCAL_PASSWORD = "Local7!" + secrets.token_urlsafe(16)
CALLS = 0
REQUESTS = []
DOWN = threading.Event()


class Service(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, payload=b""):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        if payload:
            self.wfile.write(payload)

    def do_POST(self):
        global CALLS
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length))
        CALLS += 1
        REQUESTS.append(request)
        name, password = request.get("username"), request.get("password")
        if DOWN.is_set():
            self.reply(503)
        elif name == "alice" and password == PASSWORD:
            self.reply(200, b'{"decision":"allow","groups":["Publishers"]}')
        elif name == "bob" and password == PASSWORD:
            self.reply(200, b'{"decision":"allow","groups":["Readers","Readers"]}')
        elif name == "nogroup":
            self.reply(200, b'{"decision":"allow","groups":[]}')
        elif name == "bad":
            self.reply(403)
        elif name == "invalid":
            self.reply(200, b'{"decision": ')
        elif name == "service":
            self.reply(200, b'{"decision":"not_handled"}')
        else:
            self.reply(200, b'{"decision":"deny"}')


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    libraries = pathlib.Path(sys.argv[2]).resolve()
    root = pathlib.Path(tempfile.mkdtemp(prefix="http-auth-", dir="/tmp/opencode"))
    root.chmod(0o755)
    print("Artifacts:", root, flush=True)

    service = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Service)
    threading.Thread(target=service.serve_forever, daemon=True).start()
    endpoint = "http://127.0.0.1:%d/authenticate" % service.server_address[1]

    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]

    database = root / "users.db"
    config = json.loads((REPO / "xmq_server.conf.template").read_text())
    config["database_uri"] = f"sqlite3://localhost/{database}"
    config["connections"]["listener"] = [{
        "id": 1, "name": "HTTP auth acceptance", "bind_ip": "127.0.0.1", "port": port,
        "threads": 4, "protocol": "MQTT", "enable": True}]
    config["authentication"].update({"database_uri": f"sqlite3://localhost/{database}",
                                     "allow_anonymous": False})
    config["persistence"]["enabled"] = False
    config["web_service"].update({"listener_port": 0, "encrypted": False, "whitelist": "127.0.0.1"})
    config["logging"]["log_to"] = str(root / "events.log")
    config["server_limits"].update(
        {"send_threads": "1", "receive_threads": "1", "delivery_threads": "1"})
    config.pop("extensions", None)
    config_file = root / "xmq_server.conf"
    config_file.write_text(json.dumps(config))
    (root / "xmq_extensions.conf").write_text(json.dumps({"extensions": [
        {"name": "http-auth", "library": str(libraries / "libxmq_http_auth.so"),
         "enabled": True, "required": True, "settings": {
             "url": endpoint, "username_prefix": "http/", "timeout_seconds": "2",
             "cache_ttl_seconds": "60", "negative_cache_ttl_seconds": "10", "max_inflight": "4"}},
        {"name": "user-database", "library": str(libraries / "libxmq_user_database.so"),
         "enabled": True, "required": True, "settings": {"unknown_user": "deny"}}]}))
    for path in (config_file, root / "xmq_extensions.conf"):
        path.chmod(0o600)

    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = str(libraries) + ":" + env.get("LD_LIBRARY_PATH", "")
    subprocess.run([str(binary), "-c", str(config_file), "--set-password", "admin"],
                   input="Admin7!" + secrets.token_urlsafe(24) + "\n", text=True, env=env, check=True,
                   stdout=subprocess.DEVNULL)

    # SHA-512 crypt is one of the verifiers the user-database extension accepts.
    local_hash = subprocess.run(["openssl", "passwd", "-6", LOCAL_PASSWORD], check=True,
                                text=True, capture_output=True).stdout.strip()
    with sqlite3.connect(database) as db:
        for group in ("Publishers", "Readers"):
            db.execute("INSERT INTO xmq_user_group(name) VALUES (?)", (group,))
        for group, action in (("Publishers", "publish"), ("Readers", "subscribe")):
            db.execute("INSERT INTO xmq_acl_rule(group_name,action,deny,topic_filter) VALUES (?,?,0,?)",
                       (group, action, "allowed/#"))
        db.execute("INSERT INTO xmq_user(username,password,is_enabled) VALUES (?,?,1)",
                   ("http/service", local_hash))

    process = None
    sockets = []
    with (root / "server.log").open("w") as log:
        process = subprocess.Popen([str(binary), "-c", str(config_file)], env=env,
                                   stdin=subprocess.DEVNULL, stdout=log, stderr=log)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError("broker exited; see server.log")
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.1):
                        break
                except OSError:
                    pass
                threading.Event().wait(.1)
            else:
                raise RuntimeError("broker did not listen")

            def connect(username, password=PASSWORD, expected=0, client_id=None):
                sock = mqtt.connect(port, username, password, expected, client_id)
                sockets.append(sock)
                return sock

            alice = connect("http/alice")
            request = REQUESTS[-1]
            assert request["username"] == "alice" and request["password"] == PASSWORD
            assert request["protocol_version"] == 5 and request["encrypted"] is False
            assert request["remote_address"] == "127.0.0.1"
            calls = CALLS
            connect("http/alice", client_id="cached-client")
            connect("http/alice", client_id="cached-client")
            assert CALLS == calls + 1, "a repeated ALLOW must come from the decision cache"

            bob = connect("http/bob")
            mqtt.subscribe(bob, "allowed/#", 0)
            mqtt.publish(alice, "allowed/test", 0)
            header, message = mqtt.receive(bob)
            assert header >> 4 == 3 and message.endswith(b"test-payload"), "group delivery"
            mqtt.publish(alice, "forbidden/test", 0x87)
            mqtt.subscribe(alice, "allowed/#", 0x87)
            mqtt.publish(bob, "allowed/test", 0x87)
            print("PASS allow, group permissions and delivery", flush=True)

            nogroup = connect("http/nogroup", "anything")
            mqtt.publish(nogroup, "allowed/test", 0x87)
            mqtt.subscribe(nogroup, "allowed/#", 0x87)
            print("PASS allow with no groups grants no topic access", flush=True)

            connect("http/bad", "wrong", 0x86, client_id="denied-client")
            calls = CALLS
            connect("http/bad", "wrong", 0x86, client_id="denied-client")
            assert CALLS == calls, "a repeated DENY must come from the negative cache"
            print("PASS deny refused and cached", flush=True)

            calls = CALLS
            connect("http/invalid", "anything", 0x88)
            assert CALLS == calls + 1
            connect("http/invalid", "anything", 0x88)
            assert CALLS == calls + 2, "a malformed answer must never be cached"
            print("PASS malformed answer is a non-cached subsystem error", flush=True)

            DOWN.set()
            calls = CALLS
            connect("http/outage", "anything", 0x88)
            assert CALLS == calls + 1
            connect("http/outage", "anything", 0x88)
            assert CALLS == calls + 2, "an outage must never be cached"
            DOWN.clear()
            print("PASS outage refused as a non-cached subsystem error", flush=True)

            # The service abstains, so the user-database authenticator next in the chain decides.
            connect("http/service", LOCAL_PASSWORD)
            print("PASS not_handled continues to the next authenticator", flush=True)

            connect("http/unknown", "anything", 0x86)
            print("PASS an unrecognised account is refused, never admitted", flush=True)
        finally:
            for sock in sockets:
                sock.close()
            if process:
                process.terminate()
                process.wait(timeout=10)
    service.shutdown()
    print("HTTP AUTH ACCEPTANCE PASSED", flush=True)


if __name__ == "__main__":
    main()
