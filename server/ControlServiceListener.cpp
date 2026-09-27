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
#include "ControlServiceListener.h"
#include "ControlService.h"
#include "Server.h"

#include "common/DirectoryNames.h"

#ifndef _WIN32
#include <poll.h>
#endif

using namespace std;
using namespace sptk;
using namespace xmq;

// One constant used to serve two unrelated jobs, and four was too many for both.
//
// The web service pool is "max number of simultaneously running requests", and the requests are an
// administrator's: a handful of short reads and the occasional settings write, with the dashboard
// polling on a timer. Nothing here streams - no SSE, no WebSocket, no long poll - so a single
// worker cannot be held open by one client, and the only cost is that a page load fetches its
// assets in turn rather than at once.
constexpr auto webServiceThreads = 1;

/// Accept threads for the control port. One is enough: the connections arriving here are an
/// administrator's browser, not a client population, and accepting them is not work worth spreading.
constexpr auto controlListenerThreads = 1;

namespace {

/// First byte of a TLS record carrying a handshake. No HTTP method begins with it.
constexpr uint8_t tlsHandshakeByte = 0x16;

/// How long a connection is given to say its first byte before it is treated as TLS.
constexpr int firstByteTimeoutMs = 2000;

/// As much of a plain request as is worth reading: the request line and the Host header.
constexpr size_t maxPlainRequest = 2048;

/// Send without raising SIGPIPE where the platform offers the choice.
#ifdef MSG_NOSIGNAL
constexpr int sendFlags = MSG_NOSIGNAL;
#else
constexpr int sendFlags = 0;
#endif

/**
 * @brief Close an accepted socket that no connection object has taken over.
 */
void closeHandle(const SocketType handle)
{
#ifdef _WIN32
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

/**
 * @brief Wait until the socket has something to read, or the deadline passes.
 * @param handle            Socket to wait on.
 * @param deadline          When to give up.
 * @return true if there is input to read.
 */
bool waitForInput(const SocketType handle, const chrono::steady_clock::time_point deadline)
{
    while (true)
    {
        const auto remaining = chrono::duration_cast<chrono::milliseconds>(deadline - chrono::steady_clock::now());
        if (remaining.count() <= 0)
        {
            return false;
        }

        pollfd descriptor {handle, POLLIN, 0};
#ifdef _WIN32
        const auto ready = WSAPoll(&descriptor, 1, static_cast<int>(remaining.count()));
#else
        const auto ready = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
        if (ready < 0 && errno == EINTR)
        {
            // A signal, not an answer about the socket: wait out what is left of the time.
            continue;
        }
#endif
        return ready > 0;
    }
}

/**
 * @brief What the client has already sent, without waiting for it.
 *
 * Asked rather than waited for: both callers know whether there is anything to read - one has
 * just polled the socket, the other does not care. Waiting was done with a deadline a
 * millisecond away, and duration_cast to milliseconds truncates anything shorter than one to
 * zero, so the wait gave up before it looked and every connection read as silent.
 *
 * The byte is left in the socket for whoever reads it next.
 *
 * @param handle            Socket to look at.
 * @return The first byte, or 0 when nothing is there.
 */
uint8_t peekAvailableByte(const SocketType handle)
{
    pollfd descriptor {handle, POLLIN, 0};
#ifdef _WIN32
    const auto ready = WSAPoll(&descriptor, 1, 0);
#else
    const auto ready = ::poll(&descriptor, 1, 0);
#endif
    if (ready <= 0 || (descriptor.revents & POLLIN) == 0)
    {
        return 0;
    }

    char       first = 0;
    const auto peeked = ::recv(handle, &first, 1, MSG_PEEK);
    return peeked == 1 ? static_cast<uint8_t>(first) : 0;
}

/**
 * @brief Read a plain request, as far as the end of its headers.
 * @param handle            Socket to read from.
 * @param deadline          When to stop waiting for more.
 * @return What was read, which may be an incomplete request.
 */
String readPlainRequest(const SocketType handle, const chrono::steady_clock::time_point deadline)
{
    String request;
    while (request.length() < maxPlainRequest)
    {
        if (!waitForInput(handle, deadline))
        {
            break;
        }

        array<char, 512> buffer {};
        const auto       received = ::recv(handle, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0)
        {
            break;
        }

        request.append(buffer.data(), static_cast<size_t>(received));
        if (request.find("\r\n\r\n") != String::npos)
        {
            break;
        }
    }
    return request;
}

/**
 * @brief The path and query the request asked for.
 * @param request           Request as it was read.
 * @return Target to put after the host, always starting with a slash.
 */
String requestTarget(const String& request)
{
    const auto methodEnd = request.find(' ');
    if (methodEnd == String::npos)
    {
        return "/";
    }

    const auto targetStart = methodEnd + 1;
    const auto targetEnd = request.find_first_of(" \r\n", targetStart);
    if (targetEnd == String::npos)
    {
        return "/";
    }

    const String target = request.substr(targetStart, targetEnd - targetStart);

    // Only an origin-form target is carried over. The absolute form a proxy would send, and
    // anything malformed, are answered at the root rather than reflected back.
    return target.starts_with('/') ? target : String("/");
}

/**
 * @brief Whether a host name is one worth putting in a redirect.
 *
 * The name comes from the request, so it is whatever the client chose to send. Names outside
 * what a host name can hold are refused rather than escaped: there is no legitimate request
 * they come from, and the redirect is built into both a header and a page.
 *
 * @param host              Host as the request gave it.
 * @return true if it may be used.
 */
bool isUsableHost(const String& host)
{
    constexpr size_t maxHostLength = 255;
    if (host.empty() || host.length() > maxHostLength)
    {
        return false;
    }

    return ranges::all_of(host,
                          [](const char character)
                          {
                              return isalnum(static_cast<unsigned char>(character)) != 0 ||
                                     character == '.' || character == '-' || character == ':' ||
                                     character == '[' || character == ']';
                          });
}

/**
 * @brief The host the request was addressed to, port included.
 *
 * Taken from the request rather than from this machine: the browser reached the server under
 * some name - a name, an address, a tunnel - and that is the route that will still work. A name
 * chosen here instead may not resolve, and will not match the certificate.
 *
 * @param request           Request as it was read.
 * @param port              Port the interface is served on, for a Host that omits it.
 * @return Host to redirect to, or empty when the request named none worth using.
 */
String requestHost(const String& request, const uint16_t port)
{
    // Header names are case-insensitive, so the search is made over a lowercased copy while the
    // value is taken from the original.
    const auto lowercased = request.toLowerCase();

    constexpr auto headerName = "\r\nhost:";
    const auto     headerStart = lowercased.find(headerName);
    if (headerStart == String::npos)
    {
        return "";
    }

    const auto valueStart = headerStart + strlen(headerName);
    const auto valueEnd = request.find_first_of("\r\n", valueStart);
    const auto host = String(request.substr(valueStart, valueEnd - valueStart)).trim();
    if (!isUsableHost(host))
    {
        return "";
    }

    // A Host with no port means the default port for the scheme it arrived on, which is not the
    // port this interface is served on: naming it is what keeps the redirect off 443.
    const auto hasPort = host.starts_with('[') ? host.find("]:") != String::npos
                                               : host.find(':') != String::npos;

    return hasPort ? host : String(host + ":" + to_string(port));
}

/**
 * @brief The response that sends a browser to the same address under HTTPS.
 * @param location          Address to send it to.
 * @return Complete response, headers and body.
 */
String redirectResponse(const String& location)
{
    const String body = "<html><head><title>XMQ</title></head><body>"
                        "This port serves the XMQ configuration interface over HTTPS. "
                        "Continuing at <a href=\"" + location + "\">" + location + "</a>."
                        "</body></html>";

    // Temporary, and not Moved Permanently: the interface can be configured back to plain HTTP,
    // and a permanent redirect cached by the browser would then have to be cleared by hand.
    // 307 rather than 302 so that a request with a body is repeated as it was, not turned into
    // a GET - the API is reached on this port too, not only the pages.
    return "HTTP/1.1 307 Temporary Redirect\r\n"
           "Location: " + location + "\r\n"
           "Content-Type: text/html; charset=utf-8\r\n"
           "Content-Length: " + to_string(body.length()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" + body;
}

/**
 * @brief The response for a plain request that named no host to redirect to.
 * @return Complete response, headers and body.
 */
String noHostResponse()
{
    const String body = "<html><head><title>XMQ</title></head><body>"
                        "This port serves the XMQ configuration interface over HTTPS. "
                        "Ask for it with an https:// address."
                        "</body></html>";

    return "HTTP/1.1 400 Bad Request\r\n"
           "Content-Type: text/html; charset=utf-8\r\n"
           "Content-Length: " + to_string(body.length()) + "\r\n"
           "Connection: close\r\n"
           "\r\n" + body;
}

} // namespace

ControlServiceListener::ControlServiceListener(const WSServices& services, const WSConnection::Options& options,
                                               const shared_ptr<SSLKeys>& sslKeys, const shared_ptr<LogEngine>& logEngine,
                                               const String& bindAddress, const uint16_t servicePort)
    : WSServer(services, *logEngine, "localhost", webServiceThreads, options)
    , m_bindAddress(bindAddress)
    , m_servicePort(servicePort)
    , m_sslKeys(sslKeys)
    , m_logEngine(logEngine)
{
    // Uncomment and trace in to get into SPTK code
    //const auto& opts = getOptions();

    if (m_sslKeys)
    {
        setSSLKeys(m_sslKeys);
    }
}

bool ControlServiceListener::bindTo(const String& bindAddress, const uint16_t port)
{
    if (bindAddress == m_bindAddress && port == m_servicePort)
    {
        return false;
    }

    const auto connectionType = m_sslKeys ? ServerConnection::Type::SSL : ServerConnection::Type::TCP;
    const Host previous {m_bindAddress, m_servicePort};
    const Host wanted {bindAddress, port};

    if (port == m_servicePort)
    {
        // The same port under another address. The old socket holds the port, so it has to go
        // before the new one can be opened, and a failure leaves the interface with nothing
        // listening unless the old address is put back.
        removeListener(previous);
        try
        {
            addListener(connectionType, wanted, controlListenerThreads);
        }
        catch (const Exception&)
        {
            try
            {
                addListener(connectionType, previous, controlListenerThreads);
            }
            catch (const Exception&)
            {
                // Swallowed deliberately: the caller is told about the failure that started this.
            }
            throw;
        }
    }
    else
    {
        // A different port, so the new socket is opened before the old one is given up and a
        // port that cannot be taken costs nothing.
        addListener(connectionType, wanted, controlListenerThreads);
        removeListener(previous);
    }

    m_bindAddress = bindAddress;
    m_servicePort = port;
    return true;
}

void ControlServiceListener::useKeys(const shared_ptr<SSLKeys>& sslKeys)
{
    if (!sslKeys)
    {
        throw Exception("The configuration interface cannot be served without keys.");
    }

    if (!m_sslKeys)
    {
        throw Exception("The configuration interface is serving plain HTTP. Turn encryption on and "
                        "restart the service for a certificate to be used.");
    }

    // Throws if the certificate is not where it says it is, before anything here is replaced: a
    // listener left holding keys it cannot serve would fail every connection from now on.
    setSSLKeys(sslKeys);
    m_sslKeys = sslKeys;
}

void ControlServiceListener::serveEncrypted(const shared_ptr<SSLKeys>& sslKeys)
{
    const auto wasEncrypted = m_sslKeys != nullptr;
    const auto willBeEncrypted = sslKeys != nullptr;

    if (wasEncrypted == willBeEncrypted)
    {
        // Already the right scheme. New keys for the scheme it is already serving are still worth
        // taking, and cost nothing to hand over.
        if (willBeEncrypted)
        {
            useKeys(sslKeys);
        }
        return;
    }

    const Host listenerHost {m_bindAddress, m_servicePort};
    const auto previousKeys = m_sslKeys;

    // Keys first, and only when there are any: addListener refuses to open an SSL listener
    // without them, and that refusal is better had before the old socket is gone.
    if (willBeEncrypted)
    {
        setSSLKeys(sslKeys);
    }
    m_sslKeys = sslKeys;

    removeListener(listenerHost);

    try
    {
        addListener(willBeEncrypted ? ServerConnection::Type::SSL : ServerConnection::Type::TCP,
                    listenerHost, controlListenerThreads);
    }
    catch (const Exception&)
    {
        // The port was given up and could not be taken back under the new scheme. Whatever the
        // reason, an interface nobody can reach is the one outcome worth avoiding, so the old
        // scheme goes back on. If even that fails there is nothing left to try, and the original
        // reason is the one worth reporting.
        m_sslKeys = previousKeys;
        if (previousKeys)
        {
            setSSLKeys(previousKeys);
        }

        try
        {
            addListener(wasEncrypted ? ServerConnection::Type::SSL : ServerConnection::Type::TCP,
                        listenerHost, controlListenerThreads);
        }
        catch (const Exception&)
        {
            // Swallowed deliberately: the caller is told about the failure that started this.
        }
        throw;
    }
}

shared_ptr<ServerConnection> ControlServiceListener::createConnection(const ServerConnection::Type connectionType,
                                                                      const SocketType             connectionSocket,
                                                                      const sockaddr_in*           peer)
{
    if (connectionType != ServerConnection::Type::SSL)
    {
        // Nothing to be mistaken about: the interface is already plain HTTP.
        return WSServer::createConnection(connectionType, connectionSocket, peer);
    }

    // Whatever is there already, with no waiting at all: a client that has spoken is classified
    // here, on the listener thread, where it is cheapest. Most connections do not qualify - a
    // server accepts as soon as the connection is made, which is before the client has had the
    // chance to send anything - and those are dealt with below.
    if (const auto firstByte = peekAvailableByte(connectionSocket);
        firstByte != 0)
    {
        return classify(connectionSocket, *peer, firstByte);
    }

    // Nothing to classify by, which now means the client closed before saying anything:
    // FastTCPServer does not build an encrypted connection until the client has spoken, so this
    // is reached only for a socket that has since gone. Handed on regardless - the usual
    // machinery closes it - rather than second-guessed here.
    return WSServer::createConnection(connectionType, connectionSocket, peer);
}

shared_ptr<ServerConnection> ControlServiceListener::classify(const SocketType  socket,
                                                              const sockaddr_in& peer,
                                                              const uint8_t      firstByte)
{
    if (firstByte != tlsHandshakeByte)
    {
        // Plain HTTP on the TLS port. Answered here rather than handed to the reactor: there is
        // one reply to send, and then the connection is finished with.
        redirectToHttps(socket);
        return {};
    }

    // A TLS record, and the handshake will not have to wait for it: the first byte is already
    // there to be read.
    return WSServer::createConnection(ServerConnection::Type::SSL, socket, &peer);
}

void ControlServiceListener::redirectToHttps(const SocketType connectionSocket) const
{
    const auto deadline = chrono::steady_clock::now() + chrono::milliseconds(firstByteTimeoutMs);
    const auto request = readPlainRequest(connectionSocket, deadline);
    const auto host = requestHost(request, m_servicePort);

    const auto response = host.empty() ? noHostResponse()
                                       : redirectResponse("https://" + host + requestTarget(request));

    // Best effort: the client is being told where to go, and a write that fails leaves nothing
    // worth reporting - the socket is closed either way.
    ::send(connectionSocket, response.c_str(), static_cast<int>(response.length()), sendFlags);
    closeHandle(connectionSocket);

    if (m_logEngine)
    {
        const Logger logger(*m_logEngine, "[Service] ");
        logger.debug(host.empty() ? "Plain HTTP request on the HTTPS port, with no host to redirect it to."
                                  : "Plain HTTP request on the HTTPS port, redirected to https://" + host + ".");
    }
}

shared_ptr<ControlServiceListener> ControlServiceListener::factory(IServerController*                controller,
                                                                   const std::shared_ptr<SSLKeys>&   sslKeys,
                                                                   const std::shared_ptr<LogEngine>& logEngine,
                                                                   const String&                     bindAddress,
                                                                   uint16_t                          servicePort)
{
    auto service = make_shared<ControlService>(controller);

    // Encrypted when there are keys to encrypt with, and that is the only thing that decides it:
    // a listener told it was encrypted without them would accept connections it cannot complete.
    const auto encrypted = sslKeys != nullptr;

    // Define Web Service listener
    const auto webfaceDirectory = DirectoryNames::webfaceDirectory();
    if (logEngine)
    {
        // Worth a line: on Windows the directory follows wherever the product was installed, and
        // a web interface that serves nothing looks the same as one whose files are missing.
        const Logger logger(*logEngine, "[Service] ");
        logger.debug("Web interface files are served from " + webfaceDirectory.string() + ".");
    }

    const WSConnection::Paths paths("index.html", "/test", webfaceDirectory.string());
    WSConnection::Options     options(paths);
    options.encrypted = encrypted;
    options.allowCors = true;
    const WSServices services(service);
    auto             serviceListener = make_shared<ControlServiceListener>(services, options, sslKeys, logEngine,
                                                                          bindAddress, servicePort);

    // Start Web Service listener
    auto connectionType = encrypted ? ServerConnection::Type::SSL : ServerConnection::Type::TCP;
    serviceListener->addListener(connectionType, {serviceListener->m_bindAddress, serviceListener->m_servicePort},
                                 controlListenerThreads);

    return serviceListener;
}
