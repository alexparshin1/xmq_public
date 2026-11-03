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
                nothing for it. The extension is POSIX only (Linux and BSD); a Windows build needs
                a Winsock version and is not part of this one.
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
