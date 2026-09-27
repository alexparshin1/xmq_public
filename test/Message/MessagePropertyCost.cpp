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

// What the properties cost, at the size they are actually used.
//
// Disabled, because it measures rather than asserts - run it by name:
//   ./xmq_unit_tests --gtest_also_run_disabled_tests --gtest_filter=*MessagePropertyCost*
//
// The size that matters is small: a CONNACK carries six, a CONNECT that says anything about itself
// carries three, and a PUBLISH usually carries none. That is the range where a hash map is at its
// worst - two allocations for the maps, one node per property, and a hash computed to find one of
// six entries - and it is the baseline any replacement has to beat. Measured 2026-09-12 before the
// buffer-and-index rewrite, so that "faster" can be a number rather than a conviction.

#include "base/MessageProperties.h"

#include <chrono>
#include <gtest/gtest.h>

using namespace std;
using namespace xmq;

namespace {

/**
 * @brief Keeps the optimiser from deleting the work being measured.
 *
 * Every loop below computes something nobody reads, which a release build is entitled to remove
 * entirely - and then the measurement is of an empty loop. This tells the compiler the value has
 * escaped, without generating an instruction.
 */
template<typename T>
void benchmarkSink(const T& value)
{
#ifdef _MSC_VER
    // MSVC has no inline assembler on x64. A volatile pointer does the same job here - the compiler
    // must materialise the address and may not reason about what happens to it afterwards.
    const volatile void* sink = &value;
    (void) sink;
#else
    asm volatile("" : : "r,m"(&value) : "memory");
#endif
}


/// The six a CONNACK carries, in the order the broker sets them.
constexpr array connectAckProperties {
    pair {Property::ReceiveMaximum, int64_t {32768}},
    pair {Property::MaximumPacketSize, int64_t {268435456}},
    pair {Property::TopicAliasMaximum, int64_t {128}},
    pair {Property::RetainAvailable, int64_t {1}},
    pair {Property::WildcardSubscriptionAvailable, int64_t {1}},
    pair {Property::SubscriptionIdentifierAvailable, int64_t {1}}};

template<typename Work>
double nanosecondsPer(const size_t repetitions, Work work)
{
    const auto started = chrono::steady_clock::now();
    for (size_t i = 0; i < repetitions; ++i)
    {
        work();
    }
    const auto elapsed = chrono::steady_clock::now() - started;
    return static_cast<double>(chrono::duration_cast<chrono::nanoseconds>(elapsed).count()) /
           static_cast<double>(repetitions);
}

TEST(XMQ_MessagePropertyCost, DISABLED_SixProperties)
{
    constexpr size_t repetitions = 200000;

    // Building one from nothing, which is what every MQTT5 connection does once.
    const auto build = nanosecondsPer(repetitions, [] {
        MessageProperties properties;
        for (const auto& [property, value]: connectAckProperties)
        {
            properties.setProperty(property, value);
        }
        benchmarkSink(properties);
    });

    MessageProperties filled;
    for (const auto& [property, value]: connectAckProperties)
    {
        filled.setProperty(property, value);
    }

    // Reading one of the six, which is what the connect path does three times.
    const auto readOne = nanosecondsPer(repetitions, [&filled] {
        int64_t value = 0;
        filled.getProperty(Property::TopicAliasMaximum, value);
        benchmarkSink(value);
    });

    // Reading a property that is not there - the answer on most lookups of most messages.
    const auto readAbsent = nanosecondsPer(repetitions, [&filled] {
        int64_t value = 0;
        filled.getProperty(Property::SessionExpiryInterval, value);
        benchmarkSink(value);
    });

    // Reading all six, which is what writing the block out does.
    const auto readAll = nanosecondsPer(repetitions, [&filled] {
        int64_t total = 0;
        for (const auto& [property, expected]: connectAckProperties)
        {
            int64_t value = 0;
            filled.getProperty(property, value);
            total += value;
        }
        benchmarkSink(total);
    });

    // Where the building time goes: the two maps themselves, against the allocation each property
    // costs. Measured by the slope, because that is the part a buffer-and-index representation
    // removes - the maps go with it, and the properties stop having a node each.
    const auto buildEmpty = nanosecondsPer(repetitions, [] {
        MessageProperties properties;
        benchmarkSink(properties);
    });

    const auto buildOne = nanosecondsPer(repetitions, [] {
        MessageProperties properties;
        properties.setProperty(Property::ReceiveMaximum, 32768);
        benchmarkSink(properties);
    });

    const auto buildThree = nanosecondsPer(repetitions, [] {
        MessageProperties properties;
        for (size_t i = 0; i < 3; ++i)
        {
            properties.setProperty(connectAckProperties[i].first, connectAckProperties[i].second);
        }
        benchmarkSink(properties);
    });

    cout << "\n  empty, construct+destroy: " << buildEmpty << " ns\n"
         << "  one property            : " << buildOne << " ns\n"
         << "  three properties        : " << buildThree << " ns\n"
         << "  six properties          : " << build << " ns\n"
         << "  -> per property         : " << (build - buildEmpty) / 6 << " ns\n"
         << "  getProperty, present    : " << readOne << " ns\n"
         << "  getProperty, absent     : " << readAbsent << " ns\n"
         << "  reading all six         : " << readAll << " ns\n"
         << "  (per connection the broker builds one set and reads three: about "
         << build + 3 * readOne << " ns)\n\n";
}

} // namespace
