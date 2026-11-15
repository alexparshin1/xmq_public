import React from "react";
import {Link} from "react-router-dom";
import DocumentationPage from "./DocumentationPage";

/**
 * @brief The webhook extension: the one shipped data-integration path.
 *
 * It replaces the former Data Integration page, whose `destinations` array was a prototype that
 * was removed: one extension posts to one endpoint, and an operator who wants several receivers
 * puts a small fan-out service in front of this one. The body, the signature and the delivery
 * policy are what an integrator has to implement, so they come before the settings table.
 */
export default function WebhookDocumentation() {
    return <DocumentationPage page="webhook">
        <section>
            <h4 id="webhook">Webhook and data integration</h4>

            <p>
                The <code>webhook</code> extension posts what the broker sees to one HTTP(S)
                endpoint. It has two inputs - the MQTT messages the broker accepts, and the
                broker's own log records - and either or both can be sent. Every request carries
                an HMAC-SHA256 signature when a secret is configured.
            </p>

            <p>
                It is an extension, not a core feature: a broker with no webhook loaded pays
                nothing for it. It is built for Linux, FreeBSD and Windows: the socket layer uses
                Winsock on Windows and the platform's POSIX sockets elsewhere, and TLS is OpenSSL
                on all of them. On Windows the installed fragment names
                <code>lib\xmq-extensions\xmq_webhook.dll</code>; on the other platforms the
                library is <code>libxmq_webhook.so</code>.
            </p>

            <p>
                One extension is one endpoint. To send the same stream to several receivers, put a
                small fan-out service in front of this one; it is a better place for per-receiver
                filters and credentials than a JSON document typed into a settings field.
            </p>
        </section>

        <section>
            <h4 id="webhook-enable">Enabling it</h4>

            <p>
                The shipped fragment <code>xmq_extensions.d/50-webhook.conf</code> is disabled and
                has an empty URL, so nothing is posted until both are set. Either edit that file,
                or switch the extension on in the interface - which writes
                <code>xmq_extensions.d/90-webhook.conf</code> - and fill in the settings there.
                Loading, ordering and the <b>Required</b> flag are described on the
                <Link to="/xmq_doc_extensions">Extensions</Link> page.
            </p>

            <p>
                A URL that is missing or malformed is refused and logged when the extension starts;
                it does not start in silence and lose records. The same holds for a plain
                <code>http://</code> URL while <code>allow_http</code> is false.
            </p>
        </section>

        <section>
            <h4 id="webhook-sources">What is sent</h4>

            <p>
                <code>sources</code> chooses the input. <b>Send messages</b> posts the messages the
                broker accepted, <b>Send log</b> posts the broker's log records, and <b>Send log and
                messages</b> posts both, told apart by the <code>source</code> field in the body.
                There is no "neither": to send nothing, disable the extension.
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Setting</th>
                    <th>Narrows</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>topic_filter</code></td>
                    <td>Messages. A list of MQTT filters; a message matching any item is sent.
                        <code>+</code> and <code>#</code> must each be a whole level, and
                        <code>#</code> the last one.</td>
                </tr>
                <tr>
                    <td><code>max_payload</code></td>
                    <td>Messages. Larger bodies are dropped and counted, not truncated.</td>
                </tr>
                <tr>
                    <td><code>log_min_level</code></td>
                    <td>Log records. <code>debug</code>, <code>info</code>, <code>warning</code> or
                        <code>error</code>, the lowest level that is sent.</td>
                </tr>
                <tr>
                    <td><code>log_subjects</code></td>
                    <td>Log records. A comma-separated list of the broker's log subjects, for
                        example <code>publish,connect,server_connections</code>; empty means all
                        of them.</td>
                </tr>
                </tbody>
            </table>

            <p>
                The log input is what makes an unreachable-account or refused-publication incident
                visible: refusals from the account database, the permission evaluator and the
                extension host arrive even when no client can connect, because they are records of
                why a message does not exist.
            </p>
        </section>

        <section>
            <h4 id="webhook-payload">What the endpoint receives</h4>

            <p>
                A POST with <code>Content-Type: application/json</code> and
                <code>User-Agent: xmq-webhook/1.2</code>. With no template set, a message arrives
                as:
            </p>

            <pre className="userManualCode">{`{"topic":"sensors/water/1","message":"aGVsbG8=","level":"message"}`}</pre>

            <p>
                The payload is base64 because MQTT bodies are binary and JSON strings are not, and
                a message has no severity of its own, so <code>level</code> is the literal
                <code>message</code> - which is also what tells the two inputs apart at the
                receiver. A log record arrives in the same three fields: <code>topic</code> empty,
                <code>message</code> the log text, and <code>level</code> its severity.
            </p>

            <p>
                A 2xx answer means the record was delivered. The body is not interpreted otherwise.
                The connection is kept open between records and re-established if the endpoint
                closes it while idle.
            </p>
        </section>

        <section>
            <h4 id="webhook-template">Body templates</h4>

            <p>
                Set <code>template</code> when the receiver wants its own envelope. Names written as
                <code>{"{{name}}"}</code> are replaced, and the value is JSON-escaped, so a quote or
                a newline in a topic cannot break the body:
            </p>

            <pre className="userManualCode">{`"template": "{\\"text\\":\\"[{{level}}] {{subject}}: {{message}}\\",\\"topic\\":\\"{{topic}}\\"}"`}</pre>

            <p>
                The names available for a message are <code>id</code>, <code>source</code>,
                <code>timestamp</code>, <code>topic</code>, <code>qos</code>,
                <code>retain</code>, <code>client_id</code>, <code>username</code>,
                <code>payload_base64</code>, <code>message</code> and <code>level</code>. A log
                record has <code>id</code>, <code>source</code>, <code>timestamp</code>,
                <code>level</code>, <code>subject</code> and <code>message</code>, with
                <code>topic</code> empty. For a message, <code>message</code> and
                <code>payload_base64</code> are the same base64 text.
            </p>

            <p>
                A name the template does not know is left exactly as written rather than replaced
                with nothing, so a typo fails at the receiver instead of silently emptying a field.
                One template serves both inputs, which is why the default one names only fields
                that exist in both.
            </p>
        </section>

        <section>
            <h4 id="webhook-signature">Proving the sender</h4>

            <p>
                With <code>secret</code> set, every request carries
                <code>X-XMQ-Signature: sha256=&lt;hex&gt;</code>, the HMAC-SHA256 of the exact
                request body keyed with that secret. A receiver should compute the same digest over
                the bytes it received and compare in constant time; it should reject a request with
                no signature when one is expected.
            </p>

            <p>
                <code>secret</code> is a secret setting: the interface masks it and the broker keeps
                it out of the log. Empty means unsigned requests, which is acceptable only on a
                link that is already trusted.
            </p>

            <p>
                <code>headers</code> adds request headers of your own, one <code>Name: value</code>
                item per entry - an authorization token for the receiver, for example. A header
                that is malformed is refused when the settings are applied.
            </p>
        </section>

        <section>
            <h4 id="webhook-settings">Configuration</h4>

            <p>
                These are the settings of the extension. Each is checked on the
                <b>Extensions</b> page and again by the broker; the format column is what the value
                must match as a whole.
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Setting</th>
                    <th>Type</th>
                    <th>Default</th>
                    <th>Meaning</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>url</code></td>
                    <td>string</td>
                    <td>empty</td>
                    <td>Where the bodies are posted, <code>http://</code> or
                        <code>https://</code>. Empty means the extension has nothing to do.</td>
                </tr>
                <tr>
                    <td><code>sources</code></td>
                    <td>choice</td>
                    <td>Send messages</td>
                    <td>Send log, Send messages, or Send log and messages.</td>
                </tr>
                <tr>
                    <td><code>template</code></td>
                    <td>string</td>
                    <td>the three-field JSON body</td>
                    <td>Body to post, with <code>{"{{...}}"}</code> names substituted and
                        JSON-escaped.</td>
                </tr>
                <tr>
                    <td><code>headers</code></td>
                    <td>list of strings</td>
                    <td>empty</td>
                    <td>Extra request headers, one <code>Name: value</code> per item.</td>
                </tr>
                <tr>
                    <td><code>secret</code></td>
                    <td>secret</td>
                    <td>empty</td>
                    <td>HMAC-SHA256 key for <code>X-XMQ-Signature</code>. Masked in the interface
                        and never logged.</td>
                </tr>
                <tr>
                    <td><code>topic_filter</code></td>
                    <td>list of strings</td>
                    <td><code>#</code></td>
                    <td>Which messages are sent; one filter per item. The interface shows one field
                        per filter, with <b>Add</b> and <b>Remove</b> beside the list.</td>
                </tr>
                <tr>
                    <td><code>max_payload</code></td>
                    <td>integer</td>
                    <td><code>1048576</code></td>
                    <td>Largest message body to send, in bytes; larger ones are dropped and
                        counted. <code>0</code> means no limit.</td>
                </tr>
                <tr>
                    <td><code>timeout_ms</code></td>
                    <td>integer</td>
                    <td><code>5000</code></td>
                    <td>Connect and read timeout of one request, 1 ms to 999999 ms.</td>
                </tr>
                <tr>
                    <td><code>retries</code></td>
                    <td>integer</td>
                    <td><code>3</code></td>
                    <td>Attempts after the first one, 0 to 100.</td>
                </tr>
                <tr>
                    <td><code>concurrency</code></td>
                    <td>integer</td>
                    <td><code>4</code></td>
                    <td>Requests in flight to the endpoint, 1 to 64. Each is a worker thread
                        waiting for one synchronous reply.</td>
                </tr>
                <tr>
                    <td><code>allow_http</code></td>
                    <td>boolean</td>
                    <td><code>false</code></td>
                    <td>Permit a plain <code>http://</code> URL. Meant for a receiver on the same
                        machine and for tests, not for a production endpoint.</td>
                </tr>
                <tr>
                    <td><code>log_min_level</code></td>
                    <td>choice</td>
                    <td><code>error</code></td>
                    <td>Lowest log level to send: debug, info, warning or error.</td>
                </tr>
                <tr>
                    <td><code>log_subjects</code></td>
                    <td>string</td>
                    <td>empty</td>
                    <td>Comma-separated log subjects; empty means all.</td>
                </tr>
                </tbody>
            </table>

            <p>
                <code>_comment</code> keys in the shipped fragment are not settings and are not
                edited on the page; <code>name</code> and <code>library</code> identify the
                extension itself and live in the configuration file.
            </p>
        </section>

        <section>
            <h4 id="webhook-delivery">Delivery, and what is lost</h4>

            <p>
                The publish path never waits for the endpoint. An accepted message copies its body
                into the extension's queue and the broker continues; a slow or unreachable receiver
                costs deliveries, not broker throughput.
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Answer or failure</th>
                    <th>What happens</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td>HTTP 2xx</td>
                    <td>Delivered. Counted as sent.</td>
                </tr>
                <tr>
                    <td>HTTP 5xx, 429, timeout, connection failure</td>
                    <td>Retried, up to <code>retries</code> times, with a backoff that starts at
                        100 ms and doubles to at most 2 s.</td>
                </tr>
                <tr>
                    <td>Any other 4xx</td>
                    <td>Refused without a retry: the endpoint would say no again.</td>
                </tr>
                <tr>
                    <td>The queue is full</td>
                    <td>The newest record is dropped and the drop counter rises. Delivery is
                        at-most-once and a restart loses what has not been sent.</td>
                </tr>
                </tbody>
            </table>

            <p>
                The extension holds 1024 bodies in its queue, and <code>concurrency</code> workers
                send them. A worker waits for one synchronous reply, so an endpoint 10 ms away
                carries about <code>100 × concurrency</code> bodies a second at best; raise
                <code>concurrency</code> before raising timeouts.
            </p>
        </section>

        <section>
            <h4 id="webhook-limits">What it is not, and what it costs</h4>

            <ul>
                <li>
                    <b>One endpoint, no transformations.</b> The body arrives as it was published, or
                    in the fixed shape the template gives it. There is no rule language, no JSONPath
                    and no per-record routing. Kafka, database and object-store connectors are
                    further extensions on the same message tap.
                </li>
                <li>
                    <b>No replay and no durable queue.</b> The queues are in memory, bounded by
                    count rather than bytes. With the default <code>max_payload</code> of 1 MiB and
                    a stalled endpoint, the queued bodies can be a gigabyte of memory: set
                    <code>max_payload</code> to the largest message the deployment really sends.
                </li>
                <li>
                    <b>Base64 adds a third to the body.</b> A 1 MiB payload travels as roughly
                    1.4 MB of JSON.
                </li>
                <li>
                    <b>HTTPS is verified.</b> Certificates are checked against the system store, so
                    a private CA has to be installed where the broker can see it.
                </li>
                <li>
                    <b>A URL set by an administrator can reach internal services.</b> The payload is
                    the message body, so do not point this at an endpoint that logs bodies which
                    carry credentials.
                </li>
            </ul>

            <p>
                The counters for each input are shown on the <b>Extensions</b> page as messages and
                log records delivered and dropped. A non-zero drop count means the endpoint or the
                network is slower than the traffic that matches the filters.
            </p>
        </section>
    </DocumentationPage>;
}
