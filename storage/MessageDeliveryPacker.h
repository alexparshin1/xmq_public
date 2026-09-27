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

#include "Storage.h"
#include "base/MessageProperties.h"
#include "common/mqtt/PublishMessage.h"
#include "server/ClientSession/ClientSession.h"

#include <sptk5/Buffer.h>

namespace xmq {

class Server;

/**
 * @brief Pack Publish message to buffer and unpack it back.
 * @remark The primary usage is to read and write message from/to a database.
 * @remark The class is static (stateless).
 */
class XMQ_EXPORT MessageDeliveryPacker final
{
    static constexpr auto BooleanTrue = 'T';
    static constexpr auto BooleanFalse = 'F';
    static constexpr auto Marker_Int8 = 'c';
    static constexpr auto Marker_UInt8 = 'C';
    static constexpr auto Marker_Int16 = 'd';
    static constexpr auto Marker_UInt16 = 'D';
    static constexpr auto Marker_Int32 = 'i';
    static constexpr auto Marker_UInt32 = 'I';
    static constexpr auto Marker_Int64 = 'l';
    static constexpr auto Marker_UInt64 = 'L';
    static constexpr auto Marker_String = 's';
    static constexpr auto Marker_Buffer = 'b';
    static constexpr auto Marker_Properties = 'p';
    static constexpr auto Marker_DateTime = 't';
    static constexpr auto Marker_SubscriptionIdSet = 'S';

public:
    MessageDeliveryPacker() = default;

    MessageDeliveryPacker(const MessageDeliveryPacker&) = delete;
    MessageDeliveryPacker(MessageDeliveryPacker&&) = default;
    MessageDeliveryPacker& operator=(const MessageDeliveryPacker&) = delete;
    MessageDeliveryPacker& operator=(MessageDeliveryPacker&&) = default;

    /**
     * @brief Destructor.
     */
    ~MessageDeliveryPacker() = default;

    /**
     * @brief Pack object.
     * @return Packed data.
     */
    [[nodiscard]] static sptk::Buffer pack(const SMessageDelivery& message);

    /**
     * @brief Unpack data to Publish message.
     * @param session
     * @param sourceData        Packed source data.
     * @return message.
     */
    [[nodiscard]] static SMessageDelivery unpack(const SClientSession& session, const sptk::Buffer& sourceData);

private:
    struct UnpackState
    {
        uint8_t* m_readPtr {nullptr};
        uint8_t* m_endPtr {nullptr};
    };

    /**
     * @brief Append a boolean value to packed data.
     * @param packed            Output buffer.
     * @param value             The value.
     */
    static void write(sptk::Buffer& packed, const bool value)
    {
        packed.append(value ? BooleanTrue : BooleanFalse);
    }

    /**
     * @brief Append a byte value to packed data.
     * @param packed            Output buffer.
     * @param byte              The value.
     */
    static void write(sptk::Buffer& packed, const int8_t byte)
    {
        packed.append(Marker_Int8);
        packed.append(byte);
    }

    /**
     * @brief Append a byte value to packed data.
     * @param packed            Output buffer.
     * @param byte              The value.
     */
    static void write(sptk::Buffer& packed, const uint8_t byte)
    {
        packed.append(Marker_UInt8);
        packed.append(byte);
    }

    /**
     * @brief Append a 16-bit integer value to packed data.
     * @param packed            Output buffer.
     * @param value             The value.
     */
    static void write(sptk::Buffer& packed, const int16_t value)
    {
        packed.append(Marker_Int16);
        packed.append(value);
    }

    /**
     * @brief Append a 16-bit integer value to packed data.
     * @param packed            Output buffer.
     * @param value             The value.
     */
    static void write(sptk::Buffer& packed, const uint16_t value)
    {
        packed.append(Marker_UInt16);
        packed.append(value);
    }

    /**
     * @brief Append a 32-bit integer value to packed data.
     * @param packed            Output buffer.
     * @param value             The value.
     */
    static void write(sptk::Buffer& packed, const int32_t value)
    {
        packed.append(Marker_Int32);
        packed.append(value);
    }

