# xmq_jwt — JSON Web Token authentication

Authenticates an MQTT client whose **password is a JSON Web Token** issued by the customer's
identity provider (Keycloak, Entra ID, Auth0, Okta, or anything else that speaks OIDC). No directory
round trip is made per connection: the token's signature is checked locally, and the claims say who
the client is and what it may be.

The MQTT username carries the identity. With the default `username_prefix` the client sends
`jwt/<name>` and the token must say the same `<name>` in its username claim; the token alone is the
credential. A password that is not three dot-separated base64url parts is not this extension's
business — it answers `NOT_HANDLED` and the chain continues, so local accounts keep working beside
it.

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
| `jwks_url` | — | The issuer's key set, fetched over verified TLS and refreshed every 5 minutes or at once when a token names a key it does not hold. |
| `jwks_ca_file` | — | CA that signed the JWKS server's certificate (not the token key). Empty uses the system store. |
| `issuer` | required | Exact `iss` every token must carry. |
| `audience` | — | `aud` a token must name. Empty accepts any audience — weaker; set it wherever the provider issues one. |
| `username_claim` | `preferred_username` | Claim with the identity; a dotted path is allowed. |
| `groups_claim` | `groups` | Claim with group/role names, a string or an array; a dotted path is allowed (`realm_access.roles` for Keycloak). |
| `username_prefix` | `jwt/` | Prefix the identity is presented with; keeps a token from standing in for a local account of the same name. Empty uses the claim as it is. |
| `group_mappings` | — | One `claim value => existing XMQ group` per line. Empty passes claim values through as group names. |
| `clock_skew_seconds` | 30 | How far `exp`/`nbf` may disagree with this host's clock. |
| `timeout_seconds` | 5 | Timeout for fetching the key set. |
| `cache_ttl_seconds` | 60 | How long a verified token and its groups are remembered; 0 disables the cache. |
| `negative_cache_ttl_seconds` | 10 | How long a refused token is remembered; 0 disables it. |
| `cache_max_entries` | 4096 | Bound on remembered tokens, least recently used evicted first. |

## Claims

- `exp` is **required**: a token without an expiry is a password that never changes.
- `nbf`, when present, must not be in the future beyond the allowed skew.
- `iss` must equal `issuer`; `aud`, when configured, must contain `audience`.
- The identity claim must be non-empty, and the MQTT username must be `username_prefix` + that
  identity.
- `groups_claim` entries are mapped through `group_mappings`; with no mapping they become XMQ group
  names as they are. A token that maps to no group is still admitted but has no permissions —
  native Permissions fail closed.

## Caching

A verified token is remembered for `cache_ttl_seconds`, keyed by the presented identity and an
HMAC of the token under a per-process random salt. The token itself is never stored, and a restart
shares no keys with the previous process. A refusal is remembered separately and briefly. A failure
to fetch the key set is `SUBSYSTEM_ERROR` and is never cached, so an outage cannot turn into a
remembered answer.

Because the signature and claims are checked once per lifetime, **a token revoked at the issuer
keeps working until its cached decision expires**, at most one `cache_ttl_seconds`. Set it to `0`
where immediate revocation matters; the cost is a signature check per CONNECT (still local, no
network).

## Key set over TLS

`jwks_url` must be `https://`. Plain `http://` is accepted only on loopback (`localhost`,
`127.0.0.1`, `::1`), where a local test stand lives and another machine cannot intercept it.

## Testing

`tests/JwtTests.cpp` is a standalone harness: it drives the same class and host ABI as the shared
library, with keys generated in the test, and covers

- base64url and JSON parsing (including refusals: unfinished documents, trailing text, nesting),
- HS256, RS256 and ES256 acceptance and every refusal — tampered signature, another key, another
  secret, expired, not yet valid, missing expiry, wrong issuer, wrong audience, wrong algorithm,
- a JWKS document turned into a usable key and a token verified against it,
- the decision cache (lifetimes both ways, outage never stored, least-recently-used eviction),
- the cache key (identity and token, never the token itself, no key without entropy).

```sh
cmake --build build --target xmq_jwt_tests
LD_LIBRARY_PATH=$PWD/lib ./build/extension/jwt/xmq_jwt_tests
```
