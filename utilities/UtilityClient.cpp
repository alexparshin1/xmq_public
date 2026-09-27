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

#include "UtilityClient.h"

using namespace std;
using namespace sptk;

namespace xmq {

String UtilityClient::substituteTopic(const String& topic) const
{
    return topic.replace("%ClientIndex%", to_string(getClientIndex()));
}

void UtilityClient::setTopics(const Strings& topics)
{
    ranges::transform(topics, std::back_inserter(m_topics),
                      [this](const String& topic)
                      {
                          return substituteTopic(topic);
                      });
}

} // namespace xmq
