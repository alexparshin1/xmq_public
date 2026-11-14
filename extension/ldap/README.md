# LDAP authenticator — first implementation

Optional `xmq_ldap` extension. Unix builds use OpenLDAP (`libldap-dev` on Debian); Windows builds
use the Windows SDK's WinLDAP (`wldap32`) and Windows certificate APIs, so the installer has no
third-party LDAP runtime dependency. It authenticates MQTT CONNECT with a search followed by a
password bind. It maps verified directory membership onto native XMQ groups; topic Permissions stay
in the broker. It does not create local copies of directory users or change administrative login.
Windows/MSVC support is being validated against the Windows farm build.

## Build and test

On Windows, build with the XMQ Visual Studio solution/CMake configuration; WinLDAP is supplied by
the Windows SDK. LDAPS and StartTLS both require a valid server chain and matching host name. An
empty `ca_file` uses the Windows system trust store; when `ca_file` is set, it is used as the
exclusive root for that connection. The file may contain one PEM or DER CA certificate. LDAP
settings and search filters are UTF-8; WinLDAP calls use its UTF-16 API internally.

In the configured XMQ tree:

```sh
cmake .
make -j8 xmq_ldap xmq_ldap_tests
./extension/ldap/xmq_ldap_tests
python3 extension/ldap/tests/integration.py extension/ldap/xmq_ldap_tests
```

For an out-of-tree build, use the executable under that build's `extension/ldap/` directory.
The integration runner needs Docker and OpenSSL. It creates a disposable OpenLDAP container
(`osixia/openldap:1.5.0`), binds randomly allocated ports **only on 127.0.0.1**, generates a one-day
test CA and server certificate, and removes its container and temporary credentials afterwards.
It tests verified LDAPS and StartTLS, correct/incorrect passwords, empty passwords, unknown and
ambiguous users, filter escaping, invalid service credentials, bad CA, and recovery after failure.
To include a real broker and MQTT 5 CONNECT/PUBLISH/SUBSCRIBE acceptance checks:

```sh
python3 extension/ldap/tests/integration.py build/extension/ldap/xmq_ldap_tests build/xmq_server
```

This creates a separate broker, SQLite database and loopback MQTT port. It checks mapping, delivery,
refused PUBLISH/SUBSCRIBE, an unmapped user, invalid password, directory outage and local emergency
login. It never restarts the installed broker. Some existing broker CONNECT failures use legacy
reason codes even for MQTT 5; the harness accepts those as refusals.

## CONNECT benchmark, and what it says about caching

```sh
python3 extension/ldap/tests/integration.py build/extension/ldap/xmq_ldap_tests \
    build/xmq_server --benchmark
```

This provisions 1000 distinct directory users (`bench0001`…, password `bench-pass`), then performs
one MQTT 5 CONNECT per user — TCP, CONNECT, CONNACK, DISCONNECT — at a fixed concurrency, and
repeats the whole round. Each round is timed and reported as CONNECT/s:

```
LDAP CONNECT benchmark round 1: 1000 connects, concurrency 32, 1.26 s, 794.0 CONNECT/s
LDAP CONNECT benchmark round 2: 1000 connects, concurrency 32, 0.07 s, 13548.8 CONNECT/s
```

Options: `--benchmark-users N` (default 1000), `--benchmark-rounds N` (default 2),
`--benchmark-concurrency N` (default 32), `--cache-ttl N` (default 60, `0` disables the cache).
The run is isolated: its own LDAP container and its own broker on a random loopback port, so a stand
already serving on 1883 is not disturbed.

The first round is a cold cache and pays for every directory operation; the second round is served
from the decision cache. On the development host that was 794 → 13 549 CONNECT/s at concurrency 32
(about 17×), and with `--cache-ttl 0` both rounds stay near 815–833 CONNECT/s, which is what shows
the difference comes from the cache and not from a warm directory.

## Active Directory

