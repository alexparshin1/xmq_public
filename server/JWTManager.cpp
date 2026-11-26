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
#include "JWTManager.h"
#include <sptk5/JWT.h>
#include <fstream>

using namespace std;
using namespace sptk;
using namespace chrono;
using namespace xmq;

JWTManager::JWTManager(const seconds tokenExpirationTime, filesystem::path stateFile)
    : m_stateFile(std::move(stateFile)), m_tokenExpirationTime(tokenExpirationTime)
{
    loadState();
    if (m_key.empty())
    {
        m_key = "ProTIS " + DateTime::Now().isoDateTimeString(DateTime::PrintAccuracy::SECONDS, true);
        saveStateUnlocked();
    }
}

void JWTManager::stateFile(filesystem::path path)
{
    SmartLock lock(m_mutex);
    if (path == m_stateFile)
    {
        return;
    }
    m_stateFile = std::move(path);
    loadState();
    if (m_key.empty())
    {
        m_key = "ProTIS " + DateTime::Now().isoDateTimeString(DateTime::PrintAccuracy::SECONDS, true);
    }
    saveStateUnlocked();
}

void JWTManager::loadState()
{
    if (m_stateFile.empty())
    {
        return;
    }
    ifstream input(m_stateFile);
    string  line;
    while (getline(input, line))
    {
        if (line.starts_with("key\t"))
        {
            m_key = line.substr(4);
        }
        else if (line.starts_with("token\t"))
        {
            m_issuedJwtMd5.insert(line.substr(6));
        }
    }
}

void JWTManager::saveStateUnlocked()
{
    if (m_stateFile.empty())
    {
        return;
    }
    const auto temporary = m_stateFile.string() + ".tmp";
    {
        ofstream output(temporary, ios::trunc);
        output << "key\t" << m_key << '\n';
        for (const auto& token: m_issuedJwtMd5)
        {
            output << "token\t" << token << '\n';
        }
    }
    filesystem::rename(temporary, m_stateFile);
    // The file holds the signing key: only the broker's account may read it.
    filesystem::permissions(m_stateFile, filesystem::perms::owner_read | filesystem::perms::owner_write);
}

JWTManager::~JWTManager()
{
    SmartLock lock(m_mutex);
    m_issuedJwtMd5.clear();
    m_timer.reset();
}

String JWTManager::generateToken(const CUser& user)
{
    static const String key256("012345678901234567890123456789XY");

    JWT jwt;
    jwt.set_alg(JWT::Algorithm::HS256, key256);

    const auto root = jwt.grants.root();
    auto       dataNode = root->pushNode("data");
    auto       userNode = dataNode->pushNode("user");
    user.unload(userNode);

    if (const auto userPassNode = userNode->findFirst("user_pass");
        userPassNode != nullptr)
    {
        userNode->remove(userPassNode);
    }

    DateTime issued = DateTime::Now();
    DateTime expire = issued + m_tokenExpirationTime;
    jwt.set("iss", issued.isoDateTimeString(DateTime::PrintAccuracy::MILLISECONDS, true));
    jwt.set("exp", expire.isoDateTimeString(DateTime::PrintAccuracy::MILLISECONDS, true));

    stringstream jwtStream;
    jwt.key = m_key;
    jwt.encode(jwtStream);

    auto jwtString = jwtStream.str();

    auto jwtMd5 = md5(jwtString);

    SmartLock lock(m_mutex);

    m_issuedJwtMd5.insert(jwtMd5);
    saveStateUnlocked();

    m_timer->fireAt(expire.timePoint(), [this, jwtMd5]
                    {
                        SmartLock lock2(m_mutex);
                        if (m_issuedJwtMd5.erase(jwtMd5) != 0)
                        {
                            saveStateUnlocked();
                        }
                    });

    return jwtString;
}

void JWTManager::releaseToken(const String& token)
{
    SmartLock lock(m_mutex);
    if (m_issuedJwtMd5.erase(md5(token)) != 0)
    {
        saveStateUnlocked();
    }
}

std::shared_ptr<JWT> JWTManager::decodeToken(const String& token) const
{
    const auto jwt = make_shared<JWT>();
    jwt->decode(token.c_str(), m_key.c_str());
    return jwt;
}

bool JWTManager::validateToken(const String& token)
{
    SharedSmartLock lock(m_mutex);
    const auto      tokenMd5 = md5(token);
    const auto      itor = m_issuedJwtMd5.find(tokenMd5);
    return itor != m_issuedJwtMd5.end();
}
