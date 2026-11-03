import React from "react";
import DocumentationPage from "./DocumentationPage";

export default function PermissionsDocumentation() {
    return <DocumentationPage page="permissions">
            <section>
            <h4 id="permissions">Topic permissions</h4>

            <p>
                Topic permissions answer one question: may this account publish or subscribe to
                this topic? They are kept in the accounts database with the accounts and groups, so
                every node of a cluster applies the same rules, and they are edited on the
                <b>Permissions</b> page in the configuration interface.
            </p>

            <p>
                A rule has four parts: the group it applies to, the direction (<code>publish</code>
                or <code>subscribe</code>), whether it allows or denies, and a topic filter. The
                group is one of:
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Group</th>
                    <th>Applies to</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>user:&lt;name&gt;</code></td>
                    <td>That one account, by its username.</td>
                </tr>
                <tr>
                    <td><code>group:&lt;name&gt;</code></td>
                    <td>Every account that belongs to the group on the <b>Users</b> page. The
                        <code>group:</code> prefix is how the interface shows it, so a group cannot
                        be mistaken for a username; the stored rule keeps the bare name, and so does
                        an imported rule.</td>
                </tr>
                <tr>
                    <td><code>anonymous</code></td>
                    <td>Clients that connect without a username.</td>
                </tr>
                </tbody>
            </table>

            <p>
                Rules are checked in order and the first matching rule decides, as in Mosquitto. A
                client is resolved to <code>user:&lt;name&gt;</code>, then to the groups of its
                account, and a pattern rule from an import applies to every client; the
                <code>anonymous</code> subject applies only when there is no username. When a client
                has rules but none of them covers the topic, the answer is deny - Mosquitto's
                default for a client the rules know about.
            </p>

            <p>
                A rule added here is checked before imported ones, and a manual deny before a manual
                allow, so a manual deny still wins over a manual allow. An import keeps the order
                Mosquitto would check its rules in: the client's roles first, then its groups by
                priority, then the roles in each group, with each role's ACLs by their priority.
            </p>

            <p>
                <b>Clients with no matching rule.</b> Mosquitto denies them when an ACL file is
                present. XMQ has the same choice as a setting on this page, and an import that
                brings at least one rule turns it on. An import of an empty file leaves the setting
                as it was: opening or locking the whole broker on no evidence would both be worse
                than leaving it alone.
            </p>

            <p>
                <b>Subscribe</b> is also permission to receive what is published on that filter.
                There is no separate delivery permission.
            </p>

            <p>
                A rule added on the page names a subject that already exists: the picker is a combo
                box of the groups and accounts the server knows, so a name that does not exist
                cannot be entered at all. An imported rule may name an account that has not been
                created yet - a file can describe rules before the accounts they belong to - and
                such a rule is shown like any other.
            </p>

            <p>
                The <b>Test access</b> panel answers a specific question without changing anything,
                using the same evaluation the broker applies, and names the rule that decided.
            </p>

            <h4 id="permissions-import">Importing rules</h4>

            <p>
                The <b>Import from Mosquitto</b> page takes the password file and the ACL file as
                file inputs, read by the browser and sent as content: the XMQ server does not open
                <code>/etc/mosquitto</code> with its own privileges, and cannot be used to read a
                file the administrator could not. Either file may be left empty.

                The import reads a Mosquitto <code>acl_file</code> or a
                <code>dynamic-security.json</code>. It is planned before it is applied: rules
                already present are recognised and not duplicated, a rule an existing rule already
                covers is not added, and an allow that overlaps a deny is a conflict. Nothing is
                stored while a conflict is unresolved; the screen asks whether to keep the existing
                rule, use the imported one, or skip the imported one, and an import that is refused
                leaves the rule set as it was. Importing the same file twice adds nothing.
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Mosquitto</th>
                    <th>XMQ rule</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>topic read F</code></td>
                    <td><code>subscribe</code> allow on F</td>
                </tr>
                <tr>
                    <td><code>topic write F</code></td>
                    <td><code>publish</code> allow on F</td>
                </tr>
                <tr>
                    <td><code>topic readwrite F</code></td>
                    <td><code>publish</code> and <code>subscribe</code> allow on F</td>
                </tr>
                <tr>
                    <td><code>topic deny F</code></td>
                    <td><code>publish</code> and <code>subscribe</code> deny on F</td>
                </tr>
                <tr>
                    <td><code>pattern ... %u</code> / <code>%c</code></td>
                    <td>One rule for every client, with the username or client id substituted when
                        the rule is checked</td>
                </tr>
                </tbody>
            </table>

            <p>
                The password file understands Mosquitto 2.0 <code>$7$</code> verifiers and
                clear-text lines; 1.6 <code>$6$</code> and 2.1 <code>argon2id</code> verifiers are
                reported as not imported, because they cannot be checked here and would otherwise
                become accounts nobody can sign in to. An imported <code>$7$</code> verifier is
                replaced with this broker's own at the first successful login.

                Dynamic Security roles are flattened onto accounts: the rules a client gets through
                its roles and groups become rules on <code>user:&lt;name&gt;</code>, in the order
                Mosquitto would check them. An allowing <code>defaultACLAccess</code> becomes a
                <code>#</code> rule checked after everything else; a denying one needs no rule,
                because no match is already a deny. Rules that cannot be represented are listed as
                not imported, with the reason, rather than dropped silently.
            </p>

            <h4 id="permissions-limitations">Known limitations</h4>

            <ul>
                <li>
                    <b>Per-listener rules are not available.</b> A rule applies to every MQTT and
                    MQTT+SSL listener on the node. Separating what one listener permits from another
                    has to be done with separate accounts or separate clusters for now.
                </li>
                <li>
                    <b>Read is subscribe.</b> Mosquitto's <code>read</code> and XMQ's
                    <code>subscribe</code> are the same permission, and XMQ has no separate
                    permission for receiving a message that only the delivery step would need. A
                    Dynamic Security <code>publishClientReceive</code> allow is therefore redundant
                    and is reported as not imported; a denial is carried as a subscribe deny.
                </li>
                <li>
                    <b>A subscription is matched as a filter.</b> Dynamic Security distinguishes
                    <code>subscribeLiteral</code> from <code>subscribePattern</code>; both are stored
                    as a filter here. For ordinary clients the two are the same, but a rule written
                    to permit only one literal subscription also permits the equivalent wildcard.
                </li>
                <li>
                    <b><code>%c</code> is only as good as the client id.</b> A client id is chosen by
                    the client unless it is bound by authentication (a Dynamic Security
                    <code>clientid</code>, <code>use_username_as_clientid</code>, or unique
                    credentials per device). A client id or username containing <code>+</code>,
                    <code>#</code> or <code>/</code> matches no pattern rule, so it cannot widen one.
                </li>
                <li>
                    <b>An empty topic level is refused.</b> MQTT allows a topic such as
                    <code>/finance</code>, but a rule filter that names an empty level is treated
                    as a typo by the control API and by the import alike. <code>+</code> and
                    <code>#</code> must each be a whole level, and <code>#</code> the last one.
                </li>
            </ul>
            </section>
        </DocumentationPage>;
}
