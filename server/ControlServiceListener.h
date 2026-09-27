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
#pragma once

#include <sptk5/cutils>

#include <sptk5/wsdl/WSConnection.h>
#include <sptk5/wsdl/WSServer.h>


namespace xmq {

class IServerController;


/**
 * @brief XMQ service listener.
 */
class ControlServiceListener final : public sptk::WSServer
{
public:
    sptk::String                     m_bindAddress {"0.0.0.0"}; ///< Address the interface accepts connections on
    uint16_t                         m_servicePort {18883}; ///< XMQ service port
    std::shared_ptr<sptk::SSLKeys>   m_sslKeys;             ///< Keys the interface is served with, or empty for plain HTTP
    std::shared_ptr<sptk::LogEngine> m_logEngine;           ///< XMQ service log engine

    ControlServiceListener(const sptk::WSServices& services, const sptk::WSConnection::Options& options,
                           const std::shared_ptr<sptk::SSLKeys>& sslKeys,
                           const std::shared_ptr<sptk::LogEngine>& logEngine, const sptk::String& bindAddress,
                           uint16_t servicePort);

    /**
     * @brief Build the interface listener and start it.
     * @param controller        Controller the service reaches the server through.
     * @param sslKeys           Keys to serve HTTPS with, or empty to serve plain HTTP.
     * @param logEngine         Where the listener logs.
     * @param bindAddress       Address to accept connections on.
     * @param servicePort       Port to serve on.
     */
    static std::shared_ptr<ControlServiceListener> factory(IServerController* controller,
                                                           const std::shared_ptr<sptk::SSLKeys>& sslKeys,
                                                           const std::shared_ptr<sptk::LogEngine>& logEngine,
                                                           const sptk::String& bindAddress,
                                                           uint16_t servicePort);

    /**
     * @brief Serve the interface at another address, port, or both, without restarting anything.
     *
     * Only the accepting socket changes. Connections already established are served to the end,
     * including the one that asked for this - so the reply saying the interface has moved still
     * reaches the browser that will follow it.
     *
     * A new port is taken before the old socket is given up, so a port that cannot be taken costs
     * nothing. The same port under another address cannot be done that way - the old socket holds
     * it - so there the old address is put back when the new one fails.
     *
     * @param bindAddress       Address to accept connections on.
     * @param port              Port to serve on.
     * @return true if the listening socket was replaced, false if it was already where it was
     *         asked to be.
     * @throws sptk::Exception when the address and port cannot be listened on.
     */
    bool bindTo(const sptk::String& bindAddress, uint16_t port);

    /**
     * @brief Serve the interface with another certificate, without restarting anything.
     *
     * The keys are read when a connection is accepted, not when the listener is built, so the
     * next browser to arrive gets the new certificate and the session that installed it is not
     * cut off. Connections already open keep the old one until they are made again.
     *
     * Replaces one certificate with another; it does not turn encryption on. A listener built
     * without keys accepts plain connections, which no certificate can change afterwards.
     *
     * @param sslKeys           Keys to serve with. Never empty.
     * @throws sptk::Exception when the interface is not serving HTTPS, or the keys cannot be used.
     */
    void useKeys(const std::shared_ptr<sptk::SSLKeys>& sslKeys);

    /**
     * @brief Serve the interface over the other scheme, without restarting anything.
     *
     * Whether a connection is encrypted is decided by the listening socket it arrived on, so
     * changing it means replacing that socket. Only the socket: connections already established
     * belong to the reactor and are served to the end, including the one that asked for this - so
     * the reply saying the interface has changed scheme still reaches the browser that will
     * follow it.
     *
     * The port is the same one, which is the whole difficulty: the old socket has to be closed
     * before the new one can take it. If the new one cannot be opened, the old scheme is put back
     * rather than leaving the interface with nothing listening at all.
     *
     * @param sslKeys           Keys to serve HTTPS with, or empty to serve plain HTTP.
     * @throws sptk::Exception when the port cannot be listened on again.
     */
    void serveEncrypted(const std::shared_ptr<sptk::SSLKeys>& sslKeys);

protected:
    /**
     * @brief Accept a connection, redirecting a plain HTTP request instead of failing a handshake.
     *
     * A browser given an address with no scheme - "myhost:18883" - sends an HTTP request, and a
     * port serving TLS answers it with a handshake the browser cannot report: it shows an empty
     * reply and no reason at all. Which of the two arrived is visible in the first byte, before
     * anything is decrypted, so the plain ones are sent to the same address under https:// rather
     * than dropped.
     *
     * @param connectionType    Incoming connection type.
     * @param connectionSocket  Accepted socket, before anything has been read from it.
     * @param peer              Incoming connection address.
     * @return The created connection, or empty when the request was answered and closed here.
     */
    std::shared_ptr<sptk::ServerConnection> createConnection(sptk::ServerConnection::Type connectionType,
                                                             SocketType                   connectionSocket,
                                                             const sockaddr_in*           peer) override;

private:
    /**
     * @brief Finish with a connection whose first byte is known.
     * @param socket            Accepted socket.
     * @param peer              Address it came from.
     * @param firstByte         What it said first, still unread in the socket.
     * @return the connection to be watched, or empty when the socket was dealt with here.
     */
    std::shared_ptr<sptk::ServerConnection> classify(SocketType socket, const sockaddr_in& peer,
                                                     uint8_t firstByte);

    /**
     * @brief Answer a plain HTTP request that arrived on the TLS port, then close it.
     *
     * Takes the socket over: it is closed here, and no connection object is made for it.
     *
     * @param connectionSocket  Accepted socket, with the request still unread.
     */
    void redirectToHttps(SocketType connectionSocket) const;
};

using SControlServiceListener = std::shared_ptr<ControlServiceListener>;

} // namespace xmq
