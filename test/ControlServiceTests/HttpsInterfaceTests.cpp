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

#include "ControlServiceTests.h"
#include "common/DirectoryNames.h"
#include "common/SocketFactory.h"
#include "server/ControlServiceListener.h"
#include "server/SelfSignedCertificate.h"

using namespace std;
using namespace sptk;

namespace xmq {

namespace {

/// Port the interface under test is served on. Its own, so the port the fixture's configuration
/// names for the web service stays free for whatever else expects to find it there.
constexpr uint16_t httpsTestPort = 9881;

/// How long a test waits for a reply before deciding that none is coming.
constexpr auto replyTimeout = chrono::milliseconds(5000);

/**
 * @brief The value of a header, found without regard to how the sender capitalised its name.
 *
 * Matched on the newline alone rather than on a carriage return and a newline: this server ends
 * header lines both ways depending on what produced the response, and a reader that insists on
 * one of them reads half of what it is given.
 *
 * @param response          Response as it was read.
 * @param name              Header name, lowercase, without the colon.
 * @return Value with surrounding spaces removed, or empty when the header is absent.
 */
String headerValue(const String& response, const String& name)
{
    const auto lowercased = response.toLowerCase();
    const auto headerStart = lowercased.find("\n" + name + ":");
    if (headerStart == String::npos)
    {
        return {};
    }

    const auto valueStart = headerStart + name.length() + 2;
    const auto valueEnd = response.find_first_of("\r\n", valueStart);
    return String(response.substr(valueStart, valueEnd - valueStart)).trim();
}

/**
 * @brief Where the body starts, whichever way the sender ended its header lines.
 * @param response          Response as it was read so far.
 * @return Offset of the first body byte, or npos while the headers are incomplete.
 */
size_t bodyOffset(const String& response)
{
    const auto withReturns = response.find("\r\n\r\n");
    const auto withoutThem = response.find("\n\n");

    if (withReturns != String::npos && (withoutThem == String::npos || withReturns < withoutThem))
    {
        return withReturns + 4;
    }
    return withoutThem == String::npos ? String::npos : withoutThem + 2;
}

/**
 * @brief Read one HTTP response: the headers, then as much body as they promise.
 *
 * Reading to the end of the connection would work as well, but only after waiting out the
 * keep-alive on every response that is not closing it.
 *
 * @param socket            Socket the request was sent on.
 * @return The response, which may be incomplete if the reply timed out.
 */
String readResponse(TCPSocket& socket)
{
    String               response;
    array<uint8_t, 1024> buffer {};

    while (socket.readyToRead(replyTimeout))
    {
        size_t received = 0;
        try
        {
            received = socket.read(buffer.data(), buffer.size());
        }
        catch (const Exception&)
        {
            // The peer closed the connection: whatever arrived before that is the response.
            break;
        }

        if (received == 0)
        {
            break;
        }
        response.append(bit_cast<const char*>(buffer.data()), received);

        const auto bodyStart = bodyOffset(response);
        if (bodyStart == String::npos)
        {
            continue;
        }

        const auto promised = String(headerValue(response.substr(0, bodyStart), "content-length"));
        if (response.length() - bodyStart >= static_cast<size_t>(promised.toInt()))
        {
            break;
        }
    }

    return response;
}

/**
 * @brief Send a request over a plain, unencrypted connection.
 * @param request           Request to send, headers and all.
 * @return The response.
 */
String plainRequest(const String& request)
{
    TCPSocket socket;
    socket.open(Host("localhost", httpsTestPort));
    socket.write(request);
    return readResponse(socket);
}

/**
 * @brief Sends a plain request, waiting for the interface to have finished changing scheme.
 *
 * Turning encryption off answers before the listener has rebound, so a request sent immediately
 * after can still be met with the redirect the old scheme served. On an idle machine it never is,
 * which is why this only ever failed on the build farm - fc42, under load, got
 * "307 Temporary Redirect" where the test expected the page.
 *
 * Retries while the answer is a redirect and returns whatever it last saw, so a genuine failure
 * still reports the response rather than a timeout.
 */
String plainRequestOnceServed(const String& request,
                              const std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    auto response = plainRequest(request);
    while (response.starts_with("HTTP/1.1 307") && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        response = plainRequest(request);
    }
    return response;
}

/**
 * @brief Send a request over TLS, accepting whatever certificate the server offers.
 *
 * Not verifying is the point rather than a shortcut: what is being tested is a self-signed
 * certificate, which is exactly what verification is there to refuse.
 *
 * @param request           Request to send, headers and all.
 * @return The response.
 */
String tlsRequest(const String& request)
{
    const auto socket = SocketFactory::createSocket(make_shared<SSLKeys>());
    socket->open(Host("localhost", httpsTestPort));
    socket->write(request);
    return readResponse(*socket);
}

/**
 * @brief A directory for a certificate that goes away with the test.
 */
filesystem::path temporaryCertificateDirectory()
{
    return filesystem::temp_directory_path() / ("xmq_https_tests_" + to_string(::getpid()));
}

} // namespace

/**
 * @brief The configuration interface, served the way a real installation serves it.
 *
 * The listener is built by the same factory the server uses, on a certificate generated the same
 * way, so what the tests talk to is the interface rather than an arrangement made to be testable.
 */
class XMQ_EXPORT XMQ_HttpsInterfaceTests : public XMQ_ControlServiceTests
{
protected:
    void SetUp() override
    {
        XMQ_ControlServiceTests::SetUp();

        m_certificateDirectory = temporaryCertificateDirectory();
        error_code errorCode;
        filesystem::remove_all(m_certificateDirectory, errorCode);

        // Installing a certificate writes real files, and the directory it would write them into
        // on a developer's machine belongs to that machine's own installation. Put back at the
        // end rather than cleared: the run as a whole already points certificates elsewhere.
        m_previousCertificateDirectory = DirectoryNames::certsDirectory();
        DirectoryNames::setCertsDirectory(m_certificateDirectory);

        String description;
        ASSERT_TRUE(SelfSignedCertificate::create(certificateFile(), privateKeyFile(), "test-node", description));

        // The settings are shared by the whole suite, so what is borrowed here is put back. That
        // includes the two these tests change through ServiceControl rather than by hand: several
        // of them turn encryption off or move the port, and the suite used to leave them that way.
        // saysWhichSchemeTheInterfaceMovedTo then asked for a change that had already happened, got
        // no message about a scheme that had not moved, and failed - not for anything it does, but
        // for the order it ran in.
        auto& webService = server()->getSettings()->m_web_service;
        m_previousCertificate = webService.m_certfile.asString();
        m_previousPrivateKey = webService.m_keyfile.asString();
        m_previousEncrypted = webService.m_encrypted.asBool();
        m_previousListenerPort = webService.m_listener_port.asInteger();
        webService.m_certfile = certificateFile().string();
        webService.m_keyfile = privateKeyFile().string();

        const auto sslKeys = make_shared<SSLKeys>(privateKeyFile(), certificateFile());
        m_listener = ControlServiceListener::factory(m_controller.get(), sslKeys,
                                                     ServerTests_Suite::logEngine(), "0.0.0.0", httpsTestPort);

        // So that installing a certificate, or changing the scheme, reaches the listener the way
        // it reaches the real one.
        m_controller->setControlServiceListener(m_listener);
        m_controller->setWebServiceKeys(sslKeys);
    }

