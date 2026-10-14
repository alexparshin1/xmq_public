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

#include "common/ReadBufferSizer.h"

#include <gtest/gtest.h>

using namespace std;
using namespace sptk;
using namespace xmq;

namespace {

/// A read buffer as a session leaves it after reading a packet of this size and emptying it.
void readAndEmpty(Buffer& buffer, ReadBufferSizer& sizer, const size_t bytes)
{
    buffer.reserve(bytes);
    buffer.bytes(bytes);
    sizer.packet(bytes);
    buffer.bytes(0);
    sizer.settle(buffer);
}

} // namespace

/**
 * Scenario: A session that reads small packets reads one large one, then small ones again.
 *
 * Verification: The buffer grows for the large packet and is given back once the small ones
 * follow - to about one and a half times their size, not to nothing.
 */
TEST(ReadBufferSizerTests, aLargePacketIsGivenBackWhenSmallOnesFollow)
{
    Buffer          buffer;
    ReadBufferSizer sizer;
    for (int i = 0; i < 50; ++i)
    {
        readAndEmpty(buffer, sizer, 100);
    }

    buffer.reserve(1024 * 1024);
    buffer.bytes(1024 * 1024);
    sizer.packet(1024 * 1024);
    EXPECT_GE(buffer.capacity(), 1024U * 1024U);

    // The large packet still pulls the average up; the small ones bring it down.
    for (int i = 0; i < 200; ++i)
    {
        readAndEmpty(buffer, sizer, 100);
    }
    EXPECT_LT(buffer.capacity(), 4096U);
    EXPECT_GE(buffer.capacity(), static_cast<size_t>(ReadBufferSizer::MinimumBytes));
}

/**
 * Scenario: A buffer still holding part of a packet.
 *
 * Verification: It is left alone, whatever its size: the rest of the packet is to be read into it.
 */
TEST(ReadBufferSizerTests, aBufferThatHoldsBytesIsNotTouched)
{
    Buffer          buffer(1024 * 1024);
    ReadBufferSizer sizer;
    buffer.bytes(10);
    sizer.settle(buffer);
    EXPECT_GE(buffer.capacity(), 1024U * 1024U);
    EXPECT_EQ(10U, buffer.bytes());
}

/**
 * Scenario: Packets alternate between two sizes close to each other.
 *
 * Verification: The buffer is not reallocated back and forth: it is given back only when it is
 * well above what is read.
 */
TEST(ReadBufferSizerTests, sizesCloseToEachOtherDoNotReallocate)
{
    Buffer          buffer;
    ReadBufferSizer sizer;
    for (int i = 0; i < 50; ++i)
    {
        readAndEmpty(buffer, sizer, i % 2 == 0 ? 1500 : 1000);
    }
    const auto* data = buffer.data();
    for (int i = 0; i < 50; ++i)
    {
        readAndEmpty(buffer, sizer, i % 2 == 0 ? 1500 : 1000);
    }
    EXPECT_EQ(data, buffer.data());
}
