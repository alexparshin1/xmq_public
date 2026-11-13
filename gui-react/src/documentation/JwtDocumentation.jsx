import React from "react";
import {Link} from "react-router-dom";
import DocumentationPage from "./DocumentationPage";

/**
 * @brief JWT authentication: the token the client already holds, what the broker checks locally,
 * and when a revoked token stops working.
 *
 * The delay table is the part operators ask about first, so it is a table rather than prose: the
 * signature is checked once per cache lifetime, and the claims are what the broker acts on.
 */
export default function JwtDocumentation() {
    return <DocumentationPage page="jwt">
        <section>
            <h4 id="jwt">JWT authentication</h4>

            <p>
                The JWT authenticator lets a client sign in with a token its identity provider has
                already issued — Keycloak, Entra ID, Auth0, Okta, or anything else that speaks OIDC.
                The signature is checked locally against a configured key or the issuer's published key
                set, and the claims say who the client is and which XMQ groups it joins. No directory
                round trip is made per connection.
            </p>

            <p>
                The library ships with the broker and needs no extra dependency beyond OpenSSL. An
                installation that does not use it simply has no <code>xmq_jwt</code> entry in its
                extension configuration. Loading it, and the settings that go with it, is described on
                the <Link to="/xmq_doc_extensions">Extensions</Link> page.
            </p>
            <p>
                The library's companion <code>.ui.json</code> file organizes the form into signature,
                issuer, groups and cache sections: the shared secret appears for HS256, the public key
                and key-set URL for RS256 and ES256, and a value is kept when a field is hidden. Nothing
                is saved until Apply; if the form description is unavailable or incompatible, the
                ordinary settings form remains usable.
            </p>
        </section>

        <section>
            <h4 id="jwt-token">The token is the password</h4>

            <p>
                MQTT has no field of its own for a token, so the token goes in the password field. The
                username carries the identity, prefixed: with the default
                <code>username_prefix</code> a client connects as <code>jwt/alice</code> with the token
                as its password, and the token's username claim must say
                <code>alice</code>. A token issued for one identity cannot connect under another.
            </p>

            <p>
                The prefix keeps a token from standing in for a local account of the same name, and it
                is what lets the extension tell its clients from everybody else's. A password that is
                not three dot-separated base64url parts is not this extension's business: it answers
                <em>not handled</em> and the chain continues, so local accounts keep working beside it.
                A username without the prefix is not handled either.
            </p>

            <pre className="userManualCode">{`mosquitto_pub -h broker -p 1883 \\
    --username jwt/alice \\
    --pw "$(cat alice.jwt)" \\
    -t sensors/room1 -m hello`}</pre>

            <p>
                Always connect over TLS with a token. A password field is sent in the clear on a plain
                listener, and a token is a bearer credential until it expires.
            </p>
        </section>

        <section>
            <h4 id="jwt-algorithms">Algorithms and where the key comes from</h4>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th><code>algorithm</code></th>
                    <th>Key</th>
                    <th>Notes</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>RS256</code></td>
                    <td><code>public_key</code> or <code>jwks_url</code></td>
                    <td>The usual choice; Keycloak's default.</td>
                </tr>
                <tr>
                    <td><code>ES256</code></td>
                    <td><code>public_key</code> or <code>jwks_url</code></td>
                    <td>P-256. The raw signature is converted for verification.</td>
                </tr>
                <tr>
                    <td><code>HS256</code></td>
                    <td><code>secret</code></td>
                    <td>A shared secret; no key set, no public key.</td>
                </tr>
                </tbody>
            </table>

            <p>
                Only the configured algorithm is accepted. A token that asks for another one is refused
                before the key is looked at — accepting whatever the token names is how an attacker
                turns RS256 into HS256 and signs with the public key as the secret.
            </p>

            <p>
                With <code>public_key</code> the key is pasted into the settings and never fetched. With
                <code>jwks_url</code> the issuer's key set is fetched at load and then trusted for five
                minutes; a token naming a key the set does not hold refreshes it at once, which is how
                a provider rotates its key without the broker being reconfigured. A key set that cannot
                be fetched when the configuration is applied is an error on the Extensions page rather
                than a surprise on the first connection.
            </p>
        </section>

        <section>
            <h4 id="jwt-claims">Claims the broker checks</h4>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Claim</th>
                    <th>Rule</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>exp</code></td>
                    <td>Required. A token without an expiry is a password that never changes.</td>
                </tr>
                <tr>
                    <td><code>nbf</code></td>
                    <td>When present, must not be in the future beyond the allowed clock skew.</td>
                </tr>
                <tr>
                    <td><code>iss</code></td>
                    <td>Must equal <code>issuer</code> exactly.</td>
                </tr>
                <tr>
                    <td><code>aud</code></td>
                    <td>When <code>audience</code> is set, the token must name it. Empty accepts any
                        audience, which is weaker.</td>
                </tr>
                <tr>
                    <td><code>username_claim</code></td>
                    <td>Required and non-empty; must equal the MQTT username without the prefix.</td>
                </tr>
                <tr>
                    <td><code>groups_claim</code></td>
                    <td>A string or an array of strings. A dotted path is allowed.</td>
                </tr>
                </tbody>
            </table>

            <p>
                <code>clock_skew_seconds</code> (default 30) is how far <code>exp</code> and
                <code>nbf</code> may disagree with this host's clock. Keep the broker's clock
                synchronised anyway: skew is a tolerance, not a substitute for NTP.
            </p>

            <p>
                Claim names are configurable because providers disagree about where they put things.
                Keycloak, for example, puts the login in <code>preferred_username</code> and roles in
                <code>realm_access.roles</code>; that is a dotted path:
            </p>

            <pre className="userManualCode">{`{
  "algorithm": "RS256",
  "jwks_url": "https://keycloak.example.org/realms/xmq/protocol/openid-connect/certs",
  "issuer": "https://keycloak.example.org/realms/xmq",
  "audience": "xmq",
  "username_claim": "preferred_username",
  "groups_claim": "realm_access.roles"
}`}</pre>
        </section>

        <section>
            <h4 id="jwt-groups">Claim values to XMQ groups</h4>

            <p>
                Group names in the token are mapped to existing XMQ groups in
                <code>group_mappings</code>: the claim value, then <code>=&gt;</code>, then the name of
                a group the <b>Groups</b> page already knows.
            </p>

            <pre className="userManualCode">{`Operators => Operators
mqtt-publishers => Publishers`}</pre>

            <p>
                With no mappings the claim values are used as group names as they are. In either case
                the groups decide topic access through <Link to="/xmq_doc_permissions">Permissions</Link>
                — the token never holds a topic policy. A token that maps to no group is still admitted
                but has no permissions: native Permissions fail closed for token identities, and local
                group membership of a similarly named account is never merged in.
            </p>
        </section>

        <section>
            <h4 id="jwt-changes">How soon a change reaches XMQ</h4>

            <p>
                A verified token and its groups are remembered for <code>cache_ttl_seconds</code>
                (default 60). Refusals are remembered separately and briefly, for
                <code>negative_cache_ttl_seconds</code> (default 10), and the store is bounded by
                <code>cache_max_entries</code>, least recently used evicted first. The cache is keyed by
                the presented identity and a keyed hash of the token under a per-process random salt:
                the token itself is never stored, and a restart shares no keys with the previous
                process.
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Change at the issuer</th>
                    <th>Reaches clients</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td>Token expires</td>
                    <td>At its <code>exp</code>, but no earlier than the cached decision expires — the
                        two are checked together, so a token cannot outlive its own expiry.</td>
                </tr>
                <tr>
                    <td>Token revoked or an account disabled</td>
                    <td>When the cached decision expires, within one <code>cache_ttl_seconds</code>.
                        Set it to <code>0</code> where immediate revocation matters; the cost is a
                        signature check per CONNECT, still local and with no network.</td>
                </tr>
                <tr>
                    <td>Groups or roles changed in the token</td>
                    <td>The new token carries them at once; a session already connected keeps the
                        groups it was admitted with until it reconnects.</td>
                </tr>
                <tr>
                    <td>Provider rotates its signing key</td>
                    <td>A token naming the new key refreshes the key set at once; otherwise within
                        five minutes.</td>
                </tr>
                <tr>
                    <td>Key set endpoint unreachable</td>
                    <td>The CONNECT is refused as a subsystem error — never remembered, so an outage
                        cannot become a remembered answer.</td>
                </tr>
                </tbody>
            </table>
        </section>

        <section>
            <h4 id="jwt-tls">The key set over TLS</h4>

            <p>
                <code>jwks_url</code> must be an <code>https://</code> URL. The server's certificate is
                verified against the system trust store, or against <code>jwks_ca_file</code> when the
                issuer's key set is served by an internal CA, and the host name is checked against the
                certificate. Plain <code>http://</code> is accepted only on loopback
                (<code>localhost</code>, <code>127.0.0.1</code>, <code>::1</code>), where a local test
                stand lives and another machine cannot intercept it.
            </p>

            <p>
                <code>jwks_ca_file</code> is the CA that signed the key-set server's certificate — not
                the key that signs tokens. For RS256 and ES256 the <code>public_key</code> setting is a
                PEM <i>public</i> key; the issuer's private key never comes near the broker.
            </p>
        </section>

        <section>
            <h4 id="jwt-limitations">Current limits</h4>

            <ul>
                <li>
                    Revocation is as prompt as the cache lifetime. Tokens are checked locally and the
                    issuer is not asked per connection, so a token revoked at the provider keeps
                    working until its cached decision expires, at most one
                    <code>cache_ttl_seconds</code>.
                </li>
                <li>
                    Only HS256, RS256 and ES256 are accepted. <code>none</code> is not an algorithm
                    here, and a key set entry for another algorithm is ignored.
                </li>
                <li>
                    The token identity is the MQTT username, prefixed. Per-user subjects
                    (<code>user:&lt;name&gt;</code>) do not apply to token users today; write their
                    policies on the mapped group, or as a pattern rule.
                </li>
                <li>
                    The key-set fetch has a socket timeout (<code>timeout_seconds</code>, default 5) but
                    no separate deadline for the TLS handshake.
                </li>
                <li>
                    The extension is experimental: the settings, the cache behaviour and the
                    <code>.ui.json</code> form may change in a later release.
                </li>
            </ul>
        </section>
    </DocumentationPage>;
}