    void TearDown() override
    {
        // Before the certificate goes: the listener holds the keys it was built with.
        m_controller->setControlServiceListener({});
        m_listener.reset();

        auto& webService = server()->getSettings()->m_web_service;
        webService.m_certfile = m_previousCertificate;
        webService.m_keyfile = m_previousPrivateKey;
        webService.m_encrypted = m_previousEncrypted;
        webService.m_listener_port = m_previousListenerPort;

        DirectoryNames::setCertsDirectory(m_previousCertificateDirectory);

        error_code errorCode;
        filesystem::remove_all(m_certificateDirectory, errorCode);

        XMQ_ControlServiceTests::TearDown();
    }

    [[nodiscard]] filesystem::path certificateFile() const { return m_certificateDirectory / "webface.crt"; }
    [[nodiscard]] filesystem::path privateKeyFile() const { return m_certificateDirectory / "webface.key"; }

    /**
     * @brief Call SSLKeysControl the way the interface does, signed in as the administrator.
     * @param input             Request to make.
     * @return The reply.
     */
    [[nodiscard]] CSSLKeysControlResponse sslKeysControl(const CSSLKeysControl& input) const
    {
        HttpAuthentication      authentication("bearer " + Login("admin", "admin"));
        CSSLKeysControlResponse output;
        m_controlService->SSLKeysControl(input, output, &authentication);
        return output;
    }

