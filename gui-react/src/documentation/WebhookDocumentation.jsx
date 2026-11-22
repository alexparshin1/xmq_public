import React from "react";
import DocumentationPage from "./DocumentationPage";

export default function WebhookDocumentation() {
    return <DocumentationPage page="webhook">
        <section>
            <h4 id="webhook">Message and log exports</h4>
            <p><code>message_export</code> exports accepted MQTT messages and <code>log_export</code>
                {" "}exports broker log records. Each has its own filters, sink settings and queue,
                even when both use the same sink type.</p>

            <h4 id="webhook-enable">Set up an export</h4>
            <ol>
                <li>Open the exporter on the <b>Extensions</b> screen and choose a filter mode.</li>
                <li>Under <b>Delivery</b>, choose <code>http_sink</code> or <code>db_sink</code> and configure it.</li>
                <li>Click <b>Apply</b>, then <b>Enable</b> if it is stopped.</li>
            </ol>

            <h4 id="webhook-sources">Sources and filters</h4>
            <ul>
                <li><b>MQTT:</b> <b>Topics</b> matches any configured MQTT filter and uses the MQTT
                    body template. <b>SQL rules</b> filters and shapes the output with SQL.
                    <b> Max payload</b> limits the incoming MQTT payload in bytes; 0 means no limit.</li>
                <li><b>Logs:</b> <b>Level and subjects</b> selects a minimum severity and optional
                    comma-separated categories, such as <code>connect,server_connections</code>.
                    An empty category list means all categories. <b>SQL rules</b> replaces both the
                    level/category filter and the template. Only broker log records are available;
                    internal <code>[extensions]</code> diagnostics are not exported.</li>
            </ul>

            <h4 id="webhook-payload">SQL rules and output fields</h4>
            <p><code>SELECT</code> and <code>FROM</code> are required; <code>WHERE</code> is optional.
                Use <code>AS</code> to rename an output key. Separate rules with semicolons; the first
                match produces one JSON object. <b>Test rule</b> uses sample data to show the match and
                output without saving settings or sending anything.</p>
            <p>MQTT <code>FROM</code> takes one or more quoted topic filters separated by commas.
                Fields: <code>topic</code>, <code>client_id</code>, <code>username</code>,
                <code> timestamp</code> (UTC), <code>qos</code> (number), <code>retain</code> (boolean),
                and <code>payload</code> (complete UTF-8 text, or null for invalid UTF-8).
                <code> SELECT *</code> includes all of these. Select scalar JSON paths such as
                <code> payload.temperature</code> explicitly; only rules using paths require valid JSON.</p>
            <pre>{'SELECT topic, payload.temperature AS temp\nFROM "sensors/#", "devices/+/status"\nWHERE CAST(payload.temperature AS DOUBLE) > 80 AND NOT retain'}</pre>
            <p>For logs, <code>logs</code> in <code>FROM logs</code> is the only source; it is not a
                destination database table. <code>SELECT *</code> returns <code>timestamp</code> (UTC),
                <code> level</code> (debug, info, warning, or error), <code>subject</code> (category),
                and <code>message</code> (log text).</p>

            <h4 id="webhook-sql-where">SQL rules: WHERE syntax</h4>
            <ul>
                <li>Comparisons: <code>{"= != <> < <= > >="}</code>.</li>
                <li>Logic: <code>NOT</code>, <code>AND</code>, <code>OR</code>, in that order.
                    Use parentheses to group conditions and boolean fields directly, as in <code>NOT retain</code>.</li>
                <li>Null tests: <code>IS NULL</code> and <code>IS NOT NULL</code>.</li>
                <li>Numeric conversion: <code>CAST(payload.temperature AS DOUBLE) &gt; 80</code>.</li>
            </ul>
            <p>Comparisons across types or with NULL yield UNKNOWN. Only TRUE matches.
                <code> LIKE</code>, <code>IN</code>, <code>BETWEEN</code>, regular expressions and
                <code> ==</code> are not supported. Log <code>level</code> is text, not a severity rank:</p>
            <pre>{"SELECT * FROM logs\nWHERE level = 'warning' OR level = 'error'"}</pre>

            <h4 id="webhook-template">Body templates</h4>
            <p>Templates apply only in <b>Topics</b> or <b>Level and subjects</b> mode. A blank template
                uses the default JSON with <code>topic</code>, <code>message</code> and <code>level</code>.
                Substitution JSON-escapes values but does not add quotes; quote string placeholders.
                Unknown placeholders stay unchanged.</p>
            <p>MQTT placeholders: <code>{"{{id}}, {{source}}, {{timestamp}}, {{topic}}, {{qos}}, {{retain}}, {{client_id}}, {{username}}, {{payload}}, {{payload_base64}}, {{message}}, {{level}}"}</code>.
                Source and level are <code>message</code>; <code>{"{{message}}"}</code> is Base64,
                as is <code>{"{{payload_base64}}"}</code>. <code>{"{{payload}}"}</code> inserts UTF-8
                text and skips invalid UTF-8. Leave QoS and retain unquoted for JSON numbers and booleans.</p>
            <p>Log placeholders: <code>{"{{id}}, {{source}}, {{timestamp}}, {{topic}}, {{level}}, {{subject}}, {{message}}"}</code>.
                Source is <code>log</code> and topic is empty.</p>

            <h4 id="webhook-settings">Delivery settings</h4>
            <h5 id="webhook-signature">HTTP/HTTPS request</h5>
            <p><code>http_sink</code> sends JSON as a POST. Select the URL scheme next to the address;
                HTTPS verifies certificates against the broker's trust store. Plain HTTP is unencrypted.
                Optional headers use <code>Name: value</code>. A secret adds
                <code> X-XMQ-Signature: sha256=&lt;hex&gt;</code>, the HMAC-SHA256 of the exact request body.</p>
            <p>Timeout is in milliseconds; retries are additional attempts after the first. Concurrency
                limits the number of in-flight requests. Delivery retries transient failures such as
                connection failures, timeouts, HTTP 429 and 5xx.</p>
            <h5>Database</h5>
            <p><code>db_sink</code> uses a parameterized INSERT. Choose an installed PostgreSQL, MySQL,
                Oracle, SQLite or MSSQL driver and supply its connection settings, or choose an existing
                database URI. The table must already exist and <code>:fields</code> is required:</p>
            <pre>{"INSERT INTO exported (body) VALUES (:fields)"}</pre>
            <p><code>:fields</code> contains the entire JSON body. Extra parameters can reference names
                or aliases selected by SQL, or placeholder values in template mode, as strings. Only
                available fields can be bound in addition to <code>:fields</code>.</p>

            <h4 id="webhook-delivery">Queues and retries</h4>
            <p>Queues are bounded and in memory. A full queue can drop records and a restart can lose
                queued records. On a database connection failure the exporter retries after 1, 2, 4, 8,
                16 and 32 seconds, then every 60 seconds until delivery or shutdown. A lost response
                after a commit can produce a duplicate; use idempotent writes where possible.</p>

            <h4 id="webhook-limits">Deprecated: webhook</h4>
            <p>The legacy <code>webhook</code> is deprecated in 1.2.x and is scheduled to be disabled
                in 1.3. Use <code>message_export</code> and <code>log_export</code> instead: they have
                independent settings and queues, SQL filtering and projection with a rule tester,
                and a choice of HTTP or database delivery.</p>
            <p>To migrate, copy the applicable source filters or SQL rules and body templates into
                the corresponding exporter. To keep HTTP delivery, choose <code>http_sink</code> and
                copy the URL, headers and signing secret. Disable the legacy extension when switching
                over to avoid duplicate deliveries.</p>
        </section>
    </DocumentationPage>;
}
