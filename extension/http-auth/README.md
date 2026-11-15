# xmq_http_auth — HTTP authentication

Asks an HTTP service whether an MQTT client may connect, and takes the client's groups from the
answer. The service owns the accounts, the policies and the business rules; XMQ owns the MQTT topic
permissions. Nothing about the user database has to be known to the broker.

With the default `username_prefix` only `http/<name>` is handled. The prefix is stripped before the
request is sent. A username outside the prefix, or an empty one, answers `NOT_HANDLED`, and the
next authenticator in `xmq_extensions.conf` is asked. Put a store such as `user-database` or `ldap`
after this one if service accounts must share the broker with the HTTP users; a name nobody claims
is refused, never admitted by accident.

## Request

```http
POST /mqtt/authenticate HTTP/1.1
Content-Type: application/json
Accept: application/json
Accept-Encoding: identity

{
  "username": "alice",
  "password": "secret",
  "client_id": "sensor-room-7",
  "remote_address": "10.1.1.25",
  "protocol_version": 5,
  "encrypted": true
}
```

`username` is the MQTT username after the prefix. `password` is what the client sent and may be
empty. `remote_address` is the peer address, never a forwarded header. `protocol_version` is 3, 4
or 5. `encrypted` is true when MQTT itself runs over TLS.

When `username_prefix` is empty, every client with a non-empty username is sent to the service;
clients that send no username are still left to the rest of the chain.

## Response

A `200` response with a JSON object and a top-level `decision`:

```json
{"decision": "allow", "groups": ["Sensors", "Room7"]}
```

| Answer | Meaning |
|---|---|
| `{"decision": "allow", "groups": [...]}` | The client is admitted. `groups` may be absent or empty: the client then has no topic access under native permissions, because nothing maps it to a group. |
| `{"decision": "deny"}` | The credentials are refused. Cached for the negative lifetime. |
| `{"decision": "not_handled"}` | Abstain: the next authenticator in configuration order is asked. A named client that no authenticator claims is refused. Never cached. |
| `401` or `403` | Same as `deny`. |
| Any other status, a non-JSON body, or a malformed object | Subsystem error: the CONNECT is refused as server unavailable. Never cached. |

Anything other than `allow`, `deny` or `not_handled` in `decision`, a non-object JSON body, or a
`groups` value that is not an array of strings is treated as a malformed answer — a subsystem error,
never an accidental allow. A group name is used as-is; the service must return the names of groups
that exist in XMQ's permissions. Duplicate names are collapsed. At most 256 groups of at most 256
characters each are accepted.

Group membership is a **group name**, not a topic ACL: `Sensors` then publishes and subscribes
exactly as a local account in `Sensors` does. Authorization stays in XMQ, and the service never
sees a topic.

## Settings

| Name | Default | Meaning |
|---|---|---|
| `url` | — | Endpoint. Required. `https://` outside loopback. |
| `ca_file` | — | CA bundle for this endpoint. Empty uses system trust. |
| `bearer_token` | — | Optional `Authorization: Bearer` credential for the service itself. Hidden in the form. |
| `username_prefix` | `http/` | Only usernames starting with this are handled; it is stripped in the request. Empty handles every named client. |
| `timeout_seconds` | 5 | Budget shared by connect, TLS handshake and HTTP I/O. |
| `max_inflight` | 16 | Concurrent uncached requests. Excess CONNECTs fail as subsystem errors. |
| `cache_ttl_seconds` | 60 | ALLOW decision lifetime. `0` disables the allow cache. |
| `negative_cache_ttl_seconds` | 10 | DENY decision lifetime. `0` disables the deny cache. |
| `cache_max_entries` | 4096 | Bounded LRU store; least recently used decisions are evicted. |

Presentation lives in `http-auth.ui.json`, following `extension/UI.md`.

## Transport

The endpoint is parsed with the SPTK URL parser. Plain `http://` is accepted only for `localhost`
and `127.0.0.1`; IPv6 URLs, including `::1`, are rejected because the SPTK TCP connect path is
currently IPv4-only. Endpoints with userinfo (`user:password@host`) are rejected, so credentials
belong in `bearer_token`.

HTTPS verifies the certificate chain and the DNS name/IP, requires TLS 1.2 or newer, and uses a
private SSL context. A configured `ca_file` replaces system trust for this connection only.

The client accepts status `200` only: redirects are not followed. Headers are limited to 16 KiB
and the body to 64 KiB. Chunked and Content-Length responses are supported, responses are requested
with `Accept-Encoding: identity`, and a compressed answer is rejected.

