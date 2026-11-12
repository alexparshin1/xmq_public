import React from "react";
import {Link} from "react-router-dom";
import DocumentationPage from "./DocumentationPage";

/**
 * @brief LDAP authentication: what the directory proves, what the broker decides, and when each
 * kind of change reaches a client.
 *
 * The delay table is the part operators ask about first, so it is written as a table rather than
 * prose: membership is read at CONNECT, rules are applied as they are saved.
 */
export default function LdapDocumentation() {
    return <DocumentationPage page="ldap">
        <section>
            <h4 id="ldap">LDAP authentication</h4>

            <p>
                The LDAP authenticator lets people sign in to MQTT with the account they already have
                in OpenLDAP or Active Directory. It does two things and only two: it verifies the
                password against the directory, and it reads which directory groups the account
                belongs to. What those groups may publish or subscribe to is decided by XMQ, on the
                <Link to="/xmq_doc_permissions">Permissions</Link> page — the directory never holds a
                topic policy.
            </p>

            <p>
                The library is optional: it is built where OpenLDAP development headers are installed,
                and an installation that does not need it simply has no <code>xmq_ldap</code> entry in
                its extension configuration. Loading it, and the settings that go with it, is
                described on the <Link to="/xmq_doc_extensions">Extensions</Link> page.
            </p>
        </section>

        <section>
            <h4 id="ldap-users">Directory users and the ldap/ namespace</h4>

            <p>
                A client signs in as <code>ldap/alice</code>: the prefix, a slash, then the directory
                login. The prefix is a setting (<code>username_prefix</code>), it defaults to
                <code>ldap/</code>, and it must not be empty.
            </p>

            <p>
                The prefix exists so that a directory account and a local account can never be
                confused by accident. A local account named <code>admin</code> keeps working when the
                directory is unreachable — it is the way back in — while <code>ldap/alice</code> is only
                ever answered by the directory. A username without the prefix is not handled by this
                extension at all: the broker passes it to the remaining authenticators and finally to
                its own accounts, so a directory outage cannot lock out the local administrator.
            </p>

            <p>
                Directory accounts are <b>not</b> copied into the accounts database. They do not appear
                on the <b>Users</b> page, they have no local password, and there is nothing to keep in
                sync. Reserve the prefix for the directory: do not create local accounts whose name
                starts with <code>ldap/</code>.
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Client sends</th>
                    <th>Answered by</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>ldap/alice</code> + directory password</td>
                    <td>The directory. Group membership is read here too.</td>
                </tr>
                <tr>
                    <td><code>admin</code> + local password</td>
                    <td>The broker's own accounts, as before.</td>
                </tr>
                <tr>
                    <td><code>alice</code> without a prefix</td>
                    <td>The broker's own accounts — so it fails unless such a local account exists.</td>
                </tr>
                </tbody>
            </table>
        </section>

        <section>
            <h4 id="ldap-groups">Directory groups to XMQ groups</h4>

            <p>
                Group membership is mapped explicitly, one directory group at a time, in the
                extension's <code>group_mappings</code> setting: the distinguished name of the
                directory group, then <code>=&gt;</code>, then the name of an existing XMQ group.
            </p>

            <pre className="userManualCode">{`group_mappings:
cn=mqtt-publishers,ou=groups,dc=example,dc=org => Publishers
cn=Server Monitoring,dc=example,dc=org => Server Monitoring`}</pre>

            <p>
                In the JSON configuration file each line is separated by <code>\n</code>. Create the XMQ
                group first, on the <b>Groups</b> page; the name must match exactly. Up to 64 mappings
                are allowed.
            </p>

            <p>
                After the password has been verified, each mapped group is searched at its own
                distinguished name with <code>group_filter</code>. The default,
                <code>(member={'{'}dn{'}'})</code>, matches a <code>groupOfNames</code> entry holding the
                user's DN. Use <code>(uniqueMember={'{'}dn{'}'})</code> for
                <code>groupOfUniqueNames</code>, or <code>(memberUid={'{'}username{'}'})</code> for
                <code>posixGroup</code>. Only direct membership is read: a group nested inside another
                group does not confer its rights.
            </p>

            <p>
                A user may be mapped into several XMQ groups; all of their rules then apply, with the
                usual rule order and deny precedence. Local group membership of a local account with a
                similar name is never merged in.
            </p>

            <p>
                Directory identities are an allow-list by construction: if a user is in no mapped
                group, or the mapped groups have no rule covering the topic, the answer is deny — even
                when the ordinary default-deny setting is off. Per-user subjects
                (<code>user:&lt;name&gt;</code>) do not apply to directory users today; write their
                policies on the mapped group, or as a pattern rule.
            </p>
        </section>

        <section>
            <h4 id="ldap-permissions">A worked example</h4>

            <p>
                Directory group <code>cn=Server Monitoring,ou=groups,dc=example,dc=org</code> holds
                Alice and Bob, and is mapped to the XMQ group <code>Server Monitoring</code>. On the
                Permissions page that group gets two rules:
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Action</th>
                    <th>Topic filter</th>
                </tr>
                </thead>
                <tbody>
                <tr><td><code>publish</code></td><td><code>brokers/thinker10</code></td></tr>
                <tr><td><code>subscribe</code></td><td><code>brokers/thinker10</code></td></tr>
                </tbody>
            </table>

            <p>
                Bob connects as <code>ldap/bob</code> and subscribes to
                <code>brokers/thinker10</code>; Alice connects as <code>ldap/alice</code> and publishes
                to it. Bob receives the message. A subscription to <code>brokers/#</code> is refused,
                because a filter wider than the rule is not covered by it — give the group a rule with
                that filter if the wider subscription is intended. A refusal is written to the
                broker's log, naming the username and the filter.
            </p>
        </section>

        <section>
            <h4 id="ldap-changes">How soon XMQ sees a change</h4>

            <p>
                A verified login and the groups read for it are cached for
                <code>cache_ttl_seconds</code> (default 60), so a reconnect inside that window costs
                no directory round trip. Refusals are cached separately and only for
                <code>negative_cache_ttl_seconds</code> (default 10). Set the positive lifetime to
                <code>0</code> to check the directory on every CONNECT; that is the setting to choose
                when a revoked password must stop working immediately.
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Change</th>
                    <th>Reaches clients</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td>Password changed, or the account disabled in the directory</td>
                    <td>At the next CONNECT, or when the cached decision expires — within one
                        <code>cache_ttl_seconds</code>. A session already connected stays connected
                        until it disconnects or is closed.</td>
                </tr>
                <tr>
                    <td>Account added to, or removed from, a mapped directory group</td>
                    <td>At the next CONNECT, or within one <code>cache_ttl_seconds</code>. An existing
                        session keeps the groups it connected with.</td>
                </tr>
                <tr>
                    <td>Permission rules for a mapped XMQ group edited on the Permissions page</td>
                    <td>Immediately, including sessions already connected.</td>
                </tr>
                <tr>
                    <td>Mapped XMQ group deleted, or its name changed</td>
                    <td>Immediately: the mapping no longer names an existing group, and the identity
                        is denied until the mapping is corrected.</td>
                </tr>
                <tr>
                    <td>Extension settings: URI, search base, filter, mappings</td>
                    <td>When saved on the Extensions page — the running extension applies them
                        without a restart. Invalid settings are refused and the previous ones stay.</td>
                </tr>
                <tr>
                    <td>Directory unreachable, certificate expired, service account refused</td>
                    <td>New <code>ldap/</code> connections are refused while it lasts, except identities
                        admitted before the outage and still inside their cache lifetime. Sessions
                        already connected continue, and local accounts are unaffected.</td>
                </tr>
                </tbody>
            </table>

            <p>
                An outage is never cached, so it can never turn into a remembered allow or a remembered
                refusal. Cache keys are built from the username and a keyed hash of the password under a
                per-process salt: the password itself is not kept, and a changed password is a miss by
                construction — the previous password simply stops matching once its entry expires.
                Reloading the extension settings clears the cache.
            </p>

            <p>
                The cost can be measured rather than guessed: the test stand shipped with the extension
                has a benchmark mode that connects 1000 distinct directory users once each at a fixed
                concurrency, reports CONNECT/s, and repeats the round. A cold round pays for every
                directory operation; the second is served from the cache. On the development host that
                was 794 → 13 549 CONNECT/s, while with the cache switched off both rounds stayed near
                815–833. See <code>extension/ldap/README.md</code> in the source tree.
            </p>
        </section>

        <section>
            <h4 id="ldap-tls">TLS and the search account</h4>

            <p>
                Passwords travel in a bind, so the connection to the directory must be encrypted:
                <b>LDAPS</b> from the first byte, or <b>StartTLS</b>, which upgrades a plain connection
                before any password is sent and is refused if that upgrade fails. Certificate and
                hostname checks are mandatory. <b>Directory CA certificate</b> names the authority that
                signed the directory server's certificate — not this broker's own certificate, which
                vouches for the broker and for nothing else. Leave it empty to use the system trust
                store. Referrals are not followed, so credentials are not sent to a server nobody
                configured.
            </p>

            <p>
                Searching for the user's DN is done with a service account (<code>bind_dn</code> and
                <code>bind_password</code>), or anonymously when both are empty. Give it read access to
                the user subtree and to the mapped group entries, and nothing else — not the directory
                administrator. Each network operation has its own timeout
                (<code>timeout_seconds</code>, 1–30), so the worst case for one CONNECT is roughly that
                timeout times two plus the number of mapped groups.
            </p>

            <pre className="userManualCode">{`"settings": {
  "host": "ldap.example.org",
  "port": "636",
  "encryption": "ldaps",
  "search_base": "ou=people,dc=example,dc=org",
  "bind_dn": "cn=xmq-reader,ou=services,dc=example,dc=org",
  "bind_password": "REPLACE-ME",
  "user_filter": "(uid={username})",
  "username_prefix": "ldap/",
  "ca_file": "/etc/xmq/directory-ca.crt",
  "timeout_seconds": "3",
  "group_filter": "(member={dn})",
  "group_mappings": "cn=Server Monitoring,dc=example,dc=org => Server Monitoring",
  "cache_ttl_seconds": "60",
  "negative_cache_ttl_seconds": "10",
  "cache_max_entries": "4096"
}`}</pre>

            <p>
                In a cluster every node needs the same extension configuration, because each of them
                authenticates its own clients against the directory. Keep the file readable only by the
                broker's account: it carries the service password.
            </p>
        </section>

        <section>
            <h4 id="ldap-troubleshooting">When it does not work</h4>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Symptom</th>
                    <th>Usually</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>Authentication failed</code> for a name typed without the prefix</td>
                    <td>The client sent <code>alice</code>; send <code>ldap/alice</code>.</td>
                </tr>
                <tr>
                    <td>Connected, but nothing is delivered</td>
                    <td>The SUBSCRIBE was refused: a filter wider than the group's rule. The log says
                        so, naming the filter.</td>
                </tr>
                <tr>
                    <td>Everybody with the prefix is refused, local accounts still work</td>
                    <td>The directory, its certificate or the service account. The log names the
                        failed operation without printing credentials or DNs.</td>
                </tr>
                <tr>
                    <td>Signed in, but every topic is denied</td>
                    <td>No mapped group matched, or the mapped XMQ group has no rule for that topic.
                        Check <code>group_filter</code>, the group DN, and the Permissions page.</td>
                </tr>
                </tbody>
            </table>

            <p>
                A directory user is also a perfectly ordinary MQTT client: any MQTT 5 client, including
                the bundled command-line tools, can be used to reproduce a problem without involving
                the application that reported it.
            </p>
        </section>

        <section>
            <h4 id="ldap-limitations">Current limits</h4>

            <p>
                Direct group membership only, no nested groups. Account state beyond what the bind
                reports — disabled or expired accounts, password-expiry warnings — is not interpreted
                separately. No CRL or OCSP checking. Compatibility has been verified against OpenLDAP;
                Active Directory has not yet been certified, and its schema variants are a separate
                test. There is no provisioning of directory users into the accounts database.
            </p>
        </section>
    </DocumentationPage>;
}
