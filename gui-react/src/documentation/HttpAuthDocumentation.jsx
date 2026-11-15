import React from "react";
import {Link} from "react-router-dom";
import DocumentationPage from "./DocumentationPage";

/**
 * @brief HTTP authentication: the broker asks a company service for a decision, and the answer
 * supplies the groups that native permissions use.
 *
 * The request/response contract is the part an integrator needs first, so it comes before the
 * settings; the cache table is what an operator asks about when an account is disabled.
 */
export default function HttpAuthDocumentation() {
    return <DocumentationPage page="http-auth">
        <section>
            <h4 id="http-auth">HTTP authentication</h4>

            <p>
                The HTTP authenticator asks a service of yours whether an MQTT client may connect,
                and takes the client's groups from the answer. The accounts, the business rules and
                the state of each device stay in that service; XMQ keeps the MQTT topic permissions
                in <Link to="/xmq_doc_permissions">Permissions</Link> and never asks the service
                about a topic.
            </p>

            <p>
                The library ships with the broker and needs no directory or vendor SDK, only HTTPS.
                An installation that does not use it has no <code>xmq_http_auth</code> entry in its
                extension configuration. Loading it, and the settings that go with it, is described
                on the <Link to="/xmq_doc_extensions">Extensions</Link> page.
            </p>

            <p>
                A common arrangement is HTTP authentication first and
                <code>user-database</code> or <code>ldap</code> after it: the HTTP users are handled
                by the service, and the accounts that must keep working when the service is down are
                owned by the next authenticator. A named client that no authenticator claims is
                refused.
            </p>
        </section>

        <section>
            <h4 id="http-auth-request">What the service receives</h4>

            <p>
                With the default <code>username_prefix</code> of <code>http/</code>, a client
                connects as <code>http/alice</code> and the prefix is stripped before the request is
                sent. A username outside the prefix is not handled, so it continues down the chain.
            </p>

            <pre className="userManualCode">{`POST /mqtt/authenticate HTTP/1.1
Content-Type: application/json
Accept: application/json

{
  "username": "alice",
  "password": "secret",
  "client_id": "sensor-room-7",
  "remote_address": "10.1.1.25",
  "protocol_version": 5,
  "encrypted": true
}`}</pre>

            <p>
                <code>protocol_version</code> is 3, 4 or 5; <code>encrypted</code> says whether MQTT
                itself runs over TLS. <code>remote_address</code> is the peer address, not a
                forwarded header. If <code>username_prefix</code> is empty, every client with a
                username is sent to the service; clients without one are left to the broker's
                anonymous policy.
            </p>
        </section>

        <section>
            <h4 id="http-auth-answers">What the answer means</h4>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Answer</th>
                    <th>Effect</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>{`{"decision": "allow", "groups": ["Sensors"]}`}</code></td>
                    <td>Admit the client with those groups. <code>groups</code> may be missing or
                        empty, but the client then has no topic access.</td>
                </tr>
                <tr>
                    <td><code>{`{"decision": "deny"}`}</code>, HTTP <code>401</code> or <code>403</code></td>
                    <td>Refuse the credentials. Remembered for the negative lifetime.</td>
                </tr>
                <tr>
                    <td><code>{`{"decision": "not_handled"}`}</code></td>
                    <td>Ask the next authenticator. Never remembered.</td>
                </tr>
                <tr>
                    <td>Any other status, malformed JSON, unknown decision</td>
                    <td>Refuse as <em>server unavailable</em>, never as a wrong password and never
                        as an admission.</td>
                </tr>
                </tbody>
            </table>

            <p>
                A malformed answer cannot become an accidental allow: a missing or unknown
                <code>decision</code>, a body that is not a JSON object, or a <code>groups</code>
                value that is not an array of strings is a subsystem error. MQTT 5 clients see
                CONNACK <code>0x86</code> for a refused credential and <code>0x88</code> for an
                unavailable service; MQTT 3.x clients see <code>0x04</code> and <code>0x03</code>.
            </p>
        </section>

        <section>
            <h4 id="http-auth-groups">Groups, not topic rules</h4>

            <p>
                The names in <code>groups</code> are used as they are and must already exist as XMQ
                groups. A service that returns <code>Sensors</code> gives the client exactly the
                permissions of a local account in <code>Sensors</code>; the extension never
                synthesizes an ACL rule. Duplicate names are collapsed, at most 256 groups of at most
                256 characters each are accepted, and a group that matches no rule grants nothing.
            </p>

            <p>
                Keep the mapping in the service if it should not depend on the broker's group names:
                the service is free to translate its own roles into XMQ groups before answering.
            </p>
        </section>

        <section>
            <h4 id="http-auth-cache">Caching and when a change applies</h4>

            <p>
                An ALLOW is remembered for <code>cache_ttl_seconds</code> (default 60), a DENY for
                <code>negative_cache_ttl_seconds</code> (default 10), and the store is bounded by
                <code>cache_max_entries</code>, least recently used first out. The key is a keyed
                hash of the exact request under a per-process random salt, so the identity, password,
                client id and peer address all have to match; neither passwords nor request bodies
                are stored.
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Change at the service</th>
                    <th>Reaches clients</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td>An account or password is disabled</td>
                    <td>When the cached ALLOW expires (at most one <code>cache_ttl_seconds</code>)
                        and the client reconnects. Set it to <code>0</code> when every CONNECT must
                        ask the service.</td>
                </tr>
                <tr>
                    <td>Groups change</td>
                    <td>With the next token-free recheck: after the cached ALLOW expires, on
                        reconnect.</td>
                </tr>
                <tr>
                    <td>The service is unavailable</td>
                    <td>A cached answer can still be used. Without one, the CONNECT is refused as
                        server unavailable; the outage is never remembered. That is checked by the
                        bundled stand.</td>
                </tr>
                <tr>
                    <td>The extension configuration is applied</td>
                    <td>At once: reload replaces both the configuration and the decision cache.</td>
                </tr>
                </tbody>
            </table>

            <p>
                An already connected session is not disconnected when a cached decision would have
                expired or an account is disabled at the service. Authentication decides at CONNECT;
                cutting a live session needs a separate policy.
            </p>
        </section>

        <section>
            <h4 id="http-auth-tls">TLS and limits</h4>

            <p>
                The endpoint must be <code>https://</code> outside loopback. Plain <code>http://</code>
                is accepted only for <code>localhost</code> and <code>127.0.0.1</code>, for a stand on
                the same machine; IPv6 endpoints, including <code>::1</code>, are currently
                rejected. HTTPS verifies the chain and the host name, requires TLS 1.2 or newer and
                uses a private context. <code>ca_file</code> replaces system trust for this endpoint
                and is how a corporate CA is trusted.
            </p>

            <p>
                <code>timeout_seconds</code> (default 5) is one budget for connection establishment,
                the TLS handshake and HTTP I/O. Synchronous DNS resolution cannot be interrupted and
                can exceed it. Responses are requested uncompressed: only HTTP 200 is accepted, the
                body is limited to 64 KiB and headers to 16 KiB, and redirects are not followed.
                <code>max_inflight</code> (default 16) bounds concurrent uncached requests; excess
                CONNECTs are refused as server unavailable rather than queued without limit.
            </p>
        </section>

        <section>
            <h4 id="http-auth-limits">Known limits</h4>

            <ul>
                <li>
                    Authentication only. There is no HTTP request per PUBLISH or SUBSCRIBE; topic
                    rules stay in XMQ's own Permissions, which is what keeps the message path free of
                    network calls.
                </li>
                <li>
                    No introspection or revocation list: the broker trusts the service's answer for
                    the cache lifetime and does not ask again per message.
                </li>
                <li>
                    No retries and no background refresh. A request either answers within the
                    deadline or the CONNECT fails.
                </li>
                <li>
                    The cache is per process: two nodes decide independently until their entries
                    expire. A clustered deployment that needs one answer for all nodes should keep
                    <code>cache_ttl_seconds</code> short or set it to <code>0</code>.
                </li>
                <li>
                    <code>not_handled</code> is not anonymous access. Anonymous access remains the
                    broker's own setting and applies to clients that send no username.
                </li>
            </ul>
        </section>
    </DocumentationPage>;
}
