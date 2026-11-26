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

#include "base/xmq.h"
#include "service/CUser.h"
#include <filesystem>
#include <set>
#include <sptk5/JWT.h>
#include <sptk5/threads/SmartLock.h>
#include <sptk5/threads/Timer.h>

namespace xmq {

class XMQ_EXPORT JWTManager
{
public:
    explicit JWTManager(std::chrono::seconds tokenExpirationTime = std::chrono::hours(12),
                        std::filesystem::path stateFile = {});
    ~JWTManager();

    /**
     * @brief Where the signing key and the issued tokens are kept across restarts.
     *
     * A token issued before a restart has to remain valid afterwards: the interface keeps it in
     * the browser through a refresh, and a broker restarted in the middle of that refresh used to
     * answer "Not authenticated". Set from the control service once the configuration path is
     * known; empty in tests, where the old in-memory behaviour is what is being tested.
     */
    void stateFile(std::filesystem::path path);

    [[nodiscard]] sptk::String               generateToken(const CUser& user);
    bool                                     validateToken(const sptk::String& token);
    void                                     releaseToken(const sptk::String& token);
    [[nodiscard]] std::shared_ptr<sptk::JWT> decodeToken(const sptk::String& token) const;

private:
    sptk::SmartMutex             m_mutex;
    std::filesystem::path        m_stateFile;
    sptk::String                 m_key;
    std::set<sptk::String>       m_issuedJwtMd5;
    std::shared_ptr<sptk::Timer> m_timer{std::make_shared<sptk::Timer>()};
    std::chrono::seconds         m_tokenExpirationTime;

    void loadState();
    void saveStateUnlocked(); ///< Caller holds m_mutex.
};

} // namespace xmq