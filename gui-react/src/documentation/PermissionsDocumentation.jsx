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
                A group that has any rule is an allow-list: a topic no rule in that group covers is
                refused there, and the first group with rules that the client resolves to decides.
                A deny always wins over an allow, whichever came first. A client is resolved to
                <code>user:&lt;name&gt;</code> first, then to the groups of its account, then to
                <code>anonymous</code>.
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
                    <td><code>pattern ... %u</code></td>
                    <td>The same rule with the username substituted, per account</td>
                </tr>
                </tbody>
            </table>

            <p>
                Dynamic Security roles are flattened onto accounts: the rules a client gets through
                its roles and groups become rules on <code>user:&lt;name&gt;</code>. The
                <code>defaultACLAccess</code> settings are carried as <code>#</code> rules for an
                account that has no rule of its own in that direction. Rules that cannot be
                represented are listed as not imported, with the reason, rather than dropped
                silently.
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
                    <b>Dynamic Security priorities are flattened.</b> Mosquitto applies the first
                    matching rule in order; XMQ applies deny-wins. An imported rule set can
                    therefore be stricter than the file it came from, and the import says so.
                </li>
                <li>
                    <b>An empty topic level is refused.</b> MQTT allows a topic such as
                    <code>/finance</code>, but a rule filter that names an empty level is treated
                    as a typo by the control API and by the import alike. <code>+</code> and
                    <code>#</code> must each be a whole level, and <code>#</code> the last one.
                </li>
                <li>
                    <b><code>%c</code> is not supported.</b> A client-id pattern cannot be expanded
                    when the rules are evaluated, so those rules are listed as not imported.
                    <code>%u</code> is expanded when the rule is imported.
                </li>
            </ul>
            </section>
        </DocumentationPage>;
}
