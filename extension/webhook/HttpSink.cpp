/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

/**
 * @file HttpSink.cpp
 * @brief The HTTP delivery sink for the export extensions.
 *
 * Built from the same Webhook.cpp as the export extensions, with XMQ_SINK_BUILD
 * selecting this descriptor. The source extension owns the SQL engine and the
 * rule editor; this one only posts the JSON the rule produced.
 */

#include "Webhook.cpp"

namespace {

const xmq_setting sinkSettings[] = {
    {.name = "url", .label = "URL", .description = "Where accepted messages are posted, http:// or https://",
     .type = XMQ_SETTING_STRING, .default_value = "", .pattern = "https?://\\S+"},
    {.name = "template", .label = "Body template", .description = "Body to post: {{topic}}, {{message}}, {{level}} and the other names are "
     "substituted and JSON-escaped. For a message {{message}} is the base64 payload and {{level}} is \"message\"; for a log record {{topic}} is empty",
     .type = XMQ_SETTING_STRING, .default_value = defaultTemplate},
    {.name = "headers", .label = "Headers",
     .description = "Add custom request headers as Name: value, for example Authorization: Bearer …. "
                    "Leave the list empty to send no custom headers. The HTTP sink still adds standard HTTP "
                    "headers, including Content-Type: application/json. If a secret is configured, it "
                    "also adds X-XMQ-Signature.",
     .type = XMQ_SETTING_LIST, .default_value = "", .pattern = "[-A-Za-z0-9!#$%&'*+.^_`|~]+: *\\S.*"},
    {.name = "secret", .label = "Secret", .description = "HMAC-SHA256 key for the X-XMQ-Signature header",
     .type = XMQ_SETTING_SECRET, .default_value = ""},
    {.name = "timeout_ms", .label = "Timeout", .description = "HTTP connect and read timeout, in milliseconds",
     .type = XMQ_SETTING_INTEGER, .default_value = "5000", .pattern = "[1-9][0-9]{0,5}"},
    {.name = "retries", .label = "Retries", .description = "Attempts after the first one, 0 to 100",
     .type = XMQ_SETTING_INTEGER, .default_value = "3", .pattern = "[0-9]|[1-9][0-9]|100"},
    {.name = "allow_http", .label = "Allow HTTP", .description = "Permit a plain HTTP URL (for a local receiver)",
     .type = XMQ_SETTING_BOOLEAN, .default_value = "false"},
    {.name = "concurrency", .label = "Concurrency", .description = "How many requests may be in flight at once, 1 to 64",
     .type = XMQ_SETTING_INTEGER, .default_value = "4", .pattern = "[1-9]|[1-5][0-9]|6[0-4]"},
};

struct HttpDelivery
{
    Destination config;
    std::unique_ptr<Peer> peer;
};

const XmqSinkV1 deliveryApi{
    [](XmqSinkV1::Setting get, void* context, char* error, size_t capacity) -> void*
    {
        try
        {
            auto self = std::make_unique<HttpDelivery>();
            auto& config = self->config;
            std::string detail;
            if (!startSockets())
                throw std::runtime_error("Cannot initialize HTTP sockets");
            if (!parseUrl(get(context, "url"), config.url))
                throw std::runtime_error("url must be an http:// or https:// address");
            if (!config.url.m_tls && std::string_view(get(context, "allow_http")) != "true")
                throw std::runtime_error("plain HTTP is refused unless allow_http is true");
            if (!parseHeaders(get(context, "headers"), config.headers, detail))
                throw std::runtime_error(detail);
            config.secret = get(context, "secret");
            config.timeoutMs = std::stoi(get(context, "timeout_ms"));
            config.retries = std::stoi(get(context, "retries"));
            if (config.timeoutMs <= 0 || config.retries < 0 || config.retries > 100)
                throw std::runtime_error("invalid HTTP timeout or retry limit");
            self->peer = std::make_unique<Peer>(config.url, config.secret, config.timeoutMs, config.headers);
            return self.release();
        }
        catch (const std::exception& exception)
        {
            if (error && capacity)
                std::snprintf(error, capacity, "%s", exception.what());
            return nullptr;
        }
    },
    [](void* instance) { delete static_cast<HttpDelivery*>(instance); },
    [](void* instance, const char* body, size_t length, const char* const*, const char* const*, size_t) -> int
    {
        try
        {
            auto& self = *static_cast<HttpDelivery*>(instance);
            return deliverOnce(*self.peer, self.config, std::string(body, length), "export record") ? 0 : 1;
        }
        catch (...)
        {
            return 1;
        }
    }
};

} // namespace

extern "C" XMQ_EXTENSION_EXPORT const XmqSinkV1* xmq_sink_v1()
{
    return &deliveryApi;
}

namespace {
class HttpSink : public xmq::XmqExtensionBase
{
public:
    using XmqExtensionBase::XmqExtensionBase;
    bool start() override { return false; }
};
}

XMQ_DEFINE_EXTENSION_IMPL(HttpSink, "http_sink", "1.2", XMQ_CAP_SINK,
                          "Posts JSON from an export rule to an HTTP(S) endpoint.",
                          sinkSettings, sizeof(sinkSettings) / sizeof(sinkSettings[0]), nullptr, 0)
