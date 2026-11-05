/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "RoundTripInterval.h"
#include "base/Message.h"
#include <atomic>
#include <functional>
#include <memory>

namespace xmq {

// Connect completions can arrive after a group's timeout or after its caller returns. Keep
// everything the callback touches alive independently of that caller's stack and engine.
struct ConnectProgress : std::enable_shared_from_this<ConnectProgress>
{
    std::atomic_size_t connected {0};
    sptk::Semaphore finished;
    RoundTripLatency latency;

    std::function<void(const SMessage&)> completion(size_t total, bool measure,
                                                   std::shared_ptr<std::atomic_bool> counted)
    {
        return [self = shared_from_this(), total, measure, counted = std::move(counted)](const SMessage&)
        {
            if (counted->exchange(true)) return;
            if (measure) self->latency.received();
            if (++self->connected == total) self->finished.post();
        };
    }
};
} // namespace xmq