    /**
     * @brief Append a 32-bit integer value to packed data.
     * @param packed            Output buffer.
     * @param value             The value.
     */
    static void write(sptk::Buffer& packed, const uint32_t value)
    {
        packed.append(Marker_UInt32);
        packed.append(value);
    }

    /**
     * @brief Append a 64-bit integer value to packed data.
     * @param packed            Output buffer.
     * @param value             The value.
     */
    static void write(sptk::Buffer& packed, const int64_t value)
    {
        packed.append(Marker_Int64);
        packed.append(value);
    }

    /**
     * @brief Append a 64-bit integer value to packed data.
     * @param packed            Output buffer.
     * @param value             The value.
     */
    static void write(sptk::Buffer& packed, const uint64_t value)
    {
        packed.append(Marker_UInt64);
        packed.append(value);
    }

    /**
     * @brief Append a datetime value to packed data.
     * @param packed            Output buffer.
     * @param value             The value.
     */
    static void write(sptk::Buffer& packed, const sptk::DateTime& value)
    {
        packed.append(Marker_DateTime);
        const uint64_t seconds = value.sinceEpoch().count();
        packed.append(seconds);
    }

    /**
     * @brief Append a string value to packed data.
     * @param packed            Output buffer.
     * @param value             The value.
     */
    static void write(sptk::Buffer& packed, const std::string_view value)
    {
        packed.append(Marker_String);
        packed.append<uint32_t>(static_cast<uint32_t>(value.size()));
        packed.append(value.data(), value.size());
    }

    /**
     * @brief Append a byte array to packed data.
     * @param packed            Output buffer.
     * @param buffer            The array.
     * @param size              The array size.
     */
    static void write(sptk::Buffer& packed, const uint8_t* buffer, const size_t size)
    {
        packed.append(Marker_Buffer);
        packed.append<uint32_t>(static_cast<uint32_t>(size));
        packed.append(buffer, size);
    }

    /**
     * @brief Append a binary buffer to packed data.
     * @param packed            Output buffer.
     * @param buffer            The buffer.
     */
    static void write(sptk::Buffer& packed, const sptk::Buffer& buffer)
    {
        packed.append(Marker_Buffer);
        packed.append<uint32_t>(static_cast<uint32_t>(buffer.size()));
        packed.append(buffer);
    }

    /**
     * @brief Append a set of integers to packed data.
     * @param packed            Output buffer.
     * @param uintSet           The set of integers.
     */
    static void write(sptk::Buffer& packed, const SubscriptionIdSet& uintSet)
    {
        packed.append(Marker_SubscriptionIdSet);
        packed.append<uint32_t>(static_cast<uint32_t>(uintSet.size()));
        for (const auto& item: uintSet)
        {
            packed.append(item);
        }
    }

    /**
     * @brief Append a message properties to packed data.
     * @param packed            Output buffer.
     * @param properties        The properties.
     */
    static void write(sptk::Buffer& packed, const SMessageProperties& properties);

