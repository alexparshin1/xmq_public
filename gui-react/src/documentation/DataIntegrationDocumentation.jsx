import React from "react";
import DocumentationPage from "./DocumentationPage";

export default function DataIntegrationDocumentation() {
    return <DocumentationPage page="data-integration">
            <section>
            <h4 id="data-integration">Data integration</h4>

            <p>
                The <code>webhook</code> extension posts what the broker sees to an HTTP(S) endpoint.
                It has two sources - the MQTT messages the broker accepts, and the broker's own log
                - and one or more destinations, each with its own URL, body template, filters and
                retries. Every request carries an HMAC-SHA256 signature when a secret is configured.
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
                reason. The <code>destinations</code> array is the exception: it is one JSON
                document inside one setting, and is described below.
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
                Both sources are independent and share one endpoint; every body says which it is
                with <code>"source"</code>. The two taps have separate queues and separate threads,
                so a receiver that cannot keep up with log records does not delay messages, and the
                other way round.
            </p>

            <h4 id="data-integration-payload">What is sent</h4>

            <pre className="userManualCode">{`{
  "id": "1762790400123-7",
  "source": "message",
  "version": 1,
  "timestamp": "2026-11-03T19:45:00Z",
  "topic": "sensors/water/1",
  "qos": 1,
  "retain": false,
  "client_id": "device-1",
  "username": "user7",
  "payload_base64": "aGVsbG8="
}`}</pre>

            <p>
                The payload is base64 because MQTT bodies are binary and JSON strings are not.
                <code>X-XMQ-Signature: sha256=&lt;hex&gt;</code> is the HMAC-SHA256 of the exact
                request body, keyed with <code>secret</code>. A receiver should compare it in
                constant time and reject a request without it when one is configured.
            </p>

            <p>A log record looks like this:</p>

            <pre className="userManualCode">{`{
  "id": "1762790400123-8",
  "source": "log",
  "version": 1,
  "timestamp": "2026-11-03T19:45:01Z",
  "level": "error",
  "subject": "server_connections",
  "message": "test-client: refused by an extension"
}`}</pre>

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
                    <td><code>destinations</code></td>
                    <td>string</td>
                    <td>empty</td>
                    <td>JSON array</td>
                    <td>Several endpoints, each with its own settings; replaces every setting
                        below except the two <code>send_*</code> switches only through its own
                        <code>sources</code> field. See the next section.</td>
                </tr>
                <tr>
                    <td><code>url</code></td>
                    <td>string</td>
                    <td>empty</td>
                    <td><code>https?://\S+</code></td>
                    <td>Where the bodies are posted. Required when <code>destinations</code> is
                        empty; an empty URL means the connector has nothing to do.</td>
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
                    <td>string</td>
                    <td><code>#</code></td>
                    <td>an MQTT filter, or a JSON array of them</td>
                    <td>Only matching messages are sent. <code>+</code> and <code>#</code> must
                        each be a whole level, and <code>#</code> the last one. With an array, a
                        message matching any of the filters is sent; a comma cannot separate them,
                        because a comma is a legal character in a topic.</td>
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
                    <td><code>send_messages</code></td>
                    <td>boolean</td>
                    <td><code>true</code></td>
                    <td><code>true</code> / <code>false</code></td>
                    <td>Post the messages the broker accepts.</td>
                </tr>
                <tr>
                    <td><code>send_log</code></td>
                    <td>boolean</td>
                    <td><code>false</code></td>
                    <td><code>true</code> / <code>false</code></td>
                    <td>Post the broker's log records as well.</td>
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

            <h4 id="data-integration-destinations">Several destinations</h4>

            <p>
                One connector can post to more than one endpoint, each with its own body template
                and its own filters. Set <code>destinations</code> to a JSON array; it replaces the
                single-URL settings entirely:
            </p>

            <pre className="userManualCode">{`"destinations": "[
  {"url": "https://ntfy.sh/xmq-errors",
   "sources": "log",
   "log_min_level": "error",
   "template": "[{{level}}] {{subject}}: {{message}}"},

  {"url": "https://collector.example/hook",
   "sources": "messages",
   "topic_filter": "sensors/#",
   "secret": "a shared secret"}
]"`}</pre>

            <p>
                One destination can also name several filters, as a JSON array inside the string:
            </p>

            <pre className="userManualCode">{`"topic_filter": "[\"sensors/#\",\"alarms/+\"]"`}</pre>

            <p>
                A message is sent when it matches any of them, so one endpoint receives two
                unrelated subtrees without a wildcard that would also match the rest of the
                broker's traffic. The whole array is one setting, so its quotes are escaped in the
                configuration file; the first destination above sends a plain-text body, the second
                the standard JSON envelope. A messenger that wants JSON of its own - Slack's
                <code>{"{\"text\": ...}"}</code>, for instance - needs that JSON as the template,
                which means a string inside a string.
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Field</th>
                    <th>Default</th>
                    <th>Meaning</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>url</code></td>
                    <td>required</td>
                    <td>The endpoint, <code>http://</code> or <code>https://</code>.</td>
                </tr>
                <tr>
                    <td><code>sources</code></td>
                    <td><code>messages</code></td>
                    <td>Comma-separated: <code>messages</code>, <code>log</code>, or both.</td>
                </tr>
                <tr>
                    <td><code>template</code></td>
                    <td>empty</td>
                    <td>Body template; empty means the JSON envelope. Names in
                        <code>{"{{...}}"}</code> are replaced, see below.</td>
                </tr>
                <tr>
                    <td><code>topic_filter</code></td>
                    <td><code>#</code></td>
                    <td>Which messages this destination wants: one filter, or a JSON array of
                        them.</td>
                </tr>
                <tr>
                    <td><code>log_min_level</code></td>
                    <td><code>error</code></td>
                    <td>Lowest log level this destination wants.</td>
                </tr>
                <tr>
                    <td><code>log_subjects</code></td>
                    <td>empty</td>
                    <td>Comma-separated log subjects; empty means all.</td>
                </tr>
                <tr>
                    <td><code>headers</code></td>
                    <td>empty</td>
                    <td>An object of extra request headers, for example an authorization
                        token.</td>
                </tr>
                <tr>
                    <td><code>secret</code></td>
                    <td>empty</td>
                    <td>HMAC key for this destination.</td>
                </tr>
                <tr>
                    <td><code>retries</code></td>
                    <td><code>3</code></td>
                    <td>Attempts after the first one.</td>
                </tr>
                <tr>
                    <td><code>timeout_ms</code></td>
                    <td><code>5000</code></td>
                    <td>Connect and read timeout of one request.</td>
                </tr>
                <tr>
                    <td><code>max_payload</code></td>
                    <td><code>0</code></td>
                    <td>Largest message body; 0 means no limit.</td>
                </tr>
                <tr>
                    <td><code>concurrency</code></td>
                    <td><code>4</code></td>
                    <td>Requests in flight, 1 to 64.</td>
                </tr>
                <tr>
                    <td><code>allow_http</code></td>
                    <td><code>false</code></td>
                    <td>Permit a plain HTTP URL for this destination.</td>
                </tr>
                </tbody>
            </table>

            <p>
                With no template the body is the JSON envelope shown above; with one, the body is
                the template with these names replaced: <code>{"{{id}}"}</code>,
                <code>{"{{source}}"}</code>, <code>{"{{timestamp}}"}</code>,
                <code>{"{{topic}}"}</code>, <code>{"{{qos}}"}</code>, <code>{"{{retain}}"}</code>,
                <code>{"{{client_id}}"}</code>, <code>{"{{username}}"}</code>,
                <code>{"{{payload_base64}}"}</code>, <code>{"{{level}}"}</code>,
                <code>{"{{subject}}"}</code>, <code>{"{{message}}"}</code>.
            </p>

            <p>
                Substituted values are JSON-escaped, so a quote or a newline in a message cannot
                break the body. An unknown name is left as written, which the endpoint will reject
                rather than accept as empty.
            </p>

            <p>
                <b>Prototype caveat.</b> <code>destinations</code> is a JSON document inside a
                string setting: the settings screen shows it as one field, and a
                <code>secret</code> inside it is not masked the way the single-URL
                <code>secret</code> is, because the interface masks whole values, not parts of one.
                Separate connector instances, each with its own masked settings, are the shape this
                should grow into.
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
                    <b>Each destination has a queue of 1024 bodies and <code>concurrency</code>
                    worker threads.</b> A worker waits for one synchronous HTTP reply, so a
                    destination through a 10 ms round trip carries about
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
                    <b>The tap is registered once for all destinations</b>, with the union of every
                    filter they named - not with <code>#</code>. A body is copied into the extension
                    only when it matches at least one of those filters, and the per-destination
                    filters narrow it afterwards. Each filter costs one comparison per publication;
                    a topic no destination wants costs nothing else.
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
                <li>The <code>destinations</code> array is stored as one string, so a secret inside
                    it is visible to anyone who can read the configuration file. Prefer the
                    single-URL form, whose secret is masked, until the connector grows separate
                    instances.</li>
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
