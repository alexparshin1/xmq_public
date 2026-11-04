import React from "react";
import {Link} from "react-router-dom";
import DocumentationPage from "./DocumentationPage";

export default function MosquittoImportDocumentation() {
    return <DocumentationPage page="mosquitto-import">
            <section>
            <h4 id="mosquitto-import">Importing from Mosquitto</h4>

            <p>
                An existing Mosquitto installation describes who may connect and what they may do
                in two files: a password file and an ACL file, or a single
                <code>dynamic-security.json</code>. XMQ reads those files and turns them into its
                own accounts and topic permissions, so a migration starts from the rules the
                broker already enforces rather than from an empty set written by hand.
            </p>

            <p>
                The import is on the <b>Import from Mosquitto</b> page of the configuration
                interface, with a second entry point on the <b>Permissions</b> page for resolving
                conflicts. It writes only accounts and rules: listeners, bridges, TLS, persistence
                and every other Mosquitto setting are <b>not</b> imported, because XMQ's
                configuration has a different shape and a half-copied listener block is worse than
                none. Set those up on the <b>Listeners</b> and <b>Setup</b> pages.
            </p>

            <h5 id="mosquitto-import-files">The files it reads</h5>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>File</th>
                    <th>Usually</th>
                    <th>What is imported</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>password_file</code></td>
                    <td><code>/etc/mosquitto/passwd</code></td>
                    <td>Usernames and their password verifiers, as accounts.</td>
                </tr>
                <tr>
                    <td><code>acl_file</code></td>
                    <td><code>/etc/mosquitto/aclfile</code></td>
                    <td><code>user</code>, <code>topic</code> and <code>pattern</code> rules, as
                        XMQ topic permissions.</td>
                </tr>
                <tr>
                    <td><code>dynamic-security.json</code></td>
                    <td>Beside the Mosquitto configuration</td>
                    <td>Clients, roles, groups and their ACLs, flattened onto accounts and
                        permissions.</td>
                </tr>
                </tbody>
            </table>

            <p>
                The files are read by the browser and sent to the broker as content. The XMQ server
                never opens <code>/etc/mosquitto</code> with its own privileges, and cannot be used
                to read a file the administrator could not read. If Mosquitto runs on another
                machine, copy the files to the machine where the interface is open; the password
                file holds secrets and should be deleted after the import.
            </p>

            <p>
                Either file may be left empty, and then that part is not imported. Choosing a
                <code>.json</code> file sets the ACL format to dynamic security by itself; the
                selector beside the input can be changed if the name of the file does not say what
                it is. Accounts and rules are applied in one database transaction: either both
                arrive or neither does.
            </p>

            <h5 id="mosquitto-import-plan">Plan, then apply</h5>

            <p>
                <b>Preview</b> reads the files and shows what would change without writing
                anything. For the password file it counts the accounts that are new, the ones that
                already exist, the verifiers this build cannot check and the lines it could not
                use. For the ACL it counts the rules that are new, already present, covered by an
                existing rule, in conflict with one, or not representable. Neither the plan nor the
                result ever shows a password.
            </p>

            <p>
                Nothing is stored while a conflict is unresolved. An allow that overlaps a deny, or
                two rules that cannot both hold for the same subject, is shown on the
                <b>Permissions</b> page, where each one is resolved by keeping the existing rule,
                using the imported one, or skipping it. An import that is refused leaves the rule
                set exactly as it was. Importing the same file twice adds nothing: rules already
                present are recognised rather than duplicated, and a rule an existing rule already
                covers is not added.
            </p>

            <p>
                An import of at least one ACL rule turns on <b>deny by default</b>: a client with
                no matching rule is denied, exactly as Mosquitto behaves when an ACL file is
                present. An empty ACL file leaves that setting alone - opening or locking the whole
                broker on no evidence would both be worse.
            </p>

            <h5 id="mosquitto-import-passwords">Password verifiers</h5>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Verifier</th>
                    <th>Mosquitto</th>
                    <th>Handling</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td>clear text</td>
                    <td>any</td>
                    <td>Accepted and re-hashed with this broker's own verifier.</td>
                </tr>
                <tr>
                    <td><code>$7$</code></td>
                    <td>2.0 and newer</td>
                    <td>Checked natively; replaced with this broker's verifier at the first
                        successful login.</td>
                </tr>
                <tr>
                    <td><code>$6$</code></td>
                    <td>1.6</td>
                    <td>Checked when the platform has <code>crypt()</code> - not on Windows.</td>
                </tr>
                <tr>
                    <td><code>argon2id</code></td>
                    <td>2.1</td>
                    <td>Checked when OpenSSL is 3.2 or newer. Older source builds - Ubuntu 24.04
                        and older, Debian 12, RHEL 9, FreeBSD 14 - report it as not imported.</td>
                </tr>
                </tbody>
            </table>

            <p>
                A verifier this build cannot check is listed as <b>not imported</b> rather than
                stored as an account nobody can sign in to; that account's password has to be set
                again. An imported verifier is checked both by the interface's own accounts and by
                the <code>user-database</code> extension that MQTT clients authenticate against.
                Only verifiers read from a Mosquitto password file are affected: XMQ's own accounts
                and every other authentication path are unchanged.
            </p>

            <h5 id="mosquitto-import-acl">ACL file rules</h5>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Mosquitto</th>
                    <th>XMQ permission</th>
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
                Rules are ordered the way Mosquitto would check them: the client's roles first, then
                its groups by priority, then the roles in each group, with each role's ACLs by their
                priority. A <code>topic deny</code> is checked before any allow, because that is
                what Mosquitto does.
            </p>

            <h5 id="mosquitto-import-dynsec">Dynamic Security</h5>

            <p>
                A <code>dynamic-security.json</code> describes clients, roles and groups, and the
                rules a client gets come from all three. The import flattens that onto accounts:
                a client's rules become rules on <code>user:&lt;name&gt;</code>, in the order
                Mosquitto would check them. A client that is disabled is imported as a disabled
                account; <code>clientid</code> is checked by the import and reported when it would
                change how a <code>%c</code> rule matches.
            </p>

            <p>
                An allowing <code>defaultACLAccess</code> becomes a <code>#</code> rule checked after
                everything else. A denying one needs no rule at all, because in XMQ a client with
                rules but no match is already denied. Rules that cannot be represented are listed as
                not imported, with the reason, rather than dropped silently.
            </p>

            <h5 id="mosquitto-import-csv">Many devices from a spreadsheet</h5>

            <p>
                An XLS or XLSX file is not read directly. Save the sheet as <b>CSV UTF-8</b>, then
                make one <code>username:password</code> line per device. With the username in column
                A and the password in column B, a third column with
                <code>=A2&amp;":"&amp;B2</code> produces the line; a small script does the same for
                larger sheets. The result is the password file described above, so it is imported
                through the same page: clear-text lines are accepted there and hashed with this
                broker's verifier.
            </p>

            <p>
                That import puts every account in <code>Default</code>; it has no column for groups,
                so a fleet that needs one group per device type cannot be expressed this way. For
                very large files, import in batches: the broker hashes every password as it adds the
                account. The file holds clear passwords, so send it over HTTPS, delete it after the
                import, use unique passwords rather than one shared secret, and look at the plan for
                repeated usernames before applying it.
            </p>

            <h5 id="mosquitto-import-limitations">Known limitations</h5>

            <ul>
                <li>
                    <b>Per-listener rules are not available.</b> A rule applies to every MQTT and
                    MQTT+SSL listener on the node. Separating what one listener permits from another
                    has to be done with separate accounts or separate clusters.
                </li>
                <li>
                    <b>Read is subscribe.</b> Mosquitto's <code>read</code> and XMQ's
                    <code>subscribe</code> are the same permission. A Dynamic Security
                    <code>publishClientReceive</code> allow is therefore redundant and is reported
                    as not imported; a denial is carried as a subscribe deny.
                </li>
                <li>
                    <b>A subscription is matched as a filter.</b> Dynamic Security distinguishes
                    <code>subscribeLiteral</code> from <code>subscribePattern</code>; both are stored
                    as a filter. For ordinary clients the two are the same, but a rule written to
                    permit one literal subscription also permits the equivalent wildcard.
                </li>
                <li>
                    <b><code>%c</code> is only as good as the client id.</b> A client id is chosen
                    by the client unless it is bound by authentication. A client id or username
                    containing <code>+</code>, <code>#</code> or <code>/</code> matches no pattern
                    rule, so it cannot widen one.
                </li>
                <li>
                    <b>An empty topic level is refused.</b> A rule filter naming an empty level is
                    treated as a typo by the import and by the control API alike. <code>+</code> and
                    <code>#</code> must each be a whole level, and <code>#</code> the last one.
                </li>
                <li>
                    <b>Passwords are not imported as clear text files are.</b> A verifier this build
                    cannot check is reported rather than taken on trust, and has to be set again.
                </li>
            </ul>

            <p>
                After the import, <b>Test access</b> on the
                <Link to="/xmq_doc_permissions">Permissions</Link> page answers a specific question
                using the same evaluation the broker applies, and names the rule that decided - the
                quickest way to confirm that a migrated policy behaves as the old one did.
            </p>
            </section>
        </DocumentationPage>;
}
