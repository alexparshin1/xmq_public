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

#include "common/mqtt/VariableLength.h"
#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {
constexpr auto testNumber100 = 100;
constexpr auto testNumber127 = 127;
constexpr auto testNumber128 = 128;
constexpr auto testNumber16383 = 16383;
constexpr auto testNumber16384 = 16384;
constexpr auto testNumber2097151 = 2097151;
constexpr auto testNumber2097152 = 2097152;

constexpr auto oneByte = 1;
constexpr auto twoBytes = 2;
constexpr auto threeBytes = 3;
constexpr auto fourBytes = 4;

constexpr auto byte1 = 1;
constexpr auto byte2 = 2;
constexpr auto byte3 = 3;
} // namespace

TEST(XMQ_VariableLength, bytes)
{
    EXPECT_EQ(1, mqtt::VariableLength::bytes(0));
    EXPECT_EQ(1, mqtt::VariableLength::bytes(testNumber100));
    EXPECT_EQ(1, mqtt::VariableLength::bytes(testNumber127));
    EXPECT_EQ(2, mqtt::VariableLength::bytes(testNumber128));
    EXPECT_EQ(2, mqtt::VariableLength::bytes(testNumber16383));
    EXPECT_EQ(3, mqtt::VariableLength::bytes(testNumber16384));
    EXPECT_EQ(3, mqtt::VariableLength::bytes(testNumber2097151));
    EXPECT_EQ(4, mqtt::VariableLength::bytes(testNumber2097152));
}

TEST(XMQ_VariableLength, read)
{
    constexpr auto bufferSize = 5U;
    Buffer         data(bufferSize);

    data[0] = 0;
    auto* ptr = data.data();
    EXPECT_EQ(static_cast<uint64_t>(0), mqtt::VariableLength::read(ptr, bufferSize));
    EXPECT_EQ(data.data() + 1, ptr);

    data[0] = testNumber127;
    ptr = data.data();
    EXPECT_EQ(static_cast<uint64_t>(testNumber127), mqtt::VariableLength::read(ptr, bufferSize));
    EXPECT_EQ(data.data() + 1, ptr);

    data[0] = testNumber128;
    data[1] = 1;
    ptr = data.data();
    EXPECT_EQ(static_cast<uint64_t>(testNumber128), mqtt::VariableLength::read(ptr, bufferSize));
    EXPECT_EQ(data.data() + 2, ptr);

    data[0] = testNumber128;
    data[byte1] = testNumber128;
    data[byte2] = 1;
    ptr = data.data();
    EXPECT_EQ(static_cast<uint64_t>(testNumber16384), mqtt::VariableLength::read(ptr, bufferSize));
    EXPECT_EQ(data.data() + threeBytes, ptr);

    data[0] = testNumber128;
    data[byte1] = testNumber128;
    data[byte2] = testNumber128;
    data[byte3] = 1;
    ptr = data.data();
    EXPECT_EQ(static_cast<uint64_t>(testNumber2097152), mqtt::VariableLength::read(ptr, bufferSize));
    EXPECT_EQ(data.data() + fourBytes, ptr);
}

TEST(XMQ_VariableLength, write)
{
    constexpr auto bufferSize = 5U;
    Buffer         data(bufferSize);

    auto* ptr = data.data();
    mqtt::VariableLength::write(0, ptr);
    EXPECT_EQ(data.data() + oneByte, ptr);
    EXPECT_EQ(data[0], 0);

    ptr = data.data();
    mqtt::VariableLength::write(testNumber127, ptr);
    EXPECT_EQ(data.data() + oneByte, ptr);
    EXPECT_EQ(testNumber127, data[0]);

    ptr = data.data();
    mqtt::VariableLength::write(testNumber128, ptr);
    EXPECT_EQ(data.data() + twoBytes, ptr);
    EXPECT_EQ(data[0], testNumber128);
    EXPECT_EQ(data[byte1], 1);

    ptr = data.data();
    mqtt::VariableLength::write(testNumber16384, ptr);
    EXPECT_EQ(data.data() + threeBytes, ptr);
    EXPECT_EQ(data[0], testNumber128);
    EXPECT_EQ(data[byte1], testNumber128);
    EXPECT_EQ(data[byte2], 1);

    ptr = data.data();
    mqtt::VariableLength::write(testNumber2097152, ptr);
    EXPECT_EQ(data.data() + fourBytes, ptr);
    EXPECT_EQ(data[0], testNumber128);
    EXPECT_EQ(data[byte1], testNumber128);
    EXPECT_EQ(data[byte2], testNumber128);
    EXPECT_EQ(data[byte3], 1);
}
