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

#include "service/CUser.h"
#include <set>
#include <sptk5/JWT.h>
#include <sptk5/threads/SmartLock.h>
#include <sptk5/threads/Timer.h>

namespace xmq {

class JWTManager
{
public:
    explicit JWTManager(std::chrono::seconds tokenExpirationTime = std::chrono::hours(12));
    ~JWTManager();

    [[nodiscard]] sptk::String               generateToken(const CUser& user);
    bool                                     validateToken(const sptk::String& token);
    void                                     releaseToken(const sptk::String& token);
    [[nodiscard]] std::shared_ptr<sptk::JWT> decodeToken(const sptk::String& token) const;

private:
    sptk::SmartMutex             m_mutex;
    const sptk::String           m_key {"ProTIS " + sptk::DateTime::Now().isoDateTimeString(sptk::DateTime::PrintAccuracy::SECONDS, true)};
    std::set<sptk::String>       m_issuedJwtMd5;
    std::shared_ptr<sptk::Timer> m_timer {std::make_shared<sptk::Timer>()};
    std::chrono::seconds         m_tokenExpirationTime;
};

} // namespace xmq
