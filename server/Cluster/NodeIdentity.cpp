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

#include "NodeIdentity.h"

#include <sptk5/cutils>

#include <array>
#include <fstream>
#include <random>
#include <regex>

using namespace std;
using namespace sptk;
using namespace xmq::cluster;

string NodeIdentity::generate()
{
    random_device                      device;
    uniform_int_distribution<uint32_t> byte(0, 255);
    array<uint8_t, 16>                 bytes {};
    for (auto& value: bytes)
    {
        value = static_cast<uint8_t>(byte(device));
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40); // version 4: random
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80); // RFC 4122 variant

    string text;
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
        {
            text += '-';
        }
        text += format("{:02x}", bytes[i]);
    }
    return text;
}

bool NodeIdentity::isGuid(const string& text)
{
    static const regex guid("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    return regex_match(text, guid);
}

string NodeIdentity::load(const filesystem::path& file)
{
    if (ifstream input(file); input)
    {
        string text;
        getline(input, text);
        if (isGuid(text))
        {
            return text;
        }
    }

    const auto guid = generate();
    // Written beside and renamed over, so a crash in between leaves the old file or the new one,
    // never half of one - which would give the node a new identity on its next start.
    const auto temporary = filesystem::path(file).concat(".tmp");
    {
        ofstream output(temporary, ios::trunc);
        output << guid << "\n";
        if (!output.flush())
        {
            throw Exception(format("Can't write the node's identity to {}", temporary.string()));
        }
    }
    error_code errorCode;
    filesystem::rename(temporary, file, errorCode);
    if (errorCode)
    {
        throw Exception(format("Can't write the node's identity to {}: {}", file.string(), errorCode.message()));
    }
    return guid;
}
