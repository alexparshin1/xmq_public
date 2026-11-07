/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "server/Cluster/NodeIdentity.h"
#include <sptk5/net/RedisConnect.h>
#include <sptk5/net/RedisCommand.h>

namespace xmq {

/// Persistent identity of a logical Redis database, independent of its endpoint.
/// Lua makes first-use initialization atomic when several nodes start together.
inline std::string storageIdentity(sptk::RedisConnect& redis, const bool clusterIdentity = false)
{
    const std::string script =
        "local id = redis.call('GET', KEYS[1]); "
        "if not id then id = ARGV[1]; redis.call('SET', KEYS[1], id); end; return id";
    sptk::RedisCommand command("EVAL", script);
    command.emplace_back("1");
    command.emplace_back(clusterIdentity ? "cluster:id" : "storage:id");
    command.emplace_back(cluster::NodeIdentity::generate());
    std::vector<sptk::Variant> result;
    redis.executeCommand(command, result);
    if (result.empty() || !cluster::NodeIdentity::isGuid(result.front().asString()))
    {
        throw sptk::Exception("Redis returned an invalid storage identity.");
    }
    return result.front().asString();
}

} // namespace xmq
