# Certificate-bound MQTT identity and revocation

Experimental in XMQ 1.1.2; requires the paired SPTK 5.6.21.

TLS encryption alone does not bind MQTT's username to the client certificate. These optional
policies bind that username to a verified certificate and check certificate revocation. They
run before password authentication, session lookup or takeover, and cannot be overridden by a
successful password or another authenticator.

## Configuration

Configure the broker keys under `connections.ssl_keys`:

```json
{
  "cafile": "/etc/xmq/certs/client-ca.pem",
  "keyfile": "/etc/xmq/certs/server.key",
  "certfile": "/etc/xmq/certs/server.crt",
  "verify_depth": "3",
  "identity_field": "cn",
  "crlfile": "/etc/xmq/certs/clients.crl.pem"
}
```

`identity_field` is optional:

| Value | MQTT username must equal |
|---|---|
| omitted, empty, `off` | No certificate identity binding |
| `cn` | The subject's sole Common Name |
| `san_uri` | The sole URI Subject Alternative Name |
| `san_email` | The sole email Subject Alternative Name |

Matching is exact and case-sensitive. The selected field must occur exactly once: no fallback
from SAN to CN, no choosing the first of several values, no wildcard matching or prefix removal.
An empty MQTT username is refused. The broker verifies the client-authentication purpose,
trust chain, certificate lifetime and configured verification depth; issuer names alone do not
establish trust. Certificate names must be issued under a controlled enrollment policy.

**This does not replace password authentication.** The client still sends its ordinary MQTT
username and password/token; its authenticator must accept them. The matching username is the
subject of the existing groups and topic permissions. Unknown or disabled local accounts do not
gain access just by obtaining a certificate. No new user database or ACL engine is introduced.

**The policy is node-wide, not per listener.** Enabling either identity binding or CRL checking
requires a certificate on every MQTT connection, including WS/WSS, bridges and cluster links.
Plain TCP/WS is refused, so it cannot bypass the policy. Provision certificates for every peer
before enabling it; cluster peers using username `cluster` need a selected identity of `cluster`.
It does not affect the separate web/control service's login. With both options unset, existing
TCP, TLS, WS and WSS authentication behaves as before.

The SSL Keys page offers identity selection and a PEM CRL-bundle upload. Apply the keys there
for new connections; when editing the configuration file directly, restart the broker.
Untouched upload fields retain installed files. To remove CRL enforcement, clear `crlfile` in
the configuration and restart. Setting identity to Off alone does not disable CRL enforcement.

## CRL operation

An omitted or empty `crlfile` means **no revocation check**, not an OCSP fallback. A configured
PEM file must contain current, correctly signed CRLs for every issuer of the verified client
chain (including intermediate issuers). Full-chain CRL verification is enabled. A revoked
certificate, absent list, wrong issuer, invalid signature, expired list or unreadable file
refuses MQTT CONNECT. Each list must specify both `lastUpdate` and `nextUpdate` and be current.
There is no fail-open mode.

At each CONNECT the broker rereads the CA and CRL files and verifies the chain on a completion
worker, not a receive worker. There is no cached ALLOW that could conceal a CRL update. Distribute
the bundle to **every node**, replacing files atomically before `nextUpdate`; a missing or expired
bundle can prevent every client and cluster peer from reconnecting. Monitor this separately.
The broker neither downloads CRL distribution points nor contacts an OCSP responder.

Revocation is checked at CONNECT, not continuously. **Existing connections are not terminated
when a certificate is revoked or expires.** Disconnect affected clients operationally when
immediate revocation is needed. The same check runs on MQTT reconnect even when TLS resumes.
This is not a complete certificate lifecycle service: issuance, renewal, distribution and
immediate eviction remain operator responsibilities.

## Verification

`test/ServerTests/CertificateIdentity.cpp` generates an isolated CA, certificates and signed CRLs
at runtime. Tests cover CN/SAN matching, username substitution, ambiguous/missing identities,
expired/untrusted certificates, missing/malformed/expired CRLs and CRL replacement. MQTT tests
exercise real TLS connections, password authentication, plaintext/missing-certificate refusal
and revocation on reconnect.

Run:

```sh
./test/xmq_unit_tests --gtest_filter='CertificateIdentityTests.*'
```