The Extensions page offers two starting points — **Active Directory** and **LDAP** — and links each
to its value of `directory_type`, so choosing the type applies its profile at once: nested groups,
the account-state check, its attribute and mask, and the search filters, which are cleared because a
filter left over from the other profile would override it. The host, search base, search account,
CA, group mappings, transport, login prefix, timeouts and cache are left alone, and nothing is saved
until Apply. The rest of this section is what the AD profile means, for a configuration written by
hand.

Set `directory_type` to `ad` and the defaults follow what AD expects: a login without `@` is looked
up by `sAMAccountName`, a login with `@` by `userPrincipalName`, nested groups are followed through
the in-chain matching rule (`1.2.840.113556.1.4.1941`), and the disabled bit of `userAccountControl`
is checked after a successful bind.

```json
"directory_type": "ad",
"search_base": "ou=people,dc=corp,dc=example,dc=com",
"bind_dn": "cn=xmq-reader,ou=service,dc=corp,dc=example,dc=com",
"bind_password": "REPLACE-ME",
"group_mappings": "cn=MQTT Publishers,ou=groups,dc=corp,dc=example,dc=com => Publishers"
```

Each of those three behaviours has its own switch: `nested_groups` and `account_state_check` take
`auto`, `yes` or `no`, and `auto` means "the AD way for `ad`, direct membership and no state check
for `ldap`". `user_filter` and `group_filter` override the profile entirely if an installation needs
a different attribute. The state check reads `account_state_attribute` (`userAccountControl`) and
refuses an account whose flags contain `account_state_disabled_mask` (`2`, ACCOUNTDISABLE).

AD compatibility has been checked against two real AD implementations: a **Samba AD DC** and a
**Windows Server 2026** domain controller, both serving the AD schema. `extension/ldap/tests/samba_ad.py`
provisions a throwaway Samba domain and `extension/ldap/tests/windows_ad.py` runs against a controller
you already have. Both verify login by `sAMAccountName`, login by `userPrincipalName`, a mapped group
reached through a nested group by the in-chain rule, and the refusal of a disabled account.

The Samba harness needs Docker and passwordless `sudo` on the machine where the broker runs: libldap
verifies the certificate against the name it was given, so the script adds a temporary `/etc/hosts`
entry for the directory's name and removes it afterwards. The domain is created with `NOCOMPLEXITY`
and test passwords; nothing outside the throwaway container and that one hosts line is touched.

Note that AD refuses the bind of a disabled account itself, so the account-state check is a second
guard for directories that do not. The flag parsing is covered by unit tests either way.

## AD acceptance against a Windows Server domain controller

```sh
python3 extension/ldap/tests/windows_ad.py build/xmq_server \
    --host dc1.example.org --host-address 10.1.1.18 \
    --ca ca.pem \
    --bind-dn 'cn=Administrator,cn=Users,dc=example,dc=org' \
    --bind-password 'REPLACE-ME'
```

The controller needs the same fixtures the Samba harness creates: users `alice`, `bob` and a
disabled `carol` (passwords `Xmq-Accept-1!`, `Xmq-Accept-2!`, `Xmq-Accept-3!`), and the groups
`Server Monitoring` (`alice`, `bob`), `MQTT Inner` (`alice`) and `MQTT Operators`, which contains
`MQTT Inner`. `--ca` is the CA certificate that signed the controller's LDAPS certificate; a
disposable test domain can simply publish its own self-signed CA for that. The harness starts its
own broker on a free port in a temporary directory and checks the same four points as the Samba
one.

A freshly promoted controller on a private network has no LDAPS certificate at all (port 636 listens
but the handshake fails) until one is installed. On the test machine that was done with
`New-SelfSignedCertificate` for a throwaway CA, signing a server certificate with the controller's
FQDN in the subject alternative name, imported into `LocalMachine\My` and its CA into
`LocalMachine\Root`, followed by a restart of the NTDS service. Restarting the directory service by
hand has to be done in order — NTDS first, then DNS, then Netlogon — because the AD-integrated DNS
zone cannot load before the directory does; a normal reboot starts them correctly on its own, which
was confirmed on this machine after the certificate was in place.

