import React from "react";
import DocumentationPage from "./DocumentationPage";

export default function WebhookDocumentation() {
    return <DocumentationPage page="webhook">
        <section>
            <p><b>Migration note:</b> The legacy <code>webhook</code> extension is deprecated in
                1.2.x and scheduled to be disabled in 1.3. Use the <code>message_export</code> and
                {" "}<code>log_export</code> extensions instead. Copy the applicable filters and templates
                into each replacement extension. To keep HTTP delivery, select the <code>http_sink</code> sink and copy the URL,
                headers and signing secret. Disable the legacy extension to avoid duplicate deliveries.</p>

            <h4 id="webhook">Message and log exports</h4>
            <p>The <code>message_export</code> extension exports accepted MQTT messages. The
                {" "}<code>log_export</code> extension exports broker log records. Each extension has
                its own filters, delivery settings and queue,
                even when both use the same sink type. An extension can also be configured several
                times: each <b>settings set</b> has its own instance name, filters, sink and delivery
                queue, so one <code>message_export</code> can write <code>orders/#</code> to one
                database and <code>audit/#</code> to another. Use <b>Add Data Integration</b> on the Extensions
                screen to make another set.</p>
            <p>Kafka integration works in both directions: <a href="#webhook-kafka"><code>kafka_sink</code></a>
                {" "}sends exports to Kafka, and <a href="#webhook-limits"><code>kafka_source</code></a>
                {" "}imports Kafka records into MQTT. Both are available on Linux builds with librdkafka.</p>

            <h4 id="webhook-enable">Set up an export</h4>
            <ol>
                <li>Select a settings set of <code>message_export</code> or <code>log_export</code> on the
                    {" "}<b>Extensions</b> screen, or press <b>Add Data Integration</b> to create one, then choose a filter mode.</li>
                <li>Under <b>Delivery</b>, choose <code>http_sink</code> for HTTP,
                    <code> db_sink</code> for a database, or <code>kafka_sink</code> for Kafka.
                    Configure the selected sink.</li>
                <li>Click <b>Apply</b> to save the settings. If the extension is not running, click <b>Enable</b>.</li>
            </ol>

            <h4 id="webhook-sources">Sources and filters</h4>
            <ul>
                <li><b>MQTT:</b> In <b>Topics</b> mode, the extension exports messages that match
                    any configured MQTT topic filter and formats them with the MQTT body template.
                    In <b>SQL rules</b> mode, the SQL rule selects messages and defines the output fields.
                    {" "}<b>Max payload</b> limits the size of MQTT payloads eligible for export, in bytes;
                    0 means no limit.</li>
                <li><b>Logs:</b> In <b>Level and subjects</b> mode, set a minimum severity and optionally
                    list comma-separated log categories, such as <code>connect,server_connections</code>.
                    An empty category list means all categories. In <b>SQL rules</b> mode, the rule
                    replaces both the level and category filters and the log body template. Only broker log records are available;
                    internal <code>[extensions]</code> diagnostics are not exported.</li>
            </ul>

            <h4 id="webhook-payload">SQL rules and output fields</h4>
            <p><code>SELECT</code> and <code>FROM</code> are required; <code>WHERE</code> is optional.
                Use <code>AS</code> to rename an output key. Separate rules with semicolons; the first
                match produces one JSON object. <b>Test rule</b> uses sample data to show the match and
                output without saving settings or sending anything.</p>
            <h5>MQTT fields: <code>message_export</code></h5>
            <p>The <code>FROM</code> clause takes one or more quoted MQTT topic filters separated by commas.
                The following fields are available in <code>SELECT</code> and <code>WHERE</code>.</p>
            <table>
                <thead><tr><th>Field</th><th>Type</th><th>Value</th></tr></thead>
                <tbody>
                    <tr><td><code>topic</code></td><td>String</td><td>MQTT topic name</td></tr>
                    <tr><td><code>client_id</code></td><td>String</td><td>Publisher's MQTT client identifier</td></tr>
                    <tr><td><code>username</code></td><td>String</td><td>Publisher's username</td></tr>
                    <tr><td><code>timestamp</code></td><td>String</td><td>Export timestamp (ISO 8601, UTC)</td></tr>
                    <tr><td><code>qos</code></td><td>Number</td><td>MQTT QoS: 0, 1, or 2</td></tr>
                    <tr><td><code>retain</code></td><td>Boolean</td><td>MQTT retain flag</td></tr>
                    <tr><td><code>payload</code></td><td>String or NULL</td><td>Complete message body as UTF-8 text, or NULL for invalid UTF-8</td></tr>
                    <tr><td><code>payload.field</code></td><td>String, number, boolean, or NULL</td>
                        <td>Scalar value at a JSON path, including nested paths such as <code>payload.sensor.temperature</code></td></tr>
                </tbody>
            </table>
            <p><code>SELECT *</code> includes the seven top-level fields, including <code>payload</code>.
                It does not expand JSON paths: name those explicitly. Rules that access a JSON path require valid JSON.</p>
            <pre className="integrationSqlExample"><code>{'SELECT topic, payload.temperature AS temp\nFROM "sensors/#", "devices/+/status"\nWHERE CAST(payload.temperature AS DOUBLE) > 80 AND NOT retain'}</code></pre>
            <h5>Log fields: <code>log_export</code></h5>
            <p>Use <code>FROM logs</code>. The <code>logs</code> source represents broker log records,
                not a destination database table. All four fields are strings, are available in
                <code> SELECT</code> and <code>WHERE</code>, and are included in <code>SELECT *</code>.</p>
            <table>
                <thead><tr><th>Field</th><th>Value</th></tr></thead>
                <tbody>
                    <tr><td><code>timestamp</code></td><td>Export timestamp (ISO 8601, UTC)</td></tr>
                    <tr><td><code>level</code></td><td><code>debug</code>, <code>info</code>, <code>warning</code>, or <code>error</code></td></tr>
                    <tr><td><code>subject</code></td><td>Broker log category, such as <code>server_connections</code></td></tr>
                    <tr><td><code>message</code></td><td>Log message text</td></tr>
                </tbody>
            </table>

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
            <pre className="integrationSqlExample"><code>{"SELECT * FROM logs\nWHERE (level = 'warning' AND subject = 'server_connections')\n   OR (level = 'error')"}</code></pre>

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
            <p>The <code>http_sink</code> sink sends the JSON export body to an external web service
                in an HTTP POST request. Select the URL scheme next to the address;
                HTTPS verifies certificates against the broker's trust store. Plain HTTP is unencrypted.
                Optional headers use <code>Name: value</code>. A secret adds
                <code> X-XMQ-Signature: sha256=&lt;hex&gt;</code>, the HMAC-SHA256 of the exact request body.</p>
            <p>Timeout is in milliseconds; retries are additional attempts after the first. Concurrency
                limits the number of in-flight requests. Delivery retries transient failures such as
                connection failures, timeouts, HTTP 429 and 5xx.</p>
            <h5 id="webhook-kafka">Export to Kafka: kafka_sink</h5>
            <p>In <code>message_export</code> or <code>log_export</code>, select <code>kafka_sink</code>
                {" "}under <b>Delivery</b>. Set <b>Kafka brokers</b> to a comma-separated list of
                <code> host:port</code> addresses and <b>Kafka topic</b> to the destination topic.
                <b> Kafka timeout, ms</b> limits how long the sink waits for the record's delivery report.
                The sink reports success only after Kafka acknowledges the record
                (<code>acks=all</code>, meaning all in-sync replicas). The current sink uses
                To use TLS, set <b>Kafka transport</b> to <code>SSL</code>. The broker certificate and
                hostname are verified against the system trust store, or against <b>Kafka CA certificate</b>
                {" "}if an absolute path to a PEM file is given. The default transport is
                <code> PLAINTEXT</code>. For SASL, choose <code>SASL_SSL</code> or
                <code> SASL_PLAINTEXT</code>, set the mechanism to <code>SCRAM-SHA-256</code> or
                <code> SCRAM-SHA-512</code>, and provide the Kafka username and password. SASL
                credentials require a SASL transport. The test stand's SASL listener is at
                <code> theater:9094</code> (user <code>xmqtest</code>); <code>theater:9092</code>
                remains unauthenticated.</p>
            <p>An unavailable broker or a timeout triggers a retry. With persistent delivery,
                XMQ keeps the record on disk until Kafka acknowledges it. A retry can produce a
                duplicate if Kafka stored the record but XMQ did not receive the acknowledgement.</p>
            <p>The exported JSON becomes the Kafka record value. A nonempty projected field or
                alias named <code>key</code> becomes the record key. For example:</p>
            <pre className="integrationSqlExample"><code>{'SELECT topic, client_id AS key, payload FROM "sensors/#"'}</code></pre>
            <p>To verify the export, start a Kafka consumer on the destination topic, publish an
                MQTT message that matches the export filter, and check the consumer output.</p>

            <h5>Database</h5>
            <p>The <code>db_sink</code> sink writes records with a parameterized INSERT statement. Choose an installed PostgreSQL, MySQL,
                Oracle, SQLite or MSSQL driver and supply its connection settings, or choose an existing
                database URI. The table must already exist and <code>:fields</code> is required:</p>
            <pre className="integrationSqlExample"><code>{"INSERT INTO exported (body) VALUES (:fields)"}</code></pre>
            <p><code>:fields</code> contains the entire JSON body. Extra parameters can reference names
                or aliases selected by SQL, or placeholder values in template mode, as strings. Only
                available fields can be bound in addition to <code>:fields</code>.</p>
            <p>The database sink's own bounded queue is in memory and is used only when persistent delivery
                is off: it can drop records and is lost on restart. Enable <b>Persistent delivery</b> on the
                exporter to route INSERTs through the same SQLite queue as the other sinks, so committed
                records are replayed after a restart.</p>
            <p>In SQL rules mode, <code>:fields</code> contains the JSON produced by <code>SELECT</code>,
                not the original MQTT message. To store only a few fields from a large payload,
                select them explicitly:</p>
            <pre className="integrationSqlExample"><code>{'SELECT payload.device_id AS device_id, payload.temperature AS temperature\nFROM "sensors/#"'}</code></pre>
            <p>This stores a JSON object with only <code>device_id</code> and <code>temperature</code>.
                The current <code>db_sink</code> still requires <code>:fields</code> in the
                <code> INSERT</code>, even if other parameters populate individual columns.
                An <code>INSERT</code> that binds only those individual fields is not supported.</p>

            <h4 id="webhook-delivery">Queues and retries</h4>
            <p>By default, queues are bounded and in memory. A full queue can drop records and a restart can lose
                queued records. On a database connection failure the export extension retries after 1, 2, 4, 8,
                16 and 32 seconds, then every 60 seconds until delivery or shutdown. A lost response
                after a commit can produce a duplicate; use idempotent writes where possible.</p>
            <p>In builds with SQLite support, enable <b>Persistent delivery</b> and set
                <b> Persistent queue file</b> to an absolute file path in an existing private directory
                on the broker (mode 0700 on Unix). Persistent delivery is off by default. Use a separate
                file for each extension and sink configuration. An empty path while the switch is on
                prevents the extension from starting. The queue limit counts live record data and
                bindings, not SQLite file overhead. When the limit is reached, new records are dropped.</p>
            <p>Persistent delivery writes the rendered body and bindings to disk before attempting delivery.
                The same queue and worker serve every sink — <code>http_sink</code>, <code>db_sink</code>
                and <code>kafka_sink</code> — so retries, backoff and dead-lettering behave alike.
                It replays committed records after restart, in order, using one delivery worker.
                Persistent mode uses its own retry policy instead of the HTTP retry count and
                concurrency settings. The worker retries a failed head record, including permanent failures, with a delay
                capped at 60 seconds. Later records wait behind it. A record is removed when the
                sink reports success; for <code>kafka_sink</code> this requires a Kafka acknowledgement.
                A crash before removal can cause a duplicate. An MQTT acknowledgement does not confirm
                that the export has been written to disk.</p>
            <p>Changing sink settings requires a different queue file. The previous file remains associated
                with the original settings; the exporter will not deliver its records to the new destination.</p>

            <h4 id="webhook-limits">Import from Kafka: kafka_source</h4>
            <p>On Linux builds with librdkafka, configure <code>kafka_source</code> as a separate
                extension on the <b>Extensions</b> screen, not as a delivery sink. Enter the Kafka
                brokers, an existing Kafka topic, a consumer group and a local MQTT destination topic.
                Click <b>Apply</b>, then <b>Enable</b>. To verify the import, subscribe to
                the MQTT destination topic before enabling the extension and write a record
                to the configured Kafka topic.</p>
            <p><b>Kafka transport</b> defaults to <code>SSL</code>. The Kafka broker presents the
                certificate; the client checks the certificate chain and broker hostname. A private CA
                must be trusted on the XMQ host. The XMQ MQTT listener certificate is not used, and
                client certificates for mutual TLS are not configurable. Select <code>PLAINTEXT</code>
                {" "}only on a trusted test network. Hostname verification is enabled explicitly for
                both TLS transports. For SASL, choose <code>SASL_SSL</code> or
                <code> SASL_PLAINTEXT</code>, select the SCRAM mechanism and set the username and
                password; the password is masked on the screen and in the stored settings.</p>
            <p><b>Consumer session timeout, ms</b> defaults to <code>10000</code> (allowed
                <code> 6000</code>–<code>60000</code>). It sets how long Kafka waits for this consumer
                before rejoining the group, and also bounds the wait when the extension is applied
                or disabled.</p>
            <p>The Kafka record value becomes the MQTT payload unchanged; a null value becomes an
                empty payload. Messages are not retained or passed to the export tap. A new consumer
                group starts at the earliest available offset; an existing group resumes from its
                committed offsets. Use separate group IDs if each broker must receive a copy.</p>
            <p>The source commits the Kafka offset after the broker accepts the message into its
                in-memory delivery queue, not after a subscriber receives it. A crash can therefore
                lose a queued publish or cause a duplicate. A refused publish does not advance the offset.</p>
        </section>
    </DocumentationPage>;
}
