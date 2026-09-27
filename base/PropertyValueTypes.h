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

#include "base/Property.h"
#include <sptk5/cutils>

namespace xmq::mqtt {

enum class PropertyValueType
{
    Uint8,
    Uint16,
    Uint32,
    VarInt,
    String,
    Binary,
    NameValue
};

class XMQ_EXPORT PropertyValueTypes
{
public:
    [[nodiscard]] static PropertyValueType getValueType(Property property)
    {
        return m_propertyValueTypes[static_cast<uint8_t>(property)];
    }

    static void init();

private:
    static std::array<PropertyValueType, 256> m_propertyValueTypes;
};


} // namespace xmq::mqtt
