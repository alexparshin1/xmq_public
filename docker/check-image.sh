#!/bin/bash
# Refuses an XMQ image that serves files from outside its static files directory.
#
# The web interface on 18883 used to append the request path to the directory it serves, so a
# path with '..' in it read any file the process could open - and the container runs as root,
# so that was every file in it. This ran before every push since, because the fix lives in the
# .deb the image installs rather than in anything this repository builds: an image can regress
# without a single line here changing.
#
#   ./check-image.sh alexeyparshin/xmq:<version>
set -u

IMAGE=${1:?usage: check-image.sh <image>}
NAME=xmq-image-check-$$
PORT=28883

docker rm -f "$NAME" >/dev/null 2>&1
docker run -d --name "$NAME" -p "$PORT:18883" "$IMAGE" >/dev/null || exit 1
trap 'docker rm -f "$NAME" >/dev/null 2>&1' EXIT

wait_started() {
    for _ in $(seq 30); do
        [ "$(docker logs "$NAME" 2>&1 | grep -c "Server started")" -ge "$1" ] && return
        sleep 1
    done
}

# A password first, the way a user sets one: without it the configuration interface stays on the
# container's own loopback, where a published port - and so this check - cannot reach it. The
# suffix meets the rest of the password rules: the hex has only lowercase letters and digits.
wait_started 1
printf '%s' "$(head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n')Z!" \
    | docker exec -i "$NAME" xmq_server --set-password admin >/dev/null || exit 1
docker restart "$NAME" >/dev/null || exit 1
wait_started 2    # the log survives the restart, so the second start is the one to wait for

python3 - "$PORT" <<'PY'
import gzip, socket, ssl, sys

port = int(sys.argv[1])
ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE


def get(path):
    sock = ctx.wrap_socket(socket.create_connection(("127.0.0.1", port), 10))
    sock.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\n\r\n".encode())
    sock.settimeout(5)
    data = b""
    try:
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
    except Exception:
        pass
    sock.close()
    head, _, body = data.partition(b"\r\n\r\n")
    head = head.decode(errors="replace")
    if "gzip" in head:
        try:
            body = gzip.decompress(body)
        except Exception:
            pass
    return head, body


# curl normalises '..' away before it sends anything, so these have to go over a raw socket.
# More '..' than there are directories to climb: at the root they do nothing, so one count
# works whatever the static files directory turns out to be. Counting them exactly is how a
# probe ends up landing on a path that does not exist and passing for the wrong reason.
UP = "/.." * 8

escapes = {
    f"{UP}/etc/shadow": b"root:",
    f"{UP}/etc/passwd": b"root:x:0:0",
    f"{UP}/etc/xmq/certs/node.key": b"PRIVATE KEY",
    # Through a directory that really is in the image, not an invented one.
    f"/static{UP}/etc/passwd": b"root:x:0:0",
}

failed = False
for path, leaked in escapes.items():
    head, body = get(path)
    if leaked in body:
        print(f"FAILED  {path} - served a file from outside the static files directory")
        failed = True
    else:
        print(f"ok      {path} - {head.splitlines()[0] if head else 'no response'}")

# The web interface still has to work, or every check above passes for the wrong reason.
head, body = get("/index.html")
if b"<!doctype html" not in body.lower():
    print("FAILED  /index.html - the web interface is not served, so the checks prove nothing")
    failed = True
else:
    print("ok      /index.html - the web interface responds")

sys.exit(1 if failed else 0)
PY
