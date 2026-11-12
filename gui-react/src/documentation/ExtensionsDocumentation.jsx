import React from "react";
import {Link} from "react-router-dom";
import DocumentationPage from "./DocumentationPage";

/**
 * @brief The extension model itself: what an extension may do, how it is configured, and what the
 * broker guarantees around it.
 *
 * The LDAP authenticator is the first shipped extension a reader is likely to configure by hand, so
 * this page is the general contract and the LDAP page is the worked example.
 */
export default function ExtensionsDocumentation() {
    return <DocumentationPage page="extensions">
        <section>
            <h4 id="extensions">Extensions</h4>

            <p>
                An extension is a shared library the broker loads into its own process. It can watch
                what the broker does, decide who may connect, or name what a client may publish and
                subscribe to. Extensions are how XMQ gains an identity source such as a directory
                without the broker growing a second user table, and how accepted messages or log
                records reach an external system.
            </p>

            <p>
                Each extension declares which of five capabilities it implements. A library that
                declares none is loaded, started and stopped, and otherwise never called.
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Capability</th>
                    <th>What the broker asks it</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>observer</code></td>
                    <td>To receive broker events: connections, subscriptions, publications and the
                        like, as they happen.</td>
                </tr>
                <tr>
                    <td><code>authenticator</code></td>
                    <td>Whether a connecting client may connect. Asked on the authentication threads,
                        in configuration order, and stopping at the first allow or deny.</td>
                </tr>
                <tr>
                    <td><code>authorizer</code></td>
                    <td>Which group a client belongs to, and what that group may publish or
                        subscribe to. The answer is cached by the broker per group and topic.</td>
                </tr>
                <tr>
                    <td><code>message tap</code></td>
                    <td>To copy accepted messages matching filters the extension asked for.</td>
                </tr>
                <tr>
                    <td><code>log tap</code></td>
                    <td>To copy the broker's own log records, from a level the extension asked for.</td>
                </tr>
                </tbody>
            </table>

            <p>
                The three extensions an installation may have configured are the accounts database
                (<code>user-database</code>), the HTTP(S) webhook, and — where it was built — the LDAP
                authenticator. See <Link to="/xmq_doc_ldap">LDAP Authentication</Link> for that one.
            </p>
        </section>

        <section>
            <h4 id="extensions-configuration">Where extensions are configured</h4>

            <p>
                Extensions are described in <code>xmq_extensions.d/*.conf</code> beside the broker's
                configuration file, or in a single <code>xmq_extensions.conf</code>. Each entry names
                the library, whether the broker must have it, and the extension's own settings:
            </p>

            <pre className="userManualCode">{`{
  "extensions": [{
    "name": "ldap",
    "library": "/opt/xmq/lib/xmq-extensions/libxmq_ldap.so",
    "enabled": true,
    "required": true,
    "settings": {
      "uri": "ldaps://ldap.example.org:636"
    }
  }]
}`}</pre>

            <p>
                The <b>name</b> must match the name the library reports, or the broker refuses to load
                it: a configuration that names one extension while the broker runs another is worse
                than a configuration that fails. The file an entry came from is shown on the
                <b>Extensions</b> page, which is also where the settings are edited once the broker is
                running.
            </p>

            <p>
                <b>Order matters for authenticators.</b> They are asked in configuration order and the
                first allow or deny is final; an extension that does not recognise a client abstains,
                and the broker's own accounts answer last. A directory must therefore be listed before
                the accounts database when local accounts are the fallback rather than the authority.
            </p>

            <p>
                <b>Required</b> means the broker will not serve MQTT without it — the configuration
                interface still starts, so the mistake can be corrected there. Use it for whatever
                decides who may connect; leave an optional tap or connector not required.
            </p>
        </section>

        <section>
            <h4 id="extensions-settings">Settings and secrets</h4>

            <p>
                An extension declares its settings — name, label, type, default and the values it
                accepts — so the interface can draw a form and refuse a bad value before it reaches
                the extension. A setting marked secret is masked on screen and kept out of the log.
            </p>

            <p>
                The settings are stored in the configuration files, not in the accounts database, so
                they belong to the node. A cluster therefore needs the same extension configuration on
                every node, and a credential such as a directory service password is repeated there.
                Restrict those files to the broker's account: an extension configuration with a
                password in it is as sensitive as the password itself.
            </p>

            <p>
                Saving settings on the Extensions page asks the running extension to apply them
                immediately, without restarting MQTT. An extension that cannot apply a change says so
                and keeps what it had, rather than running half-configured.
            </p>
        </section>

        <section>
            <h4 id="extensions-limits">What an extension cannot do</h4>

            <p>
                An extension runs inside the broker: a crash in its code is a crash of the broker, and
                a call that blocks holds one of the broker's threads. Its library must be built against
                this broker's extension ABI; a library built for a newer ABI is refused at load time
                rather than called into. Extensions cannot add listeners, change the storage engine, or
                serve the configuration interface.
            </p>

            <p>
                Keeping authorisation in the broker is deliberate. An extension may say who a client
                is and which groups it belongs to; what those groups may publish or subscribe to stays
                in <Link to="/xmq_doc_permissions">Topic Permissions</Link>, so the policy is in one
                place and can be read on one screen.
            </p>
        </section>
    </DocumentationPage>;
}
