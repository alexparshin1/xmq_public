import React from "react";
import DocumentationPage from "./DocumentationPage";

export default function DataIntegrationDocumentation() {
    return <DocumentationPage page="data-integration">
            <section>
            <h4 id="data-integration">Data integration</h4>

            <p>
                The <code>webhook</code> extension posts what the broker sees to an HTTP(S) endpoint.
                It has two sources - the MQTT messages the broker accepts, and the broker's own log
                - and one destination with a URL, an optional body template, filters and retries.
                Every request carries an HMAC-SHA256 signature when a secret is configured.
            </p>

            <p>
                It is an extension, not a core feature: a broker with no webhook loaded pays nothing
                for it. It is built for Linux, FreeBSD and Windows: the socket layer uses Winsock on
                Windows and the platform's POSIX sockets elsewhere, and TLS is OpenSSL on all of
                them. On Windows the installed fragment names
                <code>lib\xmq-extensions\xmq_webhook.dll</code>; on the other platforms the library
                is <code>libxmq_webhook.so</code>.
            </p>

            <p>
                All settings are edited on the <b>Extensions</b> page, which builds the form from
                what the extension declares: a URL is a URL, a number is a number, and a value that
                does not have the declared format is refused before anything is written. The same
                checks run in the broker, so a value written by a script is refused for the same
                reason.
            </p>

            <h4 id="data-integration-enable">Enabling it</h4>

            <p>
                The package installs <code>50-webhook.conf.template</code> beside the other
                extension fragments. Copy it to <code>50-webhook.conf</code> and set
                <code>enabled</code> to <code>true</code>, or switch the extension on from the
                <b>Extensions</b> page once it is configured:
            </p>

            <pre className="userManualCode">{`{
  "extensions": [{
    "name": "webhook",
    "library": "/usr/local/lib/xmq-extensions/libxmq_webhook.so",
    "enabled": true,
    "settings": {
      "url": "https://collector.example/hook",
      "secret": "a shared secret",
      "topic_filter": "sensors/#",
      "max_payload": "1048576",
      "timeout_ms": "5000",
      "retries": "3",
      "concurrency": "4",
      "allow_http": "false"
    }
  }]
}`}</pre>

            <h4 id="data-integration-sources">Sources</h4>

            <p>
                <b>Messages.</b> A message is offered to the extension when a client publishes it and
                the broker accepts it - that is, after authentication and after the topic permission
                check. A publication the broker refuses is not sent. The body is copied only after
                the topic filter matched, so a connector watching one topic costs nothing for the
                rest of the broker's traffic. Retained publications are sent like any other, with
                <code>"retain": true</code>, and a retained message that clears a topic is sent with
                an empty body.
            </p>

            <p>
                Messages the broker produces itself are <b>not</b> tapped: the <code>$SYS</code>
                counters, retained-message synchronisation between cluster nodes, copies forwarded
                by a cluster node or a bridge, and the last will. This is deliberate, because one
                publication otherwise arrives several times - once where it was published and again
                on every node it was forwarded to.
            </p>

            <p>
                <b>Log records.</b> The second source is the broker's own log, so one endpoint can be
                told both what clients sent and what the broker could not do. It is not an MQTT topic
                such as <code>$SYS/error</code>: the faults worth reporting are often the ones that
                stop MQTT from working - a refused connection, an unreachable account database - and
                routing them through the same broker and its ACLs would make the report depend on
                the thing that failed. Choose the lowest level worth sending and, optionally, the
                subjects.
            </p>

            <p>
                The <code>sources</code> setting chooses messages, log records, or both; to send
                neither, disable the extension. A body carries <code>"source"</code> when the
                template names it, and the default body tells the two apart by
                <code>"level"</code>. The two taps have separate queues and separate
                threads, so a receiver that cannot keep up with log records does not delay
                messages, and the other way round.
            </p>

            <h4 id="data-integration-payload">What is sent</h4>

            <p>A message looks like this:</p>

            <pre className="userManualCode">{`{
  "topic": "sensors/water/1",
  "message": "aGVsbG8=",
  "level": "message"
}`}</pre>

            <p>
                The payload is base64 because MQTT bodies are binary and JSON strings are not. A
                message has no level of its own, so <code>level</code> says
                <code>message</code>, which is also how a receiver tells a message from a log
                record. <code>X-XMQ-Signature: sha256=&lt;hex&gt;</code> is the HMAC-SHA256 of the
                exact request body, keyed with <code>secret</code>. A receiver should compare it
                in constant time and reject a request without it when one is configured.
            </p>

            <p>A log record looks like this:</p>

            <pre className="userManualCode">{`{
  "topic": "",
  "message": "test-client: refused by an extension",
  "level": "error"
}`}</pre>

            <p>
                These three fields are the default <code>template</code>. A template of your own
                replaces them and can name any of the values listed under
                <a href="#data-integration-template">Body template and headers</a>.
            </p>

            <p>
                <code>log_subjects</code> is a comma-separated list of the broker's log categories
                (<code>connect</code>, <code>publish</code>, <code>server_connections</code>,
                <code>server_events</code>, <code>storage_events</code>,
                <code>cluster_connections</code>, <code>cluster_events</code>); empty means all of
                them. Refused publications and failed logins are the records that matter most, and
                they arrive even when no client can connect.
            </p>

            <h4 id="data-integration-settings">Configuration</h4>

            <p>
                These are the settings of one connector with a single destination. Each is checked
                on the <b>Extensions</b> page and again by the broker; the format column is the
                regular expression the value must match as a whole.
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Setting</th>
                    <th>Type</th>
                    <th>Default</th>
                    <th>Format</th>
                    <th>Meaning</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>url</code></td>
                    <td>string</td>
                    <td>empty</td>
                    <td><code>https?://\S+</code></td>
                    <td>Where the bodies are posted. Required: an empty URL means the connector
                        has nothing to do.</td>
                </tr>
                <tr>
                    <td><code>template</code></td>
                    <td>string</td>
                    <td>empty</td>
                    <td>any</td>
                    <td>Body template; an empty value keeps the default shown under
                        <a href="#data-integration-template">Body template and headers</a>. Names
                        in <code>{"{{...}}"}</code> are replaced.</td>
                </tr>
                <tr>
                    <td><code>headers</code></td>
                    <td>list of strings</td>
                    <td>empty</td>
                    <td>each item is <code>Name: value</code></td>
                    <td>Extra request headers, for example an authorization token. Edited as a
                        list, stored as a JSON array.</td>
                </tr>
                <tr>
                    <td><code>secret</code></td>
                    <td>secret</td>
                    <td>empty</td>
                    <td>any</td>
                    <td>HMAC-SHA256 key for <code>X-XMQ-Signature</code>. Masked in the interface
                        and never logged. Empty means unsigned requests.</td>
                </tr>
                <tr>
                    <td><code>topic_filter</code></td>
                    <td>list of strings</td>
                    <td><code>#</code></td>
                    <td>each item is an MQTT filter</td>
                    <td>Only matching messages are sent; a message matching any item is sent.
                        The interface shows one field per filter, with <b>Add</b> and
                        <b>Remove</b> beside the list - the JSON array is what it stores, not what
                        you type. <code>+</code> and <code>#</code> must each be a whole level, and
                        <code>#</code> the last one. An empty list leaves the setting at its
                        default.</td>
                </tr>
                <tr>
                    <td><code>max_payload</code></td>
                    <td>integer</td>
                    <td><code>1048576</code></td>
                    <td><code>[0-9]+</code></td>
                    <td>Largest body to send, in bytes. Larger messages are dropped and counted,
                        not cut. <code>0</code> means no limit - see the memory note under
                        performance.</td>
                </tr>
                <tr>
                    <td><code>timeout_ms</code></td>
                    <td>integer</td>
                    <td><code>5000</code></td>
                    <td><code>[1-9][0-9]{'{0,5}'}</code></td>
                    <td>Connect and read timeout of one request, 1 ms to 999999 ms.</td>
                </tr>
                <tr>
                    <td><code>retries</code></td>
                    <td>integer</td>
                    <td><code>3</code></td>
                    <td><code>[0-9]|[1-9][0-9]|100</code></td>
                    <td>Attempts after the first one, 0 to 100.</td>
                </tr>
                <tr>
                    <td><code>concurrency</code></td>
                    <td>integer</td>
                    <td><code>4</code></td>
                    <td><code>[1-9]|[1-5][0-9]|6[0-4]</code></td>
                    <td>Requests in flight to this endpoint, 1 to 64. Each is a worker thread
                        waiting for one synchronous reply.</td>
                </tr>
                <tr>
                    <td><code>allow_http</code></td>
                    <td>boolean</td>
                    <td><code>false</code></td>
                    <td><code>true</code> / <code>false</code></td>
                    <td>Permit a plain <code>http://</code> URL. Meant for a receiver on the same
                        machine and for tests, not for a production endpoint.</td>
                </tr>
                <tr>
                    <td><code>sources</code></td>
                    <td>choice</td>
                    <td><code>Send messages</code></td>
                    <td>Send log / Send messages / Send log and messages</td>
                    <td>What is posted: the broker's log, the messages it accepts, or both. To
                        send neither, disable the extension instead.</td>
                </tr>
                <tr>
                    <td><code>log_min_level</code></td>
                    <td>choice</td>
                    <td><code>error</code></td>
                    <td>debug, info, warning, error</td>
                    <td>Lowest level to send from the log.</td>
                </tr>
                <tr>
                    <td><code>log_subjects</code></td>
                    <td>string</td>
                    <td>empty</td>
                    <td><code>[a-z_]+(?:,[a-z_]+)*</code></td>
                    <td>Comma-separated log subjects; empty means all.</td>
                </tr>
                </tbody>
            </table>

            <p>
                <code>_comment</code> keys in the shipped template are not settings and are not
                edited on the page; they are there to explain the file to somebody reading it.
                <code>name</code> and <code>library</code> identify the extension itself and are
                edited in the configuration file, not in the settings form.
            </p>

            <h4 id="data-integration-template">Body template and headers</h4>

            <p>
                The default body is
                <code>{'{"topic":"{{topic}}","message":"{{message}}","level":"{{level}}"}'}</code>,
                and a template of your own replaces it with these names replaced:
                <code>{"{{id}}"}</code>, <code>{"{{source}}"}</code>,
                <code>{"{{timestamp}}"}</code>, <code>{"{{topic}}"}</code>,
                <code>{"{{qos}}"}</code>, <code>{"{{retain}}"}</code>,
                <code>{"{{client_id}}"}</code>, <code>{"{{username}}"}</code>,
                <code>{"{{payload_base64}}"}</code>, <code>{"{{level}}"}</code>,
                <code>{"{{subject}}"}</code>, <code>{"{{message}}"}</code>.
            </p>

            <p>
                For a message, <code>{"{{message}}"}</code> is the base64 payload and
                <code>{"{{level}}"}</code> is the literal <code>message</code>; for a log record,
                <code>{"{{topic}}"}</code> is empty.
            </p>

            <p>
                Substituted values are JSON-escaped, so a quote or a newline in a message cannot
                break the body. An unknown name is left as written, which the endpoint will reject
                rather than accept as empty.
            </p>

            <pre className="userManualCode">{`{
  "url": "https://ntfy.sh/xmq-errors",
  "topic_filter": "[\\"alarms/#\\",\\"sensors/water/+\\"]",
  "template": "{\\"text\\":\\"[{{level}}] {{subject}}: {{message}}\\"}",
  "headers": "[\\"Authorization: Bearer a-token\\"]",
  "sources": "Send log",
  "log_min_level": "warning"
}`}</pre>

            <p>
                A messenger that wants JSON of its own - Slack's <code>{'{"text": ...}'}</code>, for
                instance - needs that JSON as the template, which means a string inside a string:
                the escaped quotes above are what the configuration file holds.
            </p>

            <p>
                <code>topic_filter</code> and <code>headers</code> are lists: the page shows one
                field per item with <b>Add</b> and <b>Remove</b>, and stores them as JSON arrays.
                A message is sent when it matches any filter, so one endpoint receives two
                unrelated subtrees without a wildcard that would also match the rest of the
                broker's traffic.
            </p>

            <pre className="userManualCode">{'"topic_filter": "[\\"sensors/#\\",\\"alarms/+\\"]"'}</pre>

            <p>
                Each header is <code>Name: value</code>; an authorization token is the usual one.
                The header travels with every request to the endpoint, beside the
                <code>X-XMQ-Signature</code> the secret produces.
            </p>

            <h4 id="data-integration-performance">Performance limits</h4>

            <p>
                The connector is built so that a slow or unreachable endpoint costs deliveries and
                not broker throughput. What that means in practice:
            </p>

            <ul>
                <li>
                    <b>The publisher never waits.</b> An accepted message copies its body into the
                    extension's queue and the publish path continues. There is no backpressure: a
                    publisher cannot be slowed down by a receiver, and messages do not wait in the
                    broker for it either.
                </li>
                <li>
                    <b>The message queue holds 4096 messages</b>, and the log queue holds 4096
                    records, each per extension instance. When one is full the oldest work is
                    dropped - the newest message is the one refused - and the drop is counted. The
                    counters for each tap are shown on the <b>Extensions</b> page as
                    <i>messages delivered / dropped</i> and <i>log records delivered / dropped</i>.
                    A non-zero drop count means the endpoint or the network is slower than the
                    traffic that matches the filter.
                </li>
                <li>
                    <b>The destination has a queue of 1024 bodies and <code>concurrency</code>
                    worker threads.</b> A worker waits for one synchronous HTTP reply, so an
                    endpoint through a 10 ms round trip carries about
                    <code>100 × concurrency</code> bodies a second at best. Raise
                    <code>concurrency</code> before raising timeouts.
                </li>
                <li>
                    <b>Memory is bounded by messages, not by bytes.</b> With the default
                    <code>max_payload</code> of 1 MiB, a stalled destination can hold 1024 bodies of
                    up to 1 MiB - and the tap queue 4096 more. Set <code>max_payload</code> to the
                    largest message the deployment actually sends; that is what keeps a stalled
                    endpoint from turning into a memory problem. <code>0</code> removes the limit
                    altogether and should not be used on a broker that accepts untrusted
                    publications.
                </li>
                <li>
                    <b>Base64 adds a third to the body.</b> A 1 MiB payload travels as about
                    1.4 MB of JSON, and the queue counts the message once, before encoding.
                </li>
                <li>
                    <b>The tap is registered with every filter <code>topic_filter</code> names</b>,
                    not with <code>#</code>. A body is copied into the extension only when it
                    matches at least one of them. Each filter costs one comparison per publication;
                    a topic nobody wants costs nothing else.
                </li>
                <li>
                    <b>Delivery is at-most-once.</b> The queues are in memory and a restart loses
                    what has not been sent. A 2xx is success; 5xx, 429, a timeout or a connection
                    failure is retried with backoff; any other 4xx is refused without a retry.
                </li>
            </ul>

            <p>
                For sizing: the extension costs one copy of each matching body on the publish path,
                plus the queued copies while a destination is behind. A broker whose only work is
                forwarding small messages will not notice it; a broker forwarding large binary
                payloads at a high rate should name a narrow <code>topic_filter</code> and a
                realistic <code>max_payload</code>, and watch the drop counters.
            </p>

            <h4 id="data-integration-security">Security</h4>

            <ul>
                <li>Use an HTTPS endpoint with a certificate the broker trusts; the system CA store
                    is used and verification is mandatory.</li>
                <li>Treat <code>secret</code> as a password: it is a secret setting, masked in the
                    interface and never logged.</li>
                <li>A URL set by an administrator can reach internal services; restrict the
                    endpoint or the network if that matters.</li>
                <li>The payload is the message body: do not point this at an endpoint that logs
                    bodies containing credentials.</li>
                <li>A header value is stored as an ordinary string, not as a masked secret, so an
                    authorization token in <code>headers</code> is visible to anyone who can read
                    the configuration file. The <code>secret</code> setting is masked; a token that
                    must not be readable there belongs in that signature instead.</li>
            </ul>

            <h4 id="data-integration-delivery">What it is not</h4>

            <p>
                This connector does not transform messages, does not speak Kafka or a database, and
                does not replay. Those are further extensions on the same message tap, not settings
                of this one.
            </p>
            </section>
        </DocumentationPage>;
}
