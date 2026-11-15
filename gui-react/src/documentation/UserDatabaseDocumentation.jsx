import React from "react";
import {Link} from "react-router-dom";
import DocumentationPage from "./DocumentationPage";

/**
 * @brief The accounts extension: the broker's own SQL account database.
 *
 * It is the authenticator every installation has, and the one other authenticators are compared
 * with in ORDER: the four answers, the password formats an import can leave behind, and the
 * one-minute cache are the three things an operator asks about after a password change "does not
 * work".
 */
export default function UserDatabaseDocumentation() {
    return <DocumentationPage page="user-database">
        <section>
            <h4 id="user-database">User database authentication</h4>

            <p>
                The <code>user-database</code> extension authenticates MQTT clients against the
                broker's own SQL account database - the same accounts the <b>Users</b> screen edits.
                An account added or changed in the interface is in the same store the extension
                reads, so there is no copy to synchronize and no second user table to explain.
            </p>

            <p>
                Unlike the optional extensions it is not a switch on the <b>Extensions</b> page: a
                broker either authenticates against its account database or runs with no accounts
                at all, and the <b>Users</b> screens exist only when it does. What it has is a
                configuration, edited like any other extension's.
            </p>

            <p>
                It answers one question - may this client connect - and nothing else. Group
                membership and topic permissions are the broker's authorizer's business, read from
                the same database through <Link to="/xmq_doc_permissions">Permissions</Link>. This
                extension declares no authorizer capability and never answers a publish or
                subscribe question.
            </p>
        </section>

        <section>
            <h4 id="user-database-answers">The four answers</h4>

            <p>
                An authenticator can say more than yes or no, and the difference is what a client
                sees during an outage. The broker asks the configured authenticators in order and
                stops at the first allow or deny.
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Answer</th>
                    <th>When</th>
                    <th>What the client sees</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td>ALLOW</td>
                    <td>The account exists, is enabled, and the password verifies.</td>
                    <td>The connection is accepted.</td>
                </tr>
                <tr>
                    <td>DENY</td>
                    <td>The password is wrong, the account is disabled, it has no password set, or
                        it does not exist while <code>unknown_user</code> is <code>deny</code>.</td>
                    <td>Bad user name or password: CONNACK <code>0x86</code> for MQTT 5,
                        <code>0x04</code> for MQTT 3.x.</td>
                </tr>
                <tr>
                    <td>NOT_HANDLED</td>
                    <td>The client sent no user name - that is the broker's anonymous policy to
                        decide - or the name is unknown while <code>unknown_user</code> is
                        <code>abstain</code>.</td>
                    <td>Nothing yet: the next authenticator is asked, and the broker refuses the
                        client if none claims it.</td>
                </tr>
                <tr>
                    <td>SUBSYSTEM_ERROR</td>
                    <td>The database could not be reached or the query failed.</td>
                    <td>Server unavailable: CONNACK <code>0x88</code> for MQTT 5,
                        <code>0x03</code> for MQTT 3.x.</td>
                </tr>
                </tbody>
            </table>

            <p>
                An outage is never reported as a wrong password: telling a client with good
                credentials that they are wrong sends an operator looking at the wrong system, and
                it hides a database failure behind an authentication failure.
            </p>

            <p>
                An unknown name is the one answer that is deliberately not remembered. A name may
                appear a second after a client tried it, and with <code>abstain</code> configured
                the next attempt has to reach the database and see it.
            </p>
        </section>

        <section>
            <h4 id="user-database-database">The database</h4>

            <p>
                Any database the SPTK driver layer speaks:
                <code>sqlite3://localhost/path</code>,
                <code>postgresql://user:pass@host/db</code> or
                <code>mysql://user:pass@host/db</code> (MariaDB is treated as MySQL). Set it once
                on the <b>Initial Setup</b> page; the extension uses that address unless its own
                <code>database_uri</code> names another.
            </p>

            <p>
                The <code>xmq_user</code> table is created on first start when it is missing, so the
                database account this connects with has to be allowed to create tables - or the
                table has to be created by hand. SQLite and PostgreSQL and MySQL are understood; a
                driver that this extension does not know how to give an auto-assigned key is
                refused rather than half-created.
            </p>

            <p>
                <b>Changing the address on a running server</b> opens and probes the new database
                first, and refuses the change while the old one is still authenticating if the new
                one names nothing, cannot be logged into, or has no schema. When the new database
                is one this extension has just created, the accounts are copied into it; a table
                that was already there is never written to, because a shared database somebody else
                populates must not quietly acquire this node's local accounts because a URI was
                edited. Ids are not copied - the new store assigns its own - and group membership is
                not copied, because it is not this extension's data.
            </p>

            <p>
                In a cluster every node has to name the same database. That shared database is what
                makes an account added on one node valid on the next connection to another; see
                <Link to="/xmq_doc_cluster">Setting up a Cluster</Link>.
            </p>
        </section>

        <section>
            <h4 id="user-database-passwords">Password formats</h4>

            <p>
                The password is verified inside the extension, not by the database, and it
                understands the formats a broker may already have on disk:
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Prefix</th>
                    <th>Format</th>
                    <th>Where it comes from</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>pbkdf2-sha256$</code></td>
                    <td><code>pbkdf2-sha256$iterations$salt$hash</code></td>
                    <td>What the interface writes for an account created here.</td>
                </tr>
                <tr>
                    <td><code>$7$</code></td>
                    <td>PBKDF2-HMAC-SHA512</td>
                    <td>A Mosquitto 2.0 password file, copied as it is by the import.</td>
                </tr>
                <tr>
                    <td><code>$6$</code></td>
                    <td>SHA512-crypt, checked with the platform's <code>crypt()</code></td>
                    <td>A Mosquitto 1.6 password file, where the platform has
                        <code>crypt()</code>.</td>
                </tr>
                <tr>
                    <td><code>$argon2id$</code></td>
                    <td>Argon2id</td>
                    <td>An account imported from a source that uses it, where Argon2 is
                        available.</td>
                </tr>
                </tbody>
            </table>

            <p>
                An account whose stored password is empty has never had one set, and nothing
                authenticates against it - least of all an empty password, which is exactly what a
                client that sent none arrives with. Disabling an account is the
                <code>is_enabled</code> column, checked on every verification.
            </p>

            <p>
                Importing accounts and their hashes is described on
                <Link to="/xmq_doc_mosquitto_import">Importing from Mosquitto</Link>.
            </p>
        </section>

        <section>
            <h4 id="user-database-settings">Configuration</h4>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Setting</th>
                    <th>Default</th>
                    <th>Meaning</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>database_uri</code></td>
                    <td>empty</td>
                    <td>The database that holds user accounts. Empty means the broker's own
                        <code>authentication.database_uri</code>, which is the setting on the
                        <b>Initial Setup</b> page. Changing it on a running server copies the
                        accounts into a database it has just created.</td>
                </tr>
                <tr>
                    <td><code>username</code></td>
                    <td>empty</td>
                    <td>The account this extension opens the database with - not an MQTT client.
                        Leave empty for SQLite, which has no users.</td>
                </tr>
                <tr>
                    <td><code>password</code></td>
                    <td>empty</td>
                    <td>The password for that database account. A secret: masked in the interface
                        and kept out of the log, including the line that names the database when a
                        connection fails.</td>
                </tr>
                <tr>
                    <td><code>unknown_user</code></td>
                    <td><code>deny</code></td>
                    <td><code>deny</code> refuses a name this database does not hold, because it is
                        the account database. <code>abstain</code> passes the client to the next
                        authenticator, for a deployment where a directory or an HTTP service owns
                        the names this one has never held.</td>
                </tr>
                <tr>
                    <td><code>poll_interval</code></td>
                    <td><code>5</code></td>
                    <td>How often, in seconds, to check <code>xmq_store_version</code> for changes
                        made by another node sharing this database. Only PostgreSQL and MySQL;
                        SQLite is local and is not polled. <code>0</code> disables polling.</td>
                </tr>
                </tbody>
            </table>
        </section>

        <section>
            <h4 id="user-database-cache">The one-minute cache</h4>

            <p>
                Verifying a password costs an HMAC and a key derivation - about a millisecond - and
                the broker hands every connection to an authenticator over a small pool of threads.
                A scenario run, or a reconnect storm after a restart, arrives far faster than that.
                So the answer is remembered for up to 60 seconds.
            </p>

            <p>
                Nothing in the cache holds a password. An entry is found by an HMAC of the
                credentials under a key made when the process starts and never written anywhere, so
                a core file yields nothing replayable. Refusals are remembered as well as
                admissions: otherwise a client sending wrong passwords costs a full derivation each
                time, which is a way to spend the broker's cores from outside. A corrected password
                hashes to a different entry and is verified properly, so keeping refusals delays no
                repair.
            </p>

            <p>
                The cache is 4096 entries, direct-mapped: a collision overwrites and a miss is not a
                failure, it falls through to a real verification. It is dropped when the interface
                saves a change to accounts or groups on this node, and when the version poll sees
                another node's change. <b>There is no way to shorten the lifetime itself</b>, so a
                password changed through a path the broker is not told about, or an account revoked
                elsewhere while polling is off, can keep working for up to a minute. Restarting the
                broker clears it immediately.
            </p>
        </section>

        <section>
            <h4 id="user-database-order">Beside other authenticators</h4>

            <p>
                Authenticators are asked in configuration order and the first allow or deny is
                final. <code>user-database</code> is the natural last entry: an HTTP service, a
                directory or a token issuer claims the names it owns, and this one answers for the
                rest. That arrangement needs <code>unknown_user</code> set to <code>abstain</code>;
                with the default <code>deny</code> a name the database has never held is refused
                before the next authenticator is asked.
            </p>

            <p>
                The other way round - this database as the authority, with another source reachable
                only for names it does not hold - is the same ordering written differently, and is
                what the abstain setting exists for.
            </p>
        </section>

        <section>
            <h4 id="user-database-limits">Limits</h4>

            <ul>
                <li>
                    <b>One answer per connection.</b> A password or an enablement change reaches the
                    next CONNECT once the cache entry expires or is dropped; an established session
                    is not disconnected by a revocation.
                </li>
                <li>
                    <b>No topic rules here.</b> Group membership and permissions are the authorizer's
                    and are edited on the <b>Groups</b> and <b>Permissions</b> screens, not in this
                    extension's settings.
                </li>
                <li>
                    <b>The database is on the connect path.</b> With the cache cold, every connect
                    that misses it costs a query; an unreachable database refuses connects as
                    <em>server unavailable</em> rather than admitting them.
                </li>
                <li>
                    <b>Schema changes are refused, not migrated.</b> A <code>xmq_user</code> table
                    this extension does not recognise is a start-up error, which is preferable to
                    reading the wrong columns.
                </li>
            </ul>
        </section>
    </DocumentationPage>;
}
