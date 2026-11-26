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
                <tr>
                    <td><code>source</code></td>
                    <td>To publish externally sourced data into a local MQTT topic, as <code>kafka_source</code> does.</td>
                </tr>
                </tbody>
            </table>

            <p>
                See the <b>Extension Docs</b> menu for the accounts database
                (<Link to="/xmq_doc_user_database">User Database Authentication</Link>), the
                <Link to="/xmq_doc_ldap">LDAP Authentication</Link> directory authenticator,
                <Link to="/xmq_doc_jwt">JWT Authentication</Link>,
                <Link to="/xmq_doc_http_auth">HTTP Authentication</Link>, and the
                <Link to="/xmq_doc_webhook">Data Integration</Link> extensions that export MQTT
                messages and broker logs to HTTP, a database, or Kafka. The standalone
                <code> kafka_source</code> extension imports Kafka records into MQTT.
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
                than a configuration that fails. Settings are edited on the page the extension
                declares for them, once the broker is running; the file an entry came from is
                reported by the control interface as its <code>source</code>.
            </p>

            <p>
                An extension declares that page as <code>data-integrations</code> or
                <code>authentication</code>; one with no settings page is managed only on
                <b>Installed Extensions</b>. The matching page lists every settings set created from
                such an extension, including one whose extension is switched off. Each row has its
                own Enable switch, independent of the extension-wide switch: a configuration runs
                only when both are on, and disabling the extension stops all of its configurations
                without losing their own choices.
            </p>

            <p>
                An entry may instead name a <b>group</b>, such as <code>"Data Integrations"</code>.
                That is the name of a menu section, and it takes precedence over the library's page:
                extensions that name the same group share one page, so an installation can arrange
                its menu without rebuilding a library. The Extensions menu follows these names as
                the broker reports them; a group with no page of its own is reached at
                <code>/configurations/&lt;group&gt;</code>. Without a group, the library's declared
                page is used exactly as before.
            </p>

            <p>
                An entry may carry an <b>extension_instance</b> (ExtensionInstance), which names one <b>settings set</b> of that
                extension. Several entries can share a name and a library when their instances
                differ — two exports to two databases, each with its own filter, sink, SQLite queue
                and dead-letter topic. The <b>Data Integrations</b> page lists them and offers <b>Add</b>
                to create another. Data integrations are rows in
                <code>xmq_extension_instance</code> in the shared SQL database, keyed to this node's
                <code>xmq_node</code> row; the name is unique within the node and can be changed without
                changing the technical instance — use <b>Rename</b> and <b>Delete</b> on its row.
                Renaming keeps the queue identity and the accumulated records; deleting stops the
                integration, removes its row and deletes the persistent delivery queue that belongs
                to it. Deletion is final. Entries without an instance remain the single
                default set, so existing configurations need no change. Two entries with the same
                name and instance are still refused.
            </p>

            <p>
                The <b>Installed Extensions</b> page lists the extensions that can be switched here,
                including libraries no entry mentions. An unmentioned library reads <b>disabled</b>,
                with the version and description it reports about itself; <b>Enable</b> writes a
                fragment of its own, <code>xmq_extensions.d/90-&lt;name&gt;.conf</code>, and loads it.
                Enabling is not starting: a configuration starts when the settings it requires are
                present. Sinks are not listed, because they are chosen inside an export's
                configuration. The accounts database is the exception: it is never offered as an
                optional switch, because a broker either configures it or does not run with accounts
                at all.
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
                Keys named by their subject — the group a rule belongs to, for example — are
                declared as a family: a name ending in <code>.*</code> describes every configured
                key with that prefix, so the form knows what each one means without the extension
                listing every group an installation may invent. Only a key that matches no
                declaration at all is marked as a likely misspelling.
            </p>

            <p>
                The settings are stored in the configuration files, not in the accounts database, so
                they belong to the node. A cluster therefore needs the same extension configuration on
                every node, and a credential such as a directory service password is repeated there.
                Restrict those files to the broker's account: an extension configuration with a
                password in it is as sensitive as the password itself.
            </p>

            <p>
                Saving settings in a configuration dialog asks the running extension to apply them
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
                The policy is kept in one place: <Link to="/xmq_doc_permissions">Topic Permissions</Link>
                shows what a group may publish and subscribe to, using the broker's own user groups.
                Its rules live in the shared database, and a Mosquitto ACL or Dynamic Security file
                is imported onto that same screen — no authorizing extension is needed to write or
                enforce them.
            </p>
        </section>
    </DocumentationPage>;
}
