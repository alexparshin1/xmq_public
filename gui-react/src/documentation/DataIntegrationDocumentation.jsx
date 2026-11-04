import React from "react";
import DocumentationPage from "./DocumentationPage";

export default function DataIntegrationDocumentation() {
    return <DocumentationPage page="data-integration">
            <section>
            <h4 id="data-integration">Data integration</h4>

            <p>
                A <code>webhook</code> extension posts the messages this broker accepts to an
                HTTP(S) endpoint. It is the first data-integration connector: messages are selected
                by topic filter, the body is sent as JSON, and the request carries an
                HMAC-SHA256 signature when a secret is configured.
            </p>

            <p>
                It is an extension, not a core feature: a broker with no webhook loaded pays
                nothing for it. It is built for Linux, FreeBSD and Windows: the socket layer uses
                Winsock on Windows and the platform's POSIX sockets elsewhere, and TLS is OpenSSL
                on all of them. On Windows the installed fragment names
                <code>lib\xmq-extensions\xmq_webhook.dll</code>; on the other platforms the
                library is <code>libxmq_webhook.so</code>.
            </p>

            <h4 id="data-integration-enable">Enabling it</h4>

            <p>
                The package installs <code>50-webhook.conf.template</code> beside the other
                extension fragments. Copy it to <code>50-webhook.conf</code> and set
                <code>enabled</code> to <code>true</code>:
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
      "allow_http": "false"
    }
  }]
}`}</pre>

            <p>
                <code>url</code> is required. <code>topic_filter</code> defaults to <code>#</code>.
                <code>max_payload</code> drops a message larger than the limit rather than cutting
                it. <code>retries</code> counts attempts after the first. Plain HTTP is refused
                unless <code>allow_http</code> is true, which is meant for a receiver on the same
                machine and for tests, not for a production endpoint.
            </p>

            <h4 id="data-integration-payload">What is sent</h4>

            <pre className="userManualCode">{`{
  "id": "1762790400123-7",
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
                The whole array is one setting, so its quotes are escaped in the configuration
                file; the first destination above sends a plain-text body, the second the standard
                JSON envelope. A messenger that wants JSON of its own - Slack's
                <code>{"{\"text\": ...}"}</code>, for instance - needs that JSON as the template,
                which means a string inside a string.
            </p>

            <p>
                Each entry takes <code>url</code> and, optionally, <code>template</code>,
                <code>sources</code> (<code>messages</code>, <code>log</code> or both),
                <code>topic_filter</code>, <code>log_min_level</code>, <code>log_subjects</code>,
                <code>headers</code>, <code>secret</code>, <code>retries</code>,
                <code>timeout_ms</code>, <code>max_payload</code>, <code>concurrency</code> and
                <code>allow_http</code>. With
                no template the body is the JSON envelope shown above; with one, the body is the
                template with these names replaced:
                <code>{"{{id}}"}</code>, <code>{"{{source}}"}</code>, <code>{"{{timestamp}}"}</code>,
                <code>{"{{topic}}"}</code>, <code>{"{{qos}}"}</code>, <code>{"{{retain}}"}</code>,
                <code>{"{{client_id}}"}</code>, <code>{"{{username}}"}</code>,
                <code>{"{{payload_base64}}"}</code>, <code>{"{{level}}"}</code>, <code>{"{{subject}}"}</code>,
                <code>{"{{message}}"}</code>.
            </p>

            <p>
                Substituted values are JSON-escaped, so a quote or a newline in a message cannot
                break the body. A partially applied template is not attempted: an unknown name is
                left as written, which the endpoint will reject rather than accept as empty.
            </p>

            <p>
                Every destination has its own queue and its own worker threads: a messenger that is
                slow to answer does not hold up the collector beside it, and a destination that falls
                behind drops (and counts) rather than delaying the broker. <code>concurrency</code>
                (1 to 64, four by default) sets how many requests may be in flight to one endpoint,
                which is what keeps a synchronous receiver from capping it at one record per round
                trip. The message tap is registered
                once for all destinations, with the widest filter, so each body is copied to the
                extension before the per-destination filters narrow it. That is the price of several
                destinations in one extension.
            </p>

            <p>
                <b>Prototype caveat.</b> <code>destinations</code> is a JSON document inside a
                string setting: the settings screen shows it as one field, and a
                <code>secret</code> inside it is not masked the way the single-URL
                <code>secret</code> is, because the interface masks whole values, not parts of one.
                Separate connector instances, each with its own masked settings, are the shape this
                should grow into.
            </p>

            <h4 id="data-integration-log">Log records</h4>

            <p>
                The same connector can carry the broker's own log. Turn
                <code>send_log</code> on and choose the lowest level worth sending:
            </p>

            <pre className="userManualCode">{`"send_messages": "true",
"send_log": "true",
"log_min_level": "error",
"log_subjects": ""`}</pre>

            <p>
                Both inputs are independent and share one endpoint; every body says which it is
                with <code>"source"</code>. A log record looks like this:
            </p>

            <pre className="userManualCode">{`{
  "id": "1762790400123-7",
  "source": "log",
  "version": 1,
  "timestamp": "2026-11-03T19:45:00Z",
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

            <p>
                This is deliberately not an MQTT topic such as <code>$SYS/error</code>. The faults
                worth reporting are often the ones that stop MQTT from working - a refused
                connection, an unreachable account database - so routing them through the same
                broker and its ACLs would make the report depend on the thing that failed.
            </p>

            <p>
                <b>Prototype.</b> The log input is the second slice of data integration, and the
                extension ABI it uses (1.2) is not final: the level scale, the subject filter and
                whether several connectors may share one library are all open. Treat it as a way to
                prove the shape, not as an interface to build against.
            </p>

            <h4 id="data-integration-delivery">Delivery</h4>

            <p>
                Delivery is at-most-once: the queue is in memory and bounded, and a restart loses
                what has not been sent. A 2xx is success; 5xx, 429, a timeout or a connection
                failure is retried with backoff; any other 4xx is refused without a retry. A tap
                that cannot keep up drops messages and counts them rather than delaying the broker.
            </p>

            <p>
                This connector does not transform messages, does not speak Kafka or a database, and
                does not replay. Those are further extensions on the same message tap, not settings
                of this one.
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
            </ul>
            </section>
        </DocumentationPage>;
}