    /**
     * @brief Read boolean value.
     * @param state         Unpack state.
     * @param value         Boolean value.
     */
    static void read(UnpackState& state, bool& value)
    {
        if (state.m_readPtr + 1 > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        if (*state.m_readPtr != BooleanTrue && *state.m_readPtr != BooleanFalse)
        {
            throw sptk::Exception("Invalid data: expect bool");
        }

        value = *state.m_readPtr == BooleanTrue;
        ++state.m_readPtr;
    }

    /**
     * @brief Read int8_t value.
     * @param state         Unpack state.
     * @param value         int8_t value.
     */
    static void read(UnpackState& state, int8_t& value)
    {
        constexpr auto propertySize {2};

        if (state.m_readPtr + propertySize > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        if (*state.m_readPtr != Marker_Int8)
        {
            throw sptk::Exception("Invalid data: expect int8");
        }
        value = *reinterpret_cast<int8_t*>(state.m_readPtr + 1);
        state.m_readPtr += propertySize;
    }

    /**
     * @brief Read uint8_t value.
     * @param state         Unpack state.
     * @param value         uint8_t value.
     */
    static void read(UnpackState& state, uint8_t& value)
    {
        constexpr auto propertySize {2};

        if (state.m_readPtr + propertySize > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        if (*state.m_readPtr != Marker_UInt8)
        {
            throw sptk::Exception("Invalid data: expect uint8");
        }
        value = *(state.m_readPtr + 1);
        state.m_readPtr += propertySize;
    }

    /**
     * @brief Read int16_t value.
     * @param state         Unpack state.
     * @param value         int16_t value.
     */
    static void read(UnpackState& state, int16_t& value)
    {
        constexpr auto propertySize {3};

        if (state.m_readPtr + propertySize > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        if (*state.m_readPtr != Marker_Int16)
        {
            throw sptk::Exception("Invalid data: expect int16");
        }
        value = *reinterpret_cast<int16_t*>(state.m_readPtr + 1);
        state.m_readPtr += propertySize;
    }

    /**
     * @brief Read uint16_t value.
     * @param state         Unpack state.
     * @param value         uint16_t value.
     */
    static void read(UnpackState& state, uint16_t& value)
    {
        constexpr auto propertySize {3};

        if (state.m_readPtr + propertySize > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        if (*state.m_readPtr != Marker_UInt16)
        {
            throw sptk::Exception("Invalid data: expect uint16");
        }
        value = *reinterpret_cast<uint16_t*>(state.m_readPtr + 1);
        state.m_readPtr += propertySize;
    }

    /**
     * @brief Read int32_t value.
     * @param state         Unpack state.
     * @param value         int32_t value.
     */
    static void read(UnpackState& state, int32_t& value)
    {
        constexpr auto propertySize {5};

        if (state.m_readPtr + propertySize > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        if (*state.m_readPtr != Marker_Int32)
        {
            throw sptk::Exception("Invalid data: expect int32");
        }
        value = *reinterpret_cast<int32_t*>(state.m_readPtr + 1);
        state.m_readPtr += propertySize;
    }

    /**
     * @brief Read uint32_t value.
     * @param state         Unpack state.
     * @param value         uint32_t value.
     */
    static void read(UnpackState& state, uint32_t& value)
    {
        constexpr auto propertySize {5};

        if (state.m_readPtr + propertySize > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        if (*state.m_readPtr != Marker_UInt32)
        {
            throw sptk::Exception("Invalid data: expect uint32");
        }
        value = *reinterpret_cast<uint32_t*>(state.m_readPtr + 1);
        state.m_readPtr += propertySize;
    }

    /**
     * @brief Read int64_t value.
     * @param state         Unpack state.
     * @param value         int64_t value.
     */
    static void read(UnpackState& state, int64_t& value)
    {
        constexpr auto propertySize {9};

        if (state.m_readPtr + propertySize > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        if (*state.m_readPtr != Marker_Int64)
        {
            throw sptk::Exception("Invalid data: expect int64");
        }
        value = *reinterpret_cast<int64_t*>(state.m_readPtr + 1);
        state.m_readPtr += propertySize;
    }

    /**
     * @brief Read uint64_t value.
     * @param state         Unpack state.
     * @param value         uint64_t value.
     */
    static void read(UnpackState& state, uint64_t& value)
    {
        constexpr auto propertySize {9};

        if (state.m_readPtr + propertySize > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        if (*state.m_readPtr != Marker_UInt64)
        {
            throw sptk::Exception("Invalid data: expect uint64");
        }

        value = *reinterpret_cast<uint64_t*>(state.m_readPtr + 1);
        state.m_readPtr += propertySize;
    }

    /**
     * @brief Read DateTime value.
     * @param state         Unpack state.
     * @param value         DateTime value.
     */
    static void read(UnpackState& state, sptk::DateTime& value)
    {
        constexpr auto propertySize {9};

        if (state.m_readPtr + propertySize > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        if (*state.m_readPtr != Marker_DateTime)
        {
            throw sptk::Exception("Invalid data: expect DateTime");
        }

        const auto seconds = *reinterpret_cast<uint64_t*>(state.m_readPtr + 1);
        value = sptk::DateTime(sptk::DateTime::duration(seconds));
        state.m_readPtr += propertySize;
    }

    /**
     * @brief Read string value.
     * @param state         Unpack state.
     * @param value         string value.
     */
    static void read(UnpackState& state, std::string_view& value)
    {
        constexpr auto propertyHeaderSize {5};

        if (state.m_readPtr + propertyHeaderSize > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        if (const auto marker = *state.m_readPtr;
            marker != Marker_String && marker != Marker_Buffer)
        {
            throw sptk::Exception("Invalid data: expect string or buffer");
        }

        const auto size = *reinterpret_cast<uint32_t*>(state.m_readPtr + 1);

        if (state.m_readPtr + propertyHeaderSize + size > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        state.m_readPtr += propertyHeaderSize;
        value = {reinterpret_cast<const char*>(state.m_readPtr), size};
        state.m_readPtr += size;
    }

    /**
     * @brief Read Buffer.
     * @param state         Unpack state.
     * @param value         Buffer.
     */
    static void read(UnpackState& state, sptk::Buffer& value)
    {
        constexpr auto propertyHeaderSize {5};

        if (state.m_readPtr + propertyHeaderSize > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        if (*state.m_readPtr != Marker_Buffer)
        {
            throw sptk::Exception("Invalid data: expect buffer");
        }

        const auto size = *reinterpret_cast<uint32_t*>(state.m_readPtr + 1);

        if (state.m_readPtr + propertyHeaderSize + size > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        uint8_t* data = state.m_readPtr + propertyHeaderSize;
        state.m_readPtr = data + size;
        value.set(data, size);
    }

    /**
     * @brief Read SubscriptionIdSet value.
     * @param state         Unpack state.
     * @param value         SubscriptionIdSet value.
     */
    static void read(UnpackState& state, SubscriptionIdSet& value)
    {
        constexpr auto propertyHeaderSize {5};

        if (state.m_readPtr + propertyHeaderSize > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        if (*state.m_readPtr != Marker_SubscriptionIdSet)
        {
            throw sptk::Exception("Invalid data: expect subscription id set");
        }

        const auto size = *reinterpret_cast<uint32_t*>(state.m_readPtr + 1);

        if (state.m_readPtr + propertyHeaderSize + size > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        auto* data = reinterpret_cast<uint32_t*>(state.m_readPtr + propertyHeaderSize);
        for (uint32_t i = 0; i < size; ++i)
        {
            value.push_back(*data);
            ++data;
        }
        state.m_readPtr = reinterpret_cast<uint8_t*>(data);
    }

    /**
     * @brief Read message properties.
     * @param state         Unpack state.
     * @param properties    Message properties to read.
     */
    static void read(UnpackState& state, MessageProperties& properties)
    {
        constexpr auto propertyHeaderSize {5};

        if (state.m_readPtr + propertyHeaderSize > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        if (*state.m_readPtr != Marker_Properties)
        {
            throw sptk::Exception("Invalid data: expect properties");
        }

        const auto size = *reinterpret_cast<uint32_t*>(state.m_readPtr + 1);

        if (state.m_readPtr + propertyHeaderSize + size > state.m_endPtr)
        {
            throw sptk::Exception("Attempt to read past the buffer end");
        }

        uint8_t* data = state.m_readPtr + propertyHeaderSize;
        state.m_readPtr = data + size;
        properties.read(data, size);
    }

    [[nodiscard]] static UnpackState startUnpacking(const sptk::Buffer& packetData)
    {
        auto* readPtr = const_cast<uint8_t*>(packetData.data());
        return UnpackState {
            .m_readPtr = readPtr,
            .m_endPtr = readPtr + packetData.size()};
    }
};

} // namespace xmq
