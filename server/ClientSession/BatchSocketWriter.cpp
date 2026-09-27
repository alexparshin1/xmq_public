/*
╔══════════════════════════════════════════════════════════════════════════════╗
║                       XMQ Message QUEUE                                      ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  copyright            © 1999-2026 by Alexey Parshin                          ║
║  email                alexeyp@gmail.com                                      ║
║  code review                                                                 ║
╟──────────────────────────────────────────────────────────────────────────────╢
║  This Source Code Form is subject to the terms of the Mozilla Public         ║
║  License, v. 2.0. If a copy of the MPL was not distributed with this         ║
║  file, You can obtain one at https://mozilla.org/MPL/2.0/.                   ║
╚══════════════════════════════════════════════════════════════════════════════╝
*/

#include "BatchSocketWriter.h"
#include "base/ProtocolException.h"

using namespace std;
using namespace sptk;

namespace xmq {

namespace {

/**
 * @brief Walks the batch and writes each item through the socket itself.
 *
 * Works on every platform and with every socket type, TLS included - Socket::write() drives the
 * SSL session where there is one. It is also the behaviour the send path had before batching
 * existed, so introducing the interface changes nothing until a platform implementation is added
 * behind it.
 */
class PortableBatchSocketWriter
    : public BatchSocketWriter
{
public:
    void writeAll(span<SocketWriteItem> items) override
    {
        for (auto& item: items)
        {
            if (!item.m_socket || item.m_size == 0)
            {
                continue;
            }
            try
            {
                // Writes all of it or throws: this is the one implementation allowed to wait,
                // because it handles one socket at a time and nothing is queued behind it.
                item.m_written = item.m_socket->write(item.m_data, item.m_size);
            }
            catch (const ConnectionException&)
            {
                // The peer went away. The caller closes such sessions without logging, so leave
                // the reason empty to say exactly that.
                item.m_failed = true;
            }
            catch (const Exception& e)
            {
                item.m_failed = true;
                item.m_error = String(e.message());
            }
        }
    }

    [[nodiscard]] string_view name() const override
    {
        return "portable";
    }
};

} // namespace

UBatchSocketWriter createBatchSocketWriter()
{
    return make_unique<PortableBatchSocketWriter>();
}

} // namespace xmq
