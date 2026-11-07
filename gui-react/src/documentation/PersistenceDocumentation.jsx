import React from "react";
import DocumentationPage from "./DocumentationPage";

export default function PersistenceDocumentation() {
    return <DocumentationPage page="persistence">
        <section>
            <h2 id="persistence-modes">Redis persistence modes</h2>
            <p>
                XMQ stores persistent client sessions, subscriptions, retained messages and
                undelivered QoS 1 and QoS 2 messages in Redis. The Persistence screen offers two
                modes. Both retain stored state across an XMQ restart while Redis remains
                available; they differ in when XMQ acknowledges publishes and how Redis persists
                writes to disk.
            </p>
            <p>
                Redis is a separate service. XMQ changes Redis persistence settings when a mode is
                applied, then saves them to the Redis configuration file. Redis itself does not
                restart. These settings apply to the <b>entire Redis server</b>, including every
                database and every application using it, not only XMQ or the database number in
                its URI. Use a Redis instance dedicated to XMQ if other applications need
                different persistence settings.
            </p>
            <table className="userManualTable">
                <thead><tr><th>Mode</th><th>Redis settings</th><th>Behavior</th></tr></thead>
                <tbody>
                <tr>
                    <td><b>Persistent</b></td>
                    <td><code>appendonly no</code></td>
                    <td>
                        XMQ uses a 10 ms write-behind window. A publisher may receive its
                        acknowledgement before Redis stores the corresponding record. The
                        <code>Max queued writes</code> default is 1000. When this queue fills,
                        publishers pause until Redis catches up. After a Redis restart, recovery
                        depends on RDB snapshots; without RDB, Redis restart loses its data.
                    </td>
                </tr>
                <tr>
                    <td><b>Durable</b></td>
                    <td><code>appendonly yes</code>, <code>appendfsync always</code></td>
                    <td>
                        XMQ waits for Redis to confirm a local AOF fsync before it sends a
                        successful publish acknowledgement. The interface uses a zero queued-write
                        limit for this mode.
                    </td>
                </tr>
                </tbody>
            </table>
            <p>
                Redis must permit <code>CONFIG SET</code> and <code>CONFIG REWRITE</code>. XMQ
                checks that Redis can accept and save the requested settings; if this fails, the
                mode change is rejected.
            </p>
        </section>

        <section>
            <h2 id="persistence-failures">Failure guarantees</h2>
            <table className="userManualTable">
                <thead><tr><th>Failure</th><th>Persistent</th><th>Durable</th></tr></thead>
                <tbody>
                <tr>
                    <td>XMQ process restart</td>
                    <td>
                        Records already stored in Redis remain available. Writes still queued in
                        XMQ can be lost, bounded by <code>Max queued writes</code>.
                    </td>
                    <td>
                        A successful acknowledgement waits for local Redis AOF fsync. If no
                        acknowledgement was received, retry according to the MQTT QoS flow; an
                        ambiguous outcome can result in a duplicate.
                    </td>
                </tr>
                <tr>
                    <td>Redis process restart, disk intact</td>
                    <td>Recovery depends on Redis RDB snapshots because AOF is disabled.</td>
                    <td>Redis recovers the AOF, including writes confirmed by the local fsync barrier.</td>
                </tr>
                <tr>
                    <td>Redis host or disk loss</td>
                    <td>Redis data on that host may be lost.</td>
                    <td>
                        Redis data on that host may be lost. XMQ does not wait for a replica fsync
                        and does not provide replica failover guarantees.
                    </td>
                </tr>
                </tbody>
            </table>
            <p>
                Durable mode depends on the operating system, filesystem, controller and drive
                honoring <code>fsync</code>. It protects locally fsynced Redis data against an
                ordinary Redis process restart while its storage remains intact; it is not a
                substitute for backups or a tested high-availability design. Persistent mode's
                queue limit is a count bound, not a fixed time bound when Redis is slow.
            </p>
            <p>
                If Redis is unavailable when XMQ starts, XMQ logs the connection failure and can
                continue using in-memory storage. In that state, session and message persistence
                is unavailable; check the broker log and Redis connection.
            </p>
        </section>

        <section>
            <h2 id="persistence-switching">Switching modes</h2>
            <p>
                Choose Persistent or Durable on the Persistence screen and apply the change. XMQ
                validates Redis access, updates and saves Redis settings, saves XMQ settings, and
                restarts the MQTT service. MQTT clients disconnect during the broker restart;
                Redis itself does not restart. Because Redis settings are server-wide, consider
                other databases and applications on the same Redis server before switching.
            </p>
        </section>
    </DocumentationPage>;
}
