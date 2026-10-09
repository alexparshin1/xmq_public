#!/usr/bin/env python3
"""
One node's door to the shared Redis.

A cluster node has to be able to lose the shared storage - that is what cluster-offline means -
while its neighbours keep theirs. Cutting a node off with a firewall rule would cut the neighbour
too, since both point at the same host and port. So each node reaches Redis through its own proxy
on localhost, and cutting a node means killing that proxy: the node loses the storage, the other
node does not notice, and restoring it puts the node back on the same endpoint it had.

    redis_proxy.py <listen-port> <target-host> <target-port>

Runs in the foreground; SIGTERM closes it and with it every connection it holds.
"""

import selectors
import signal
import socket
import sys

BUFFER = 65536


def pump(selector, sock, peer):
    """Move whatever arrived on one side to the other, and close both when either ends."""
    try:
        data = sock.recv(BUFFER)
    except OSError:
        data = b""
    if not data:
        for registered in (sock, peer):
            try:
                selector.unregister(registered)
            except KeyError:
                pass
            registered.close()
        return
    try:
        peer.sendall(data)
    except OSError:
        pass


def main():
    listen_port, target_host, target_port = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", listen_port))
    listener.listen(64)
    listener.setblocking(False)

    selector = selectors.DefaultSelector()
    selector.register(listener, selectors.EVENT_READ, None)

    # The connections are handed to the peer only once both are known, so a half-open pair is kept
    # out of the selector rather than pumped from a socket that has nowhere to go yet.
    pending = {}

    def stop(*_):
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    while True:
        for key, _ in selector.select():
            sock = key.fileobj
            if sock is listener:
                client, _ = listener.accept()
                client.setblocking(False)
                try:
                    upstream = socket.create_connection((target_host, target_port))
                except OSError:
                    client.close()
                    continue
                upstream.setblocking(False)
                pending[client] = upstream
                pending[upstream] = client
                selector.register(client, selectors.EVENT_READ, upstream)
                selector.register(upstream, selectors.EVENT_READ, client)
                continue
            pump(selector, sock, key.data)
            peer = pending.pop(sock, None)
            if peer is not None:
                pending.pop(peer, None)


if __name__ == "__main__":
    main()