## AD acceptance against a Samba AD DC

```sh
python3 extension/ldap/tests/samba_ad.py build/xmq_server
```

This needs Docker and passwordless `sudo` (see above). It provisions a throwaway
`nowsci/samba-domain` container, creates `alice`, `bob` and a disabled `carol`, the groups
`Server Monitoring`, `MQTT Inner` and `MQTT Operators` (with `MQTT Inner` nested inside
`MQTT Operators`), replaces Samba's temporary TLS certificate with one signed by a throwaway CA and
named `dc1.example.org`, and starts the broker with `directory_type=ad`. It then checks:

- `ldap/alice` logs in by `sAMAccountName` and publishes to the mapped topic;
- `ldap/bob@example.org` logs in by user principal name and receives it;
- `ldap/alice` may subscribe to a topic granted to `MQTT Operators`, which it reaches only through
  the nested group;
- `ldap/carol` is refused (AD refuses the bind of a disabled account; the state check is the second
  guard for directories that do not).

Because libldap verifies the certificate against the name it connects to, the script adds a
temporary `/etc/hosts` entry for `dc1.example.org` and removes it afterwards. The container is
removed on exit as well.

## Decision cache

A successful login and the groups read for it are remembered for `cache_ttl_seconds` (default 60);
a refusal is remembered for `negative_cache_ttl_seconds` (default 10), which limits password guessing
without holding a corrected password out for long. Up to `cache_max_entries` identities are kept,
least recently used evicted first.

The key is the username plus an HMAC of the password under a per-process random salt: the password
itself is not stored, a login that presents a different password is a different key and is checked
with the directory at once, and a restart shares no keys with the previous process. A directory
outage is **never** cached, so it cannot become a remembered answer in either direction.

The trade-off is deliberate and must be known: a password revoked in the directory, or a user
removed from a mapped group, keeps working until the cached entry expires — at most one TTL. The
cache cannot tell that the directory's answer changed; the lifetime is what bounds it. Set
`cache_ttl_seconds` to `0` where that window is unacceptable; every CONNECT then checks the
directory, and the cost is what the first benchmark round shows.

Measured on the Windows Server stand with 500 CONNECTs at concurrency 8: 3445 CONNECT/s on the cold
round and 11071 CONNECT/s once the cache answered, and a two-second lifetime, changed at the
directory, still held the old password for that long and then refused it.

Add `--serve` to keep the isolated stand running for MQTTX and GUI testing. The script prints its
randomly allocated loopback MQTT/Web GUI ports and test credentials. Alice (`ldap/alice`, password
`test-alice`) can publish to `allowed/#`; Bob (`ldap/bob`, `test-bob`) can subscribe; `ldap/nogroup`
(`test-nogroup`) authenticates but has no topic rights. Local GUI login: `admin` / `Test-local-admin7!`.
The GUI edits the stand's own Groups and Permissions. Ctrl+C removes the stand and test credentials.

No system package is installed by these tests, and no running XMQ configuration is modified.

## Configuration

Install the library with the normal `make install`. Add an extension configuration file before
`50-user-database.conf`, for example `xmq_extensions.d/40-ldap.conf` (use the actual absolute
installation path; `.dll`/`.dylib` instead of `.so` where appropriate):

```json
{
  "extensions": [{
    "name": "ldap",
    "library": "/home/alexeyp/.local/lib/xmq-extensions/libxmq_ldap.so",
    "enabled": true,
    "required": true,
    "settings": {
      "host": "ldap.example.org",
      "port": "636",
      "encryption": "ldaps",
      "search_base": "ou=people,dc=example,dc=org",
      "bind_dn": "cn=xmq-reader,ou=services,dc=example,dc=org",
      "bind_password": "REPLACE-ME",
      "user_filter": "(uid={username})",
      "username_prefix": "ldap/",
      "ca_file": "/path/to/directory-ca.crt",
      "timeout_seconds": "3",
      "group_filter": "(member={dn})",
      "group_mappings": "cn=mqtt-publishers,ou=groups,dc=example,dc=org => Publishers\ncn=mqtt-readers,ou=groups,dc=example,dc=org => Readers",
      "cache_ttl_seconds": "60",
      "negative_cache_ttl_seconds": "10",
      "cache_max_entries": "4096"
    }
  }]
}
```

