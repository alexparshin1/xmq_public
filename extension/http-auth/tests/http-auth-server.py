#!/usr/bin/env python3
"""Mock HTTP authentication service for the xmq_http_auth network test."""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        try:
            request = json.loads(body)
        except ValueError:
            request = {}
        name = request.get("username", "")
        if name == "alice" and request.get("password") == "secret":
            payload = json.dumps({"decision": "allow", "groups": ["Readers"]}).encode()
            self.reply(200, payload)
        elif name == "bad":
            self.reply(403, b"")
        elif name == "outage":
            self.reply(503, b"")
        elif name == "invalid":
            self.reply(200, b"{not json")
        elif name == "slow":
            time.sleep(5)
            self.reply(200, b'{"decision":"allow"}')
        else:
            self.reply(200, b'{"decision":"deny"}')

    def reply(self, status, payload):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        if payload:
            self.wfile.write(payload)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    if len(sys.argv) > 3:
        import ssl
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(sys.argv[2], sys.argv[3])
        server.socket = context.wrap_socket(server.socket, server_side=True)
    print(server.server_address[1], flush=True)
    server.serve_forever()
