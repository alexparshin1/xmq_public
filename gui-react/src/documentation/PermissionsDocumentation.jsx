import React from "react";
import {Link} from "react-router-dom";
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
                Each subject has one <b>policy</b>: an ordered list of rules. A rule says whether it
                allows or denies, which direction it applies to (<code>publish</code> or
                <code>subscribe</code>), and a topic filter. The list is checked from the top and the
                first matching rule decides, so an allow above a deny lets the deny's topic through;
                the order is part of the policy and can be edited. The subject is one of:
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
                A client is resolved to <code>user:&lt;name&gt;</code>, then to the groups of its
                account, and a pattern rule from an import applies to every client; the
                <code>anonymous</code> subject applies only when there is no username. Rules from
                all of those subjects share one global order, which is why the list a subject shows
                is its part of that order rather than a private sequence. When a client has rules
                but none of them covers the topic, the answer is deny: its rules are an allow-list,
                exactly as in Mosquitto. The setting below decides only clients that have no rules
                at all.
            </p>

            <p>
                An import keeps the order Mosquitto would check its rules in: the client's roles
                first, then its groups by priority, then the roles in each group, with each role's
                ACLs by their priority. Editing a subject's policy replaces its rules in one step,
                keeping the other subjects' relative order and renumbering the shared sequence.
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
                <b>See <Link to="/xmq_doc_mosquitto_import">Importing from Mosquitto</Link> for the
                file formats, password verifiers, conflict handling and known limitations in one
                place.</b> This section describes how the imported rules are evaluated.
            </p>

            <p>
                The <b>Import from Mosquitto</b> page takes the password file and the ACL file as
                file inputs, read by the browser and sent as content: the XMQ server does not open
                <code>/etc/mosquitto</code> with its own privileges, and cannot be used to read a
                file the administrator could not. Either file may be left empty. Accounts and rules
                are applied in one database transaction: either both arrive or neither does.

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
                clear-text lines. A 1.6 <code>$6$</code> verifier is checked when the platform's
                <code>crypt()</code> is available, and a 2.1 <code>argon2id</code> verifier when
                OpenSSL is 3.2 or newer; the Windows build has no <code>crypt()</code>, so
                <code>$6$</code> is reported there. A verifier the build cannot check is listed as
                not imported rather than stored as an account nobody can sign in to. An imported
                verifier is checked on both paths - the interface's own accounts and the
                user-database extension that MQTT clients authenticate against - and is replaced
                with this broker's own at the first successful login.
            </p>

            <p>
                This affects only verifiers read from a Mosquitto password file; XMQ's own accounts,
                the <code>$7$</code> import and every other authentication path are unchanged.
                <code>$6$</code> is unavailable on Windows, which has no <code>crypt()</code>, and
                <code>argon2id</code> is not compiled on a source build against OpenSSL older than
                3.2 - for example Ubuntu 24.04 or older, Debian 12, RHEL 9 and FreeBSD 14. There the
                verifier is reported as not imported and that account's password has to be set
                again; the packaged Linux and BSD versions listed under Install all have OpenSSL 3.2
                or newer.
            </p>

            <p>
                Dynamic Security roles are flattened onto accounts: the rules a client gets through
                its roles and groups become rules on <code>user:&lt;name&gt;</code>, in the order
                Mosquitto would check them. An allowing <code>defaultACLAccess</code> becomes a
                <code>#</code> rule checked after everything else; a denying one needs no rule,
                because no match is already a deny. Rules that cannot be represented are listed as
                not imported, with the reason, rather than dropped silently.
            </p>

            <h5 id="permissions-csv">Many devices from a spreadsheet</h5>

            <p>
                An XLS or XLSX file is not read directly. Save the sheet as
                <b>CSV UTF-8</b>, then make one <code>username:password</code> line per device.
                With the username in column A and the password in column B, a third column with
                <code>=A2&amp;":"&amp;B2</code> produces the line; a small script does the same for
                larger sheets. The result is the password file described above, so it is imported
                through <b>Import from Mosquitto</b>: clear-text lines are accepted there and are
                hashed with this broker's verifier.
            </p>

            <p>
                That import puts every account in <code>Default</code>; it has no column for groups,
                so a fleet that needs a group per device type cannot be expressed this way yet. When
                a pilot needs a concrete layout - for example <code>username,password,groups</code> -
                a CSV import with that shape will be added rather than guessed at now.
            </p>

            <p>
                The file holds clear passwords: send it over HTTPS, delete it after the import, use
                unique passwords rather than one shared secret, and look at the import plan for
                repeated usernames before applying it. The plan never shows a password. For very
                large files, import in batches; the broker hashes every password as it adds the
                account.
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
