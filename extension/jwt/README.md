# xmq_jwt — JSON Web Token authentication

Authenticates an MQTT client whose **password is a JSON Web Token** issued by the customer's
identity provider (Keycloak, Entra ID, Auth0, Okta, or anything else that speaks OIDC). No directory
round trip is made per connection: the token's signature is checked locally, and the claims say who
the client is and what it may be.

The MQTT username carries the identity. With the default `username_prefix` the client sends
`jwt/<name>` and the token must say the same `<name>` in its username claim; the token alone is the
credential. A password that is not three dot-separated base64url parts is not this extension's
business — it answers `NOT_HANDLED`, and the next authenticator in `xmq_extensions.conf` is asked.
Put `user-database` or `ldap` after it if other accounts must share the broker; a name that no
authenticator claims is refused. Token-shaped but malformed credentials are refused rather than
passed on.

## Build and runtime dependencies

XMQ 1.1.2 uses SPTK **5.6.20**, including the JWT, HTTP/TLS, URL and xdoc changes made for this
release, and OpenSSL **3.0 or newer**. SPTK is required: there is no alternate JWT backend or
`XMQ_JWT_USE_SPTK` switch. Rebuild the broker and extensions together after SPTK ABI changes;
an older build carrying the same version number is not sufficient.

The extension can be built against the installed XMQ SDK. If SPTK is not discovered automatically,
set `SPTK_INCLUDE_DIR` and `SPTK_LIBRARY_CORE` to the matching headers and shared library.
The authenticator is optional to configure, not optional in its SPTK dependency.

The companion `jwt.ui.json` groups the settings on the Extensions page: the shared secret appears
for HS256, the public key and the key-set URL for RS256 and ES256, and a hidden field keeps its
value until Apply. It follows the contract in `extension/UI.md`; without it the ordinary
ABI-generated form is used.

## Algorithms

| `algorithm` | Key | Notes |
|---|---|---|
| `RS256` | `public_key` (PEM) or `jwks_url` | The usual choice; Keycloak's default. |
| `ES256` | `public_key` (PEM) or `jwks_url` | P-256. JWT's raw `r‖s` signature is converted to DER for verification. |
| `HS256` | `secret` | Shared secret; no JWKS, no public key. |

Only the configured algorithm is accepted. A token that asks for another one is refused before the
key is looked at — accepting whatever `alg` the token names is how an attacker turns `RS256` into
`HS256` and signs with the public key as the secret.

## Settings

| Setting | Default | What it means |
|---|---|---|
| `algorithm` | `RS256` | `RS256`, `ES256` or `HS256`. |
| `secret` | — | HMAC secret for `HS256`. Secret; never shown again. |
| `public_key` | — | PEM public key for `RS256`/`ES256`. Exactly one of this and `jwks_url`. |
| `jwks_url` | — | Fetched at load/reload; refreshed on a subsequent key lookup after 5 minutes, or when an uncached token names an unknown key. |
| `jwks_ca_file` | — | CA that signed the JWKS server's certificate (not the token key). Empty uses the system store. |
| `issuer` | required | Exact `iss` every token must carry. |
| `audience` | — | `aud` a token must name. Empty accepts any audience — weaker; set it wherever the provider issues one. |
| `username_claim` | `preferred_username` | Claim with the identity; a dotted path is allowed. |
| `groups_claim` | `groups` | Claim with group/role names, a string or an array; a dotted path is allowed (`realm_access.roles` for Keycloak). |
| `username_prefix` | `jwt/` | Prefix the identity is presented with; keeps a token from standing in for a local account of the same name. Empty uses the claim as it is. |
| `group_mappings` | — | One `claim value => existing XMQ group` per line. Empty passes claim values through as group names. |
| `clock_skew_seconds` | 30 | How far `exp`/`nbf` may disagree with this host's clock. |
| `timeout_seconds` | 5 | Fetch budget shared by connect, TLS and HTTP; synchronous DNS cannot be interrupted. |
| `cache_ttl_seconds` | 60 | How long a verified token and its groups are remembered; 0 disables the cache. |
| `negative_cache_ttl_seconds` | 10 | How long a refused token is remembered; 0 disables it. |
| `cache_max_entries` | 4096 | Bound on remembered tokens, least recently used evicted first. |

## Claims

- `exp` is **required**: a token without an expiry is a password that never changes.
- A numeric `nbf`, when present and representable as `int64_t`, must not be in the future beyond
  the allowed skew. The current implementation ignores an unparseable `nbf`; do not rely on it
  to reject malformed claims.
- `iss` must equal `issuer`; `aud`, when configured, must contain `audience`.
- The identity claim must be non-empty, and the MQTT username must be `username_prefix` + that
  identity. The identity must be a JSON string; numeric values are not coerced into usernames.
- `aud` may be a string or an array of strings. Non-string group-array entries are ignored.
  Dotted paths are converted to safely quoted, child-only SPTK XPath lookups.
