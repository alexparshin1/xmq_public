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
#include <sptk5/xdoc/Document.h>

using namespace std;
using namespace sptk;
using namespace chrono;
using namespace xmq;

JWTManager::JWTManager(const seconds tokenExpirationTime)
    : m_tokenExpirationTime(tokenExpirationTime)
{
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

    const auto event = m_timer->fireAt(expire.timePoint(), [this, jwtMd5]
                                       {
                                           SmartLock lock2(m_mutex);
                                           m_issuedJwtMd5.erase(jwtMd5);
                                       });

    return jwtString;
}

void JWTManager::releaseToken(const String& token)
{
    SmartLock lock(m_mutex);
    m_issuedJwtMd5.erase(md5(token));
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
