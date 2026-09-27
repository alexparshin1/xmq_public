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

#include "PersistentObject.h"
#include "Storage.h"

using namespace std;
using namespace sptk;
using namespace xmq;

void PersistentObjectPacker::write(const IMessageProperties& properties) const
{
    const auto expectedPropertiesDataSize = properties.expectedSize();

    if (expectedPropertiesDataSize == 0)
    {
        return;
    }

    const auto expectedBufferSize = m_packed->size() + expectedPropertiesDataSize;
    m_packed->reserve(expectedBufferSize + 5); // Data type + data size
    m_packed->append('p');
    m_packed->append<uint32_t>(expectedPropertiesDataSize);
    auto* tail = m_packed->data() + m_packed->size();
    properties.write(tail, {});
    m_packed->bytes(expectedBufferSize + 5);
}
