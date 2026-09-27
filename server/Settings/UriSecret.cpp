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


#include "UriSecret.h"

using namespace std;
using namespace sptk;
using namespace xmq;

void UriSecret::findPassword(const String& uri, size_t& start, size_t& length)
{
    start = String::npos;
    length = 0;

    const auto schemeEnd = uri.find("://");
    if (schemeEnd == String::npos)
    {
        return;
    }
    const auto authorityStart = schemeEnd + 3;

    // The last '@' before the path, because a password may contain one and a host may not. Anything
    // after the first '/' of the path is not the authority and cannot hold credentials.
    const auto pathStart = uri.find('/', authorityStart);
    const auto authorityEnd = pathStart == String::npos ? uri.length() : pathStart;

    const auto credentialsEnd = uri.rfind('@', authorityEnd);
    if (credentialsEnd == String::npos || credentialsEnd < authorityStart)
    {
        // No credentials at all.
        return;
    }

    // The first ':' after the scheme separates the username from the password. Taking the last one
    // instead would eat a password that contains a colon, which is allowed.
    const auto separator = uri.find(':', authorityStart);
    if (separator == String::npos || separator > credentialsEnd)
    {
        // A username and no password, which is a URI with nothing to hide.
        return;
    }

    start = separator + 1;
    length = credentialsEnd - start;
}

String UriSecret::hidden(const String& uri)
{
    size_t start = 0;
    size_t length = 0;
    findPassword(uri, start, length);

    if (start == String::npos || length == 0)
    {
        return uri;
    }

    return uri.substr(0, start) + String(string(mask)) + uri.substr(start + length);
}

String UriSecret::restored(const String& given, const String& stored)
{
    size_t givenStart = 0;
    size_t givenLength = 0;
    findPassword(given, givenStart, givenLength);

    if (givenStart == String::npos ||
        given.substr(givenStart, givenLength) != String(string(mask)))
    {
        // A real password, or none: what was sent is what was meant, and this is how a password is
        // changed. Notably it is also how one is cleared.
        return given;
    }

    size_t storedStart = 0;
    size_t storedLength = 0;
    findPassword(stored, storedStart, storedLength);
    if (storedStart == String::npos)
    {
        // The mask came back for a URI that has no password on record - so there is nothing to put
        // back, and keeping the mask would store it as the password.
        return given.substr(0, givenStart) + given.substr(givenStart + givenLength);
    }

    return given.substr(0, givenStart) + stored.substr(storedStart, storedLength) +
           given.substr(givenStart + givenLength);
}
