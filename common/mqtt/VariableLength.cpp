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

#include "VariableLength.h"

using namespace std;
using namespace sptk;

namespace {
constexpr uint8_t digitMask = 127;
constexpr uint8_t allDigitsValue = 128;
constexpr uint8_t continueBit = 128;
} // namespace

namespace xmq::mqtt {

uint64_t VariableLength::read(uint8_t*& data, int availableBytes)
{
    uint64_t value = 0;
    uint64_t multiplier = 1;

    if (data == nullptr)
    {
        return 0;
    }

    while (availableBytes > 0)
    {
        const auto digit = *data++;
        value += static_cast<uint64_t>(digit & digitMask) * multiplier;
        multiplier *= allDigitsValue;
        availableBytes--;
        if ((digit & continueBit) == 0)
        {
            break;
        }
        if (availableBytes < 0)
        {
            throw Exception("Not enough bytes to read variable length integer");
        }
    }

    return value;
}

void VariableLength::write(uint64_t value, uint8_t*& data)
{
    while (data != nullptr)
    {
        const auto digit = static_cast<uint8_t>(value % allDigitsValue);
        value /= allDigitsValue;
        if (value != 0)
        {
            *data++ = digit | continueBit;
        }
        else
        {
            *data++ = digit;
            break;
        }
    }
}

uint8_t VariableLength::bytes(uint64_t value)
{
    if (value == 0)
    {
        return 1;
    }

    uint8_t bytes = 0;
    while (value != 0)
    {
        value /= allDigitsValue;
        ++bytes;
    }

    return bytes;
}

} // namespace xmq::mqtt