`timeout_seconds` covers connection establishment, the TLS handshake and HTTP I/O through a single
deadline. Synchronous DNS resolution cannot be interrupted and can exceed that budget.

## Cache

The cache key is `HMAC-SHA256(per-process random salt, request)` over the exact JSON body, so the
identity, the password, the client id and the peer address are all part of it. Neither plaintext
credentials nor the raw request are stored, and a restart starts from an empty cache with a new
salt. Reload replaces the cache and the configuration atomically; in-flight requests finish under
the old configuration.

Only `allow` and `deny` answers are stored. A subsystem error, a timeout, an exhausted
`max_inflight`, a `503` or `not_handled` is never remembered, so an outage cannot turn into a
remembered refusal or admission. This is checked by both the unit and network tests.

Because the decision is remembered, disabling an account in the service takes effect when the
cached ALLOW expires (at most `cache_ttl_seconds`) and the client reconnects. An already connected
MQTT session is not disconnected by this extension. Set `cache_ttl_seconds=0` when every CONNECT
must ask the service; that trades a network round trip per connection for prompt enforcement.

## Failure behaviour

A CONNECT whose uncached decision cannot be obtained is refused as server unavailable: MQTT 5
CONNACK `0x88`, MQTT 3.x return code `0x03`. The broker does not silently admit it, and it does not
fall through to another authenticator — "could not check" has one safe reading.

A refused credential answers `0x86` (`0x04` on MQTT 3.x). The broker's own administrator account is
a web-interface account; it is not an MQTT account here and is not affected by an HTTP outage. MQTT
service accounts are available only when another authenticator in the chain owns them.

Exceptions from HTTP/TLS/JSON handling are logged as one generic warning without the endpoint,
body or credentials, because parser and TLS diagnostics can include them.

## Build

Requires XMQ 1.1.2 or newer with SPTK 5.6.20 (JWT, HTTP/TLS, URL and xdoc) and OpenSSL 3 or newer.
The extension links SPTK in addition to OpenSSL; a `libxmq_http_auth` built against another SPTK
ABI must be rebuilt together with the broker.

```sh
cmake -S extension/http-auth -B build-http-auth \
      -DCMAKE_PREFIX_PATH="$HOME/.local" -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-http-auth
cmake --install build-http-auth
```

## Tests

`tests/HttpAuthTests.cpp` is both a unit suite and a network client.

```sh
./xmq_http_auth_tests                              # parsing, prefix, cache, reload, configuration
./xmq_http_auth_tests http://127.0.0.1:PORT/auth   # real HTTP POST, deny, 503, malformed, timeout
./xmq_http_auth_tests https://127.0.0.1:PORT/auth cert.pem  # verified TLS with a private CA
```

The network mode expects a service that answers `allow` with group `Readers` for
`alice`/`secret`, `deny` otherwise, `403` for `bad`, `503` for `outage`, malformed JSON for
`invalid`, and sleeps past the timeout for `slow`; `tests/http-auth-server.py` is a stand-in and
takes an optional certificate and key to serve HTTPS. Without a CA argument, an HTTPS endpoint is
expected to be refused as untrusted; with one, the whole suite runs over verified TLS.

`tests/stand.py` runs the whole broker on a free port with this extension, `user-database` and
native group permissions, and proves: group ACLs and delivery, a group-less ALLOW getting no topic
access, positive and negative caching, non-cached malformed answers and outages, `not_handled`
continuing to the next authenticator, and an unrecognised name being refused.

```sh
LD_LIBRARY_PATH=/path/to/sptk/lib python3 extension/http-auth/tests/stand.py ./xmq_server ./lib
```

Accepted locally on 2026-11-15 against the stand-in service over HTTP and over verified TLS; it is
not a claim of acceptance against a particular vendor product or of two-node cache behaviour.

## Limits

- Authentication only; there is no per-publish/per-subscribe HTTP authorization.
- No introspection, revocation list or token validation: the service decides with whatever it
  receives, and the broker trusts a valid answer.
- No retries and no background refresh; a request either answers within the deadline or fails.
- No IPv6 endpoint, no URL credentials, no redirects and no compressed responses.
- The cache is per process, so two nodes decide independently until entries expire.
- `not_handled` does not mean "admit anonymously". The broker's anonymous policy is a separate
  broker setting and applies to clients that send no username.
- Groups returned must already exist as XMQ groups with the intended topic permissions.