Protect the configuration containing the service password (owner-only permissions). Use a read-only
search account limited to the user subtree, **not** the directory administrator. Empty `bind_dn`
and `bind_password` select anonymous search; they must otherwise both be set.
`host` is a bare host name; `port` empty means 636 for `ldaps` and 389 for `starttls`.
`encryption` is `ldaps` (TLS from the first byte) or `starttls` (upgrade before the password is sent;
a plain connection is refused). `ca_file` names the CA that signed the **directory's** certificate,
not this broker's own `node.crt`; empty uses the system trust store. TLS certificate and hostname
checks cannot be disabled. Referrals are disabled to avoid sending credentials to another server.

Connect as `ldap/alice` with Alice's directory password. Only `alice` is substituted into the
RFC 4515-escaped search filter. The prefix is mandatory and nonempty: it makes local versus directory
identity explicit and keeps local emergency accounts independent of directory availability.
Reserve this prefix for LDAP; do not create local accounts named `ldap/...`.

Create `Publishers` and `Readers` on the XMQ Groups page, and their policies on Permissions.
`group_mappings` is one `group DN => XMQ group name` per line (up to 64); the JSON representation
uses `\n` between lines. Every group is searched at its exact DN after successful password bind.
Default `group_filter=(member={dn})` supports `groupOfNames`; for `posixGroup` use
`(memberUid={username})`, or `(uniqueMember={dn})` for `groupOfUniqueNames`. Substitutions are escaped.
Only direct membership is implemented. Multiple matching groups supply all their rules to the
normal broker evaluator, preserving rule order and deny precedence. Local account membership is
never merged. Mapped groups with no rules and users without mapped groups deny by default, regardless
of the ordinary default-deny switch. Missing directory group DNs fail closed as configuration/store
errors. Permission edits apply to connected clients; directory membership and mapping changes apply
at the next CONNECT, or when the cached decision expires. Directory users do not appear as local
entries on the Users page.

This requires extension ABI 1.4. The broker accepts ABI 1.3 tables with no identity callback;
older brokers reject the LDAP library instead of silently ignoring native membership.

Place LDAP **before** `user-database`: the latter's default `unknown_user=deny` otherwise rejects
directory accounts before LDAP can answer. Inspect the effective extension order if extensions
are configured in more than one location. Authentication stops at the first definitive answer:

| Condition | Answer |
|---|---|
| Username outside the prefix | NOT_HANDLED (local chain continues, even during LDAP outage) |
| Unknown directory account | NOT_HANDLED (reserve the prefix to avoid local fallback collisions) |
| Empty password/username, ambiguous search, wrong user password | DENY |
| Valid password bind | ALLOW |
| Directory, TLS, service bind or search failure | SUBSYSTEM_ERROR (fail closed; no local fallback) |

Directory connectivity is checked at CONNECT, not extension startup. Network/operation timeouts
apply separately to each step, not as one total CONNECT deadline. Every attempt opens independent
search and user sessions so concurrent clients cannot change each other's bind identity. Reload
swaps a validated configuration snapshot, clears the cache, and keeps the previous settings when the
new ones are invalid. Logs report the operation and generic LDAP error only, not credentials, DNs or
server diagnostics.

## Remaining work before release

- More broker chain/ACL tests (persistent reconnects, policy reload), service-account least-privilege
  fixtures and slow-server tests.
- Samba/Microsoft AD testing, nested-group semantics, account-disabled/expired policy.
- Packaging dependency declarations on the supported platforms.

The initial auth/group/Permissions path is available for manual acceptance, not yet production-hardened.
No decisions are cached yet: every CONNECT rechecks credentials and membership, and outages never
become cached password refusals. Cache/TTL, packaging and AD coverage remain release work.