- `groups_claim` entries are mapped through `group_mappings`; with no mapping they become XMQ group
  names as they are. A token that maps to no group is still admitted but has no permissions —
  native Permissions fail closed.

## Caching

A verified token is remembered for `cache_ttl_seconds`, keyed by the presented identity and an
HMAC of the token under a per-process random salt. The token itself is never stored, and a restart
shares no keys with the previous process. A refusal is remembered separately and briefly. A failure
to fetch the key set is `SUBSYSTEM_ERROR` and is never cached, so an outage cannot turn into a
remembered answer.

The ALLOW cache is consulted **before signature and claim checks**, and its TTL is not capped by
`exp`. A cached token can therefore be admitted after its expiry (including clock skew), until
that decision's TTL elapses. Set `cache_ttl_seconds=0` when every CONNECT must recheck expiry.
Already connected sessions are not disconnected when their token expires.

There is **no token introspection or revocation-list check**. Revoking a token or disabling its
account at the provider does not invalidate an already issued, locally verifiable token — even
with the decision cache disabled. New groups/roles require a new token and reconnect. Reload
clears both the decision cache and the signing-key cache.

## Key set over TLS

Use `https://` outside loopback. Plain `http://` is allowed only for `localhost` and `127.0.0.1`
in the current transport. IPv6 URLs, including `::1`, are parsed by SPTK URL but explicitly rejected
by this extension because SPTK TCP connect is currently IPv4-only.

TLS requires version 1.2 or newer, verifies the chain and DNS name/IP, and uses a private context.
A custom `jwks_ca_file` replaces system trust for this connection. The HTTP client accepts status
200 only (no redirects), limits headers to 64 KiB and body to 4 MiB, and supports Content-Length
and chunked responses. It requests `Accept-Encoding: identity` and rejects compressed responses.
JSON nesting is limited to 32 levels by SPTK.

`timeout_seconds` is a shared budget for connection establishment, TLS handshake and HTTP I/O.
Time spent in DNS is charged to that budget after the synchronous resolver returns, but the DNS
call itself cannot be cancelled and can exceed the configured timeout.

During a provider outage, a cached ALLOW can still succeed. With no cached decision, a valid token
can still be verified against a fresh cached signing key. A required refresh that fails, including
an unknown `kid`, is a subsystem error and is not cached as a credential refusal.

## Testing

`tests/JwtTests.cpp` is a standalone harness: it drives the same class and host ABI as the shared
library, with keys generated in the test, and covers

- SPTK base64url decoding and imported RSA/EC JWKs (JSON/URL/XPath parser regressions live in SPTK),
- HS256, RS256 and ES256 acceptance and every refusal — tampered signature, another key, another
  secret, expired, not yet valid, missing expiry, wrong issuer, wrong audience, wrong algorithm,
- a JWKS document turned into a usable key and a token verified against it,
- the decision cache (lifetimes both ways, outage never stored, least-recently-used eviction),
- the cache key (identity and token, never the token itself, no key without entropy).

```sh
cmake --build build --target xmq_jwt_tests
LD_LIBRARY_PATH=$PWD/lib ./build/extension/jwt/xmq_jwt_tests
```

### Real-provider MQTT acceptance

`tests/keycloak.py` creates a disposable Keycloak 26.4.7 realm and an isolated broker with its own
SQLite accounts database. It binds only random loopback ports, does not use MQTT port 1883 and does
not modify the installed broker or any remote machine. Run it when local load generators are idle:
Keycloak startup and the broker still consume CPU and memory.

Build the broker, `xmq_jwt` and `xmq_user_database` against the same SPTK 5.6.20 headers and libraries
before running; rebuilding just the extension is not sufficient after an SPTK ABI change.

```sh
docker pull quay.io/keycloak/keycloak:26.4.7
LD_LIBRARY_PATH=/path/to/sptk/lib:$PWD/lib \
  python3 extension/jwt/tests/keycloak.py ./xmq_server ./lib
```

The script checks real issued RS256 tokens through MQTT 5: identity binding, nested realm roles,
allowed/refused publish and subscribe, delivery, unmapped groups, token expiry, a newly generated
signing key triggering JWKS refresh, and local administrator access during a provider outage.
Decision caching is disabled to avoid hiding verification behind a previous ALLOW. During an
outage, a still-valid token with a previously fetched signing key remains usable; an unknown `kid`
cannot be verified. This is a signing-key cache check, not a decision-cache acceptance test.

On exit the test broker and its own container are stopped. Artifacts remain in the printed
`/tmp/opencode/jwt-keycloak-*` directory for diagnosis; the realm contains disposable test
credentials, so do not publish that directory. A successful script writes `result.txt`.
This stand uses loopback HTTP for Keycloak; verified HTTPS transport is tested separately.

Accepted locally on 2026-11-15 with decision caching disabled. The script also verifies that
authentication refusal uses MQTT 5 CONNACK `0x86` and MQTT 3.1.1 return code `0x04`.
This is not a claim of acceptance against Entra ID, Auth0 or Okta, or of end-to-end decision-cache
TTL behaviour; those are separate scenarios.
