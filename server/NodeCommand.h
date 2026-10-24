#pragma once

#include "base/xmq.h"
#include <sptk5/String.h>
#include <chrono>
#include <filesystem>

namespace xmq {
/// Ask the running local node to join/leave through its authenticated control API.
/// Does not load Settings, change files, or start another server.
XMQ_EXPORT sptk::String controlNodeCluster(const std::filesystem::path& configuration,
                                         bool join, const sptk::String& username,
                                         const sptk::String& password,
                                         std::chrono::seconds timeout = std::chrono::seconds(30));
}
