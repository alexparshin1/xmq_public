/* Copyright (c) 2026 Alexey Parshin. MPL-2.0. */
#include "../HttpAuth.cpp"
#include <cstring>
#include <future>
#include <iostream>
#include <thread>

namespace {
std::map<std::string, std::string> settings{{"url", "http://127.0.0.1/auth"}};
int64_t getSetting(void*, const char* name, char* buffer, size_t capacity)
{
    const auto entry = settings.find(name);
    if (entry == settings.end()) return -1;
    if (buffer && capacity > entry->second.size())
        std::memcpy(buffer, entry->second.c_str(), entry->second.size() + 1);
    return entry->second.size();
}
void expect(bool result, const char* message)
{
    if (!result) throw std::runtime_error(message);
}
xmq_host host()
{
    xmq_host result{};
    result.struct_size = sizeof(result);
    result.setting = getSetting;
    return result;
}
class Mock final : public HttpAuthenticator
{
public:
    Mock() : HttpAuthenticator(host()) {}
    int calls{0};
    HttpReply response{XMQ_AUTH_ALLOW, {"Readers"}};
    std::string payload;
protected:
    HttpReply fetch(const HttpConfig&, const sptk::Buffer& body) override
    {
        ++calls;
        payload.assign(body.c_str(), body.bytes());
        return response;
    }
};
xmq_auth_decision auth(HttpAuthenticator& extension, const std::string& username,
                       const std::string& password, std::vector<std::string>& groups,
                       const std::string& client = "test-client")
{
    const std::string address = "127.0.0.1";
    xmq_auth_request request{};
    request.username = {username.data(), username.size()};
    request.password = {password.data(), password.size()};
    request.client_id = {client.data(), client.size()};
    request.remote_address = {address.data(), address.size()};
    request.protocol_version = 5;
    return extension.authenticateWithGroups(request, groups);
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 || argc == 3)
        {
            settings = {{"url", argv[1]}, {"cache_ttl_seconds", "0"},
                        {"negative_cache_ttl_seconds", "0"}, {"timeout_seconds", "1"}};
            if (argc == 3)
                settings["ca_file"] = argv[2];
            HttpAuthenticator extension(host());
            expect(extension.start(), "network configuration");
            std::vector<std::string> groups;
            const std::string endpoint(argv[1]);
            if (argc == 2 && endpoint.starts_with("https://"))
            {
                // An untrusted certificate is refused before any request; the local stand uses a
                // private CA and is run with it as the second argument.
                expect(auth(extension, "http/alice", "secret", groups) == XMQ_AUTH_SUBSYSTEM_ERROR,
                       "an untrusted TLS certificate is refused");
                std::cout << "HTTP auth TLS verification test passed\n";
                return 0;
            }
            expect(auth(extension, "http/alice", "secret", groups) == XMQ_AUTH_ALLOW &&
                   groups == std::vector<std::string>{"Readers"}, "real HTTP POST allow");
            expect(auth(extension, "http/bad", "secret", groups) == XMQ_AUTH_DENY && groups.empty(),
                   "real HTTP deny");
            expect(auth(extension, "http/outage", "secret", groups) == XMQ_AUTH_SUBSYSTEM_ERROR,
                   "real HTTP 503");
            expect(auth(extension, "http/invalid", "secret", groups) == XMQ_AUTH_SUBSYSTEM_ERROR,
                   "real malformed JSON");
            const auto started = Clock::now();
            expect(auth(extension, "http/slow", "secret", groups) == XMQ_AUTH_SUBSYSTEM_ERROR,
                   "real HTTP timeout");
            expect(Clock::now() - started < std::chrono::seconds(2), "bounded HTTP wait");
            std::cout << "HTTP auth network tests passed\n";
            return 0;
        }
        expect(parseReply(200, sptk::Buffer(R"({"decision":"allow","groups":["Readers","Readers"]})")).groups.size() == 1,
               "group deduplication");
        expect(parseReply(200, sptk::Buffer(R"({"decision":"allow","groups":[7]})")).decision ==
               XMQ_AUTH_SUBSYSTEM_ERROR, "group types checked");
        expect(parseReply(200, sptk::Buffer(R"({"nested":{"decision":"allow"}})")).decision ==
               XMQ_AUTH_SUBSYSTEM_ERROR, "decision must be direct member");
        expect(parseReply(200, sptk::Buffer(R"({"decision":"deny"})")).decision == XMQ_AUTH_DENY, "deny");
        expect(parseReply(200, sptk::Buffer(R"({"decision":"not_handled"})")).decision == XMQ_AUTH_NOT_HANDLED, "abstain");
        expect(parseReply(403, sptk::Buffer()).decision == XMQ_AUTH_DENY, "403 denies");
        expect(parseReply(503, sptk::Buffer()).decision == XMQ_AUTH_SUBSYSTEM_ERROR, "503 unavailable");
        expect(parseReply(302, sptk::Buffer()).decision == XMQ_AUTH_SUBSYSTEM_ERROR, "no redirects");
        std::vector<std::string> groups{"stale"};
        Mock extension;
        expect(extension.start(), "configuration");
        expect(auth(extension, "admin", "secret", groups) == XMQ_AUTH_NOT_HANDLED &&
               groups.empty() && extension.calls == 0, "local namespace bypasses HTTP");
        expect(auth(extension, "http/alice", "s\"ecret", groups) == XMQ_AUTH_ALLOW, "allow");
        sptk::xdoc::Document payload;
        payload.load(sptk::Buffer(extension.payload));
        expect(payload.root()->findFirst("username")->getString() == "alice" &&
               payload.root()->findFirst("password")->getString() == "s\"ecret", "JSON and prefix");
        auth(extension, "http/alice", "s\"ecret", groups);
        expect(extension.calls == 1 && groups == std::vector<std::string>{"Readers"}, "allow cached with groups");
        auth(extension, "http/alice", "different", groups);
        auth(extension, "http/alice", "different", groups, "different-client");
        expect(extension.calls == 3, "password and client id are cache key dimensions");
        expect(extension.reload(), "reload");
        extension.response = {XMQ_AUTH_SUBSYSTEM_ERROR, {}};
        auth(extension, "http/alice", "secret", groups);
        auth(extension, "http/alice", "secret", groups);
        expect(extension.calls == 5, "outages not cached");
        extension.response = {XMQ_AUTH_DENY, {}};
        auth(extension, "http/alice", "secret", groups);
        auth(extension, "http/alice", "secret", groups);
        expect(extension.calls == 6 && groups.empty(), "deny cached");
        settings["cache_ttl_seconds"] = "0";
        expect(extension.reload(), "cache off reload");
        extension.response = {XMQ_AUTH_ALLOW, {"Readers"}};
        auth(extension, "http/alice", "secret", groups);
        auth(extension, "http/alice", "secret", groups);
        expect(extension.calls == 8, "allow cache disabled");
        settings["url"] = "http://example.org/auth";
        expect(!extension.reload(), "plaintext remote endpoint rejected");
        extension.stop();
        expect(auth(extension, "http/alice", "secret", groups) == XMQ_AUTH_SUBSYSTEM_ERROR, "stopped");
        std::cout << "HTTP auth tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "HTTP auth test failed: " << error.what() << '\n';
        return 1;
    }
}
