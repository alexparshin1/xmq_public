#!/usr/bin/env python3
"""
A Redis read for the stand's own use, with no client library to install.

    redis_query.py <host> <port> <db> <command> [args...]

Prints the reply: a string as it is, a number as a number, an array one element per line (nested
ones as JSON), and an error to stderr with a non-zero exit status. Enough for the stand's checks -
what the cluster wrote where - and for the same checks from a shell, without a C++ tool in the way:

    redis_query.py redis_server 6379 4 KEYS 'cluster:*'
    redis_query.py redis_server 6379 4 GET cluster:coordinator
"""

import json
import socket
import sys

BULK = b"$"
ARRAY = b"*"
INTEGER = b":"
STATUS = b"+"
ERROR = b"-"


class Connection:
    def __init__(self, host, port, db):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.buffer = b""
        self.command("SELECT", str(db))

    def command(self, *args):
        payload = [b"*%d\r\n" % len(args)]
        for arg in args:
            encoded = arg.encode() if isinstance(arg, str) else arg
            payload.append(b"$%d\r\n%s\r\n" % (len(encoded), encoded))
        self.sock.sendall(b"".join(payload))
        return self.reply()

    def read_line(self):
        while b"\r\n" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("redis closed the connection")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\r\n", 1)
        return line

    def read_exactly(self, count):
        while len(self.buffer) < count + 2:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("redis closed the connection")
            self.buffer += chunk
        data, self.buffer = self.buffer[:count], self.buffer[count + 2:]
        return data

    def reply(self):
        line = self.read_line()
        kind, rest = line[:1], line[1:]
        if kind == STATUS:
            return rest.decode()
        if kind == ERROR:
            raise RuntimeError(rest.decode())
        if kind == INTEGER:
            return int(rest)
        if kind == BULK:
            count = int(rest)
            return None if count < 0 else self.read_exactly(count).decode("utf-8", "replace")
        if kind == ARRAY:
            count = int(rest)
            return None if count < 0 else [self.reply() for _ in range(count)]
        raise RuntimeError(f"unexpected reply: {line!r}")


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__.strip())
    host, port, db = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    connection = Connection(host, port, int(db))
    try:
        reply = connection.command(*sys.argv[4:])
    except RuntimeError as error:
        print(error, file=sys.stderr)
        sys.exit(1)

    if isinstance(reply, list):
        for element in reply:
            print(json.dumps(element) if isinstance(element, (list, dict)) else element)
    elif reply is not None:
        print(reply)


if __name__ == "__main__":
    main()