    filesystem::path        m_certificateDirectory;
    filesystem::path        m_previousCertificateDirectory;
    SControlServiceListener m_listener;
    String                  m_previousCertificate;
    String                  m_previousPrivateKey;
    bool                    m_previousEncrypted {true};
    int64_t                 m_previousListenerPort {0};
};

TEST_F(XMQ_HttpsInterfaceTests, servesTheApiOverTls)
{
    const String body = R"({"action":"status"})";
    const auto   response = tlsRequest("POST /ServerControl HTTP/1.1\r\n"
                                       "Host: localhost:" +
                                     to_string(httpsTestPort) +
                                     "\r\n"
                                       "Content-Type: application/json\r\n"
                                       "Content-Length: " +
                                     to_string(body.length()) + "\r\n\r\n" + body);

    EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response.c_str();

    // The service answered, rather than the connection merely being encrypted: an unauthenticated
    // call is refused by name, which is a reply only the service produces.
    EXPECT_NE(String::npos, response.find("Not authenticated")) << response.c_str();
}

TEST_F(XMQ_HttpsInterfaceTests, redirectsPlainHttpToHttps)
{
    const auto response = plainRequest("GET /dashboard?tab=2 HTTP/1.1\r\n"
                                       "Host: localhost:" +
                                       to_string(httpsTestPort) + "\r\n\r\n");

    // Temporary, so that turning encryption back off is not defeated by a redirect the browser
    // cached; and 307 rather than 302 so a request with a body is repeated as it was.
    EXPECT_TRUE(response.starts_with("HTTP/1.1 307")) << response.c_str();

    // Path and query are carried over: a bookmark deeper than the root still arrives where it
    // was aimed.
    EXPECT_EQ("https://localhost:" + to_string(httpsTestPort) + "/dashboard?tab=2",
              headerValue(response, "location").c_str());
}

TEST_F(XMQ_HttpsInterfaceTests, redirectKeepsTheHostTheRequestNamed)
{
    // The name the browser used, not the name of this machine: it is the one that resolved, the
    // one that may be a tunnel or an alias, and the one the certificate is checked against.
    const auto response = plainRequest("GET / HTTP/1.1\r\n"
                                       "Host: xmq.example:" +
                                       to_string(httpsTestPort) + "\r\n\r\n");

    EXPECT_EQ("https://xmq.example:" + to_string(httpsTestPort) + "/",
              headerValue(response, "location").c_str());
}

TEST_F(XMQ_HttpsInterfaceTests, redirectNamesThePortWhenTheRequestDidNot)
{
    // A Host without a port means the default port for the scheme it arrived on. Left alone, the
    // redirect would send the browser to 443, where nothing is listening.
    const auto response = plainRequest("GET / HTTP/1.1\r\n"
                                       "Host: xmq.example\r\n\r\n");

    EXPECT_EQ("https://xmq.example:" + to_string(httpsTestPort) + "/",
              headerValue(response, "location").c_str());
}

TEST_F(XMQ_HttpsInterfaceTests, refusesToRedirectWithoutAHost)
{
    const auto response = plainRequest("GET / HTTP/1.0\r\n\r\n");

    // Nothing to build an address from, so it says what the port is instead of guessing.
    EXPECT_TRUE(response.starts_with("HTTP/1.1 400")) << response.c_str();
    EXPECT_TRUE(headerValue(response, "location").empty()) << response.c_str();
}

TEST_F(XMQ_HttpsInterfaceTests, refusesAHostThatIsNotOne)
{
    // The Host header is whatever the client chose to send, and it ends up in a header and on a
    // page. One that cannot be a host name is refused rather than escaped.
    const auto response = plainRequest("GET / HTTP/1.1\r\n"
                                       "Host: x\"><script>alert(1)</script>\r\n\r\n");

    EXPECT_TRUE(response.starts_with("HTTP/1.1 400")) << response.c_str();
    EXPECT_EQ(String::npos, response.find("<script>")) << response.c_str();
}

TEST_F(XMQ_HttpsInterfaceTests, keepsServingTlsAfterAPlainRequest)
{
    const auto redirected = plainRequest("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    ASSERT_TRUE(redirected.starts_with("HTTP/1.1 307")) << redirected.c_str();

    // The plain request is answered on the accepting thread and its socket closed there. This is
    // what says that doing so leaves the listener able to accept the next connection normally.
    const String body = R"({"action":"status"})";
    const auto   response = tlsRequest("POST /ServerControl HTTP/1.1\r\n"
                                       "Host: localhost:" +
                                     to_string(httpsTestPort) +
                                     "\r\n"
                                       "Content-Type: application/json\r\n"
                                       "Content-Length: " +
                                     to_string(body.length()) + "\r\n\r\n" + body);

    EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response.c_str();
}

TEST_F(XMQ_HttpsInterfaceTests, endsHeaderLinesTheWayHttpSaysTo)
{
    // Static files and API replies come from different code, and only one of them used to end
    // header lines with a carriage return. Browsers and curl accept a bare newline, so the
    // difference stayed invisible until something read the response by the book. The reader in
    // this file now accepts either, which is exactly why the shape is asserted here instead.
    const String body = R"({"action":"status"})";
    const auto   apiReply = tlsRequest("POST /ServerControl HTTP/1.1\r\nHost: localhost:" +
                                     to_string(httpsTestPort) +
                                     "\r\nContent-Type: application/json\r\nContent-Length: " +
                                     to_string(body.length()) + "\r\n\r\n" + body);

    // A static file, reached through the fallback that serves the interface's own pages.
    const auto page = tlsRequest("GET /dashboard HTTP/1.1\r\nHost: localhost:" +
                                 to_string(httpsTestPort) + "\r\n\r\n");

    for (const auto& response: {apiReply, page})
    {
        ASSERT_TRUE(response.starts_with("HTTP/1.1 ")) << response.c_str();
        EXPECT_NE(String::npos, response.find("\r\n\r\n"))
            << "headers are not terminated the way HTTP says: " << response.substr(0, 200).c_str();

        // Every line of the headers, not only the last: a single one written with a bare newline
        // is enough to make the rest of them unreadable.
        const auto headers = response.substr(0, response.find("\r\n\r\n"));
        for (size_t position = headers.find('\n'); position != String::npos;
             position = headers.find('\n', position + 1))
        {
            EXPECT_GT(position, 0U);
            EXPECT_EQ('\r', headers[position - 1])
                << "bare newline in the headers: " << headers.c_str();
        }
    }
}

TEST_F(XMQ_HttpsInterfaceTests, reportsTheCertificateItIsServing)
{
    CSSLKeysControl input;
    input.m_action = "get";
    const auto response = sslKeysControl(input);

    ASSERT_TRUE(response.m_result.m_success.asBool()) << response.m_result.m_description.asString().c_str();
    EXPECT_EQ(certificateFile().string(), response.m_web_service_keys.m_certfile.asString());
    EXPECT_EQ(privateKeyFile().string(), response.m_web_service_keys.m_keyfile.asString());

    // The fingerprint is the point of reporting this at all: it is what an administrator checks
    // the browser's warning against.
    const auto description = response.m_web_service_keys.m_description.asString();
    EXPECT_NE(string::npos, description.find("SHA-256 fingerprint")) << description.c_str();
    EXPECT_NE(string::npos, description.find("expires")) << description.c_str();
}

TEST_F(XMQ_HttpsInterfaceTests, installsAnUploadedCertificate)
{
    // A pair made the way an administrator's certificate authority would make one - somewhere else
    // entirely, arriving as file content rather than as a path.
    const auto elsewhere = m_certificateDirectory / "elsewhere";
    String     description;
    ASSERT_TRUE(SelfSignedCertificate::create(elsewhere / "issued.crt", elsewhere / "issued.key", "other-node", description));

    Buffer certificateContent;
    Buffer privateKeyContent;
    certificateContent.loadFromFile(elsewhere / "issued.crt");
    privateKeyContent.loadFromFile(elsewhere / "issued.key");

    CSSLKeysControl input;
    input.m_action = "set";
    input.m_web_service_keys.m_certfile = certificateContent.c_str();
    input.m_web_service_keys.m_keyfile = privateKeyContent.c_str();

    const auto response = sslKeysControl(input);
    ASSERT_TRUE(response.m_result.m_success.asBool()) << response.m_result.m_description.asString().c_str();

    // Installed under the interface's own names, whatever the uploaded files were called: certfile
    // and keyfile may have been aimed at the broker's pair, which installing here must not touch.
    EXPECT_EQ(certificateFile().string(), response.m_web_service_keys.m_certfile.asString());
    EXPECT_EQ(SelfSignedCertificate::describe(elsewhere / "issued.crt"),
              response.m_web_service_keys.m_description.asString());

    // Nothing to explain away: the certificate reported above is the one being served. A note
    // here would mean the listener refused the keys and the description says why.
    EXPECT_TRUE(response.m_result.m_description.asString().empty())
        << response.m_result.m_description.asString().c_str();

    // The listener took the keys and is still answering: installing a certificate does not
    // interrupt the session that installed it.
    const String body = R"({"action":"status"})";
    const auto   served = tlsRequest("POST /ServerControl HTTP/1.1\r\nHost: localhost:" +
                                   to_string(httpsTestPort) +
                                   "\r\nContent-Type: application/json\r\nContent-Length: " +
                                   to_string(body.length()) + "\r\n\r\n" + body);
    EXPECT_TRUE(served.starts_with("HTTP/1.1 200")) << served.c_str();
}

TEST_F(XMQ_HttpsInterfaceTests, keepsThePreviousFileWhenACertificateIsReplaced)
{
    const auto issued = SelfSignedCertificate::describe(certificateFile());

    const auto elsewhere = m_certificateDirectory / "elsewhere";
    String     description;
    ASSERT_TRUE(SelfSignedCertificate::create(elsewhere / "issued.crt", elsewhere / "issued.key", "other-node", description));

    Buffer certificateContent;
    Buffer privateKeyContent;
    certificateContent.loadFromFile(elsewhere / "issued.crt");
    privateKeyContent.loadFromFile(elsewhere / "issued.key");

    CSSLKeysControl input;
    input.m_action = "set";
    input.m_web_service_keys.m_certfile = certificateContent.c_str();
    input.m_web_service_keys.m_keyfile = privateKeyContent.c_str();
    ASSERT_TRUE(sslKeysControl(input).m_result.m_success.asBool());

    // The file replaced may be the only copy of a certificate somebody paid for, and the mistake
    // is usually noticed afterwards.
    auto previous = certificateFile();
    previous += ".old";
    ASSERT_TRUE(filesystem::exists(previous));
    EXPECT_EQ(issued, SelfSignedCertificate::describe(previous));
}

TEST_F(XMQ_HttpsInterfaceTests, refusesACertificateWithoutItsKey)
{
    Buffer certificateContent;
    certificateContent.loadFromFile(certificateFile());

    CSSLKeysControl input;
    input.m_action = "set";
    input.m_web_service_keys.m_certfile = certificateContent.c_str();

    // A certificate installed over a key that does not match it leaves the interface unable to
    // complete a handshake, and this interface is what would have to fix that.
    const auto response = sslKeysControl(input);
    EXPECT_FALSE(response.m_result.m_success.asBool());
    EXPECT_NE(string::npos, response.m_result.m_description.asString().find("private key"))
        << response.m_result.m_description.asString().c_str();
}

TEST_F(XMQ_HttpsInterfaceTests, issuesAFreshSelfSignedCertificateOnRequest)
{
    const auto replaced = SelfSignedCertificate::describe(certificateFile());

    CSSLKeysControl input;
    input.m_action = "reissue";
    const auto response = sslKeysControl(input);

    ASSERT_TRUE(response.m_result.m_success.asBool()) << response.m_result.m_description.asString().c_str();

    // Unlike the pair made at startup, which is left alone when one is already there: reissuing is
    // asked for exactly when the certificate that is installed is the problem.
    EXPECT_NE(replaced, response.m_web_service_keys.m_description.asString());
    EXPECT_EQ(SelfSignedCertificate::describe(certificateFile()),
              response.m_web_service_keys.m_description.asString());

    const String body = R"({"action":"status"})";
    const auto   served = tlsRequest("POST /ServerControl HTTP/1.1\r\nHost: localhost:" +
                                   to_string(httpsTestPort) +
                                   "\r\nContent-Type: application/json\r\nContent-Length: " +
                                   to_string(body.length()) + "\r\n\r\n" + body);
    EXPECT_TRUE(served.starts_with("HTTP/1.1 200")) << served.c_str();
}

TEST_F(XMQ_HttpsInterfaceTests, leavesTheBrokerKeysAloneWhenOnlyTheInterfaceIsSaved)
{
    CSSLKeysControl before;
    before.m_action = "get";
    const auto brokerKeys = sslKeysControl(before).m_keys;

    CSSLKeysControl input;
    input.m_action = "reissue";
    ASSERT_TRUE(sslKeysControl(input).m_result.m_success.asBool());

    // The two certificates share a screen and an action, and that is the only thing they share.
    const auto after = sslKeysControl(before).m_keys;
    EXPECT_EQ(brokerKeys.m_certfile.asString(), after.m_certfile.asString());
    EXPECT_EQ(brokerKeys.m_keyfile.asString(), after.m_keyfile.asString());
    EXPECT_EQ(brokerKeys.m_verify_depth.asString(), after.m_verify_depth.asString());
}

TEST_F(XMQ_HttpsInterfaceTests, servesPlainHttpWhenEncryptionIsTurnedOff)
{
    CServiceControl input;
    input.m_action = "set";
    input.m_web_service.m_listener_port = httpsTestPort;
    input.m_web_service.m_encrypted = false;

    HttpAuthentication      authentication("bearer " + Login("admin", "admin"));
    CServiceControlResponse output;
    m_controlService->ServiceControl(input, output, &authentication);

    ASSERT_TRUE(output.m_result.m_success.asBool()) << output.m_result.m_description.asString().c_str();
    EXPECT_FALSE(output.m_service_restart_required.asBool()) << output.m_message.asString().c_str();

    // A request that used to be answered with a redirect is now the ordinary way in. The API rather
    // than a page: a page is only there when the web interface has been built, and a 404 for a
    // missing file says nothing about whether plain HTTP is served.
    const String body = R"({"action":"status"})";
    const auto   response = plainRequestOnceServed("POST /ServerControl HTTP/1.1\r\nHost: localhost:" +
                                                   to_string(httpsTestPort) +
                                                   "\r\nContent-Type: application/json\r\nContent-Length: " +
                                                   to_string(body.length()) + "\r\n\r\n" + body);
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response.c_str();
    EXPECT_TRUE(headerValue(response, "location").empty()) << response.c_str();
}

TEST_F(XMQ_HttpsInterfaceTests, servesHttpsAgainWhenEncryptionIsTurnedBackOn)
{
    HttpAuthentication authentication("bearer " + Login("admin", "admin"));

    CServiceControl off;
    off.m_action = "set";
    off.m_web_service.m_listener_port = httpsTestPort;
    off.m_web_service.m_encrypted = false;
    CServiceControlResponse offResponse;
    m_controlService->ServiceControl(off, offResponse, &authentication);
    ASSERT_TRUE(offResponse.m_result.m_success.asBool());

    CServiceControl on;
    on.m_action = "set";
    on.m_web_service.m_listener_port = httpsTestPort;
    on.m_web_service.m_encrypted = true;
    CServiceControlResponse onResponse;
    m_controlService->ServiceControl(on, onResponse, &authentication);

    ASSERT_TRUE(onResponse.m_result.m_success.asBool()) << onResponse.m_result.m_description.asString().c_str();
    EXPECT_FALSE(onResponse.m_service_restart_required.asBool()) << onResponse.m_message.asString().c_str();

    // Back to TLS, on the same port, with nothing restarted in between.
    const String body = R"({"action":"status"})";
    const auto   served = tlsRequest("POST /ServerControl HTTP/1.1\r\nHost: localhost:" +
                                   to_string(httpsTestPort) +
                                   "\r\nContent-Type: application/json\r\nContent-Length: " +
                                   to_string(body.length()) + "\r\n\r\n" + body);
    EXPECT_TRUE(served.starts_with("HTTP/1.1 200")) << served.c_str();

    // And the redirect is back with it: plain requests are a mistake again.
    const auto plain = plainRequest("GET / HTTP/1.1\r\nHost: localhost:" +
                                    to_string(httpsTestPort) + "\r\n\r\n");
    EXPECT_TRUE(plain.starts_with("HTTP/1.1 307")) << plain.c_str();
}

TEST_F(XMQ_HttpsInterfaceTests, saysWhichSchemeTheInterfaceMovedTo)
{
    CServiceControl input;
    input.m_action = "set";
    input.m_web_service.m_listener_port = httpsTestPort;
    input.m_web_service.m_encrypted = false;

    HttpAuthentication      authentication("bearer " + Login("admin", "admin"));
    CServiceControlResponse output;
    m_controlService->ServiceControl(input, output, &authentication);

    // Every later request has to go to the other scheme, and a page that is not told so is a page
    // that has quietly stopped working.
    EXPECT_NE(string::npos, output.m_message.asString().find("http"))
        << output.m_message.asString().c_str();
}

/**
 * @brief The certificate an installation issues to itself when it has none.
 */
class XMQ_EXPORT XMQ_SelfSignedCertificateTests : public testing::Test
{
protected:
    void SetUp() override
    {
        m_directory = temporaryCertificateDirectory() / "selfsigned";
        error_code errorCode;
        filesystem::remove_all(m_directory, errorCode);
    }

    void TearDown() override
    {
        error_code errorCode;
        filesystem::remove_all(m_directory, errorCode);
    }

    [[nodiscard]] filesystem::path certificateFile() const { return m_directory / "webface.crt"; }
    [[nodiscard]] filesystem::path privateKeyFile() const { return m_directory / "webface.key"; }

    filesystem::path m_directory;
};

TEST_F(XMQ_SelfSignedCertificateTests, createsAPairWhereThereWasNone)
{
    String description;
    EXPECT_TRUE(SelfSignedCertificate::create(certificateFile(), privateKeyFile(), "test-node", description));

    // Including the directory: an installation that has never been configured has no certs
    // directory either, and having to create one by hand first would defeat the point.
    EXPECT_TRUE(filesystem::exists(certificateFile()));
    EXPECT_TRUE(filesystem::exists(privateKeyFile()));
    EXPECT_FALSE(description.empty());
}

TEST_F(XMQ_SelfSignedCertificateTests, describesWhatItWrote)
{
    String description;
    ASSERT_TRUE(SelfSignedCertificate::create(certificateFile(), privateKeyFile(), "test-node", description));

    const auto reported = SelfSignedCertificate::describe(certificateFile());

    // The fingerprint is the whole reason this is printed: a self-signed certificate is worth
    // accepting in a browser only if it can be checked against something that is not the browser.
    EXPECT_NE(String::npos, reported.find("SHA-256 fingerprint")) << reported.c_str();
    EXPECT_NE(String::npos, reported.find("XMQ node")) << reported.c_str();
}

TEST_F(XMQ_SelfSignedCertificateTests, saysNothingAboutACertificateItCannotRead)
{
    EXPECT_TRUE(SelfSignedCertificate::describe(m_directory / "absent.crt").empty());
}

TEST_F(XMQ_SelfSignedCertificateTests, leavesAnExistingPairAlone)
{
    String description;
    ASSERT_TRUE(SelfSignedCertificate::create(certificateFile(), privateKeyFile(), "test-node", description));
    const auto issued = SelfSignedCertificate::describe(certificateFile());

    // Called on every start, so this is the ordinary case rather than the exception: a certificate
    // reissued behind the administrator's back would change the fingerprint they published, and
    // browsers would start warning again about a certificate that was already accepted.
    EXPECT_FALSE(SelfSignedCertificate::create(certificateFile(), privateKeyFile(), "test-node", description));
    EXPECT_EQ(static_cast<const string&>(issued),
              static_cast<const string&>(SelfSignedCertificate::describe(certificateFile())));
}

TEST_F(XMQ_SelfSignedCertificateTests, refusesToCompleteHalfAPair)
{
    String description;
    ASSERT_TRUE(SelfSignedCertificate::create(certificateFile(), privateKeyFile(), "test-node", description));
    filesystem::remove(privateKeyFile());

    // Half a pair is somebody's certificate with a problem. Generating the missing half would
    // destroy the only copy of the half that is there, so it is reported instead.
    EXPECT_THROW(SelfSignedCertificate::create(certificateFile(), privateKeyFile(), "test-node", description), Exception);
    EXPECT_TRUE(filesystem::exists(certificateFile()));
}

} // namespace xmq
