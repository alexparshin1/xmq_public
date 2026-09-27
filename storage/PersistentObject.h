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
#include <sptk5/Buffer.h>

#include <utility>

namespace xmq {

class Server;

class XMQ_EXPORT PersistentObjectPacker
{
public:
    PersistentObjectPacker() = default;

    PersistentObjectPacker(const PersistentObjectPacker&) = delete;
    PersistentObjectPacker(PersistentObjectPacker&&) = default;
    PersistentObjectPacker& operator=(const PersistentObjectPacker&) = delete;
    PersistentObjectPacker& operator=(PersistentObjectPacker&&) = default;

    /**
     * @brief Destructor.
     */
    virtual ~PersistentObjectPacker() = default;

    /**
     * @brief Pack object to Buffer.
     * @remarks Used for RedisConnection.
     */
    virtual void pack(sptk::Buffer&)
    {
    }

protected:
    /**
     * @brief Append a boolean value to packed data.
     * @param value             The value.
     */
    void write(const bool value) const
    {
        m_packed->append(value ? 'T' : 'F');
    }

    /**
     * @brief Append a byte value to packed data.
     * @param byte              The value.
     */
    void write(const int8_t byte) const
    {
        m_packed->append('c');
        m_packed->append(byte);
    }

    /**
     * @brief Append a byte value to packed data.
     * @param byte              The value.
     */
    void write(const uint8_t byte) const
    {
        m_packed->append('C');
        m_packed->append(byte);
    }

    /**
     * @brief Append a 16-bit integer value to packed data.
     * @param value             The value.
     */
    void write(const int16_t value) const
    {
        m_packed->append('d');
        m_packed->append(value);
    }

    /**
     * @brief Append a 16-bit integer value to packed data.
     * @param value             The value.
     */
    void write(const uint16_t value) const
    {
        m_packed->append('D');
        m_packed->append(value);
    }

    /**
     * @brief Append a 32-bit integer value to packed data.
     * @param value             The value.
     */
    void write(const int32_t value) const
    {
        m_packed->append('i');
        m_packed->append(value);
    }

    /**
     * @brief Append a 32-bit integer value to packed data.
     * @param value             The value.
     */
    void write(const uint32_t value) const
    {
        m_packed->append('I');
        m_packed->append(value);
    }

    /**
     * @brief Append a 64-bit integer value to packed data.
     * @param value             The value.
     */
    void write(const int64_t value) const
    {
        m_packed->append('l');
        m_packed->append(value);
    }

    /**
     * @brief Append a 64-bit integer value to packed data.
     * @param value             The value.
     */
    void write(const uint64_t value) const
    {
        m_packed->append('L');
        m_packed->append(value);
    }

    /**
     * @brief Append a datetime value to packed data.
     * @param value             The value.
     */
    void write(const sptk::DateTime& value) const
    {
        m_packed->append('X');
        const uint64_t seconds = value.sinceEpoch().count();
        m_packed->append(seconds);
    }

    /**
     * @brief Append a string value to packed data.
     * @param value             The value.
     */
    void write(const std::string_view value) const
    {
        m_packed->append('s');
        m_packed->append<uint32_t>(static_cast<uint32_t>(value.size()));
        m_packed->append(value.data(), value.size());
    }

    /**
     * @brief Append a byte array to packed data.
     * @param buffer            The array.
     * @param size              The array size.
     */
    void write(const uint8_t* buffer, const size_t size) const
    {
        m_packed->append('b');
        m_packed->append<uint32_t>(static_cast<uint32_t>(size));
        m_packed->append(buffer, size);
    }

    /**
     * @brief Append a binary buffer to packed data.
     * @param buffer            The buffer.
     */
    void write(const sptk::Buffer& buffer) const
    {
        m_packed->append('b');
        m_packed->append<uint32_t>(static_cast<uint32_t>(buffer.size()));
        m_packed->append(buffer);
    }

    /**
     * @brief Append a set of integers to packed data.
     * @param uintSet           The set of integers.
     */
    void write(const SubscriptionIdSet& uintSet) const
    {
        m_packed->append('a');
        m_packed->append(static_cast<uint32_t>(uintSet.size()));
        for (const auto& value: uintSet)
        {
            m_packed->append(value);
        }
    }

    /**
     * @brief Append message properties to the packed data.
     * @param properties        The value.
     */
    void write(const IMessageProperties& properties) const;

    void startPacking(sptk::Buffer& destinationData)
    {
        m_packed = &destinationData;
        m_packed->bytes(0);
    }

    [[nodiscard]] std::string_view packed() const
    {
        return {m_packed->c_str(), m_packed->size()};
    }

private:
    sptk::Buffer* m_packed {nullptr};
};

class XMQ_EXPORT PersistentObjectUnpacker
{
public:
    PersistentObjectUnpacker() = default;

    PersistentObjectUnpacker(const PersistentObjectUnpacker&) = delete;
    PersistentObjectUnpacker(PersistentObjectUnpacker&&) = default;
    PersistentObjectUnpacker& operator=(const PersistentObjectUnpacker&) = delete;
    PersistentObjectUnpacker& operator=(PersistentObjectUnpacker&&) = default;

    virtual ~PersistentObjectUnpacker() = default;

    /**
     * @brief Unpack data to the object.
     * @param sourceData        Packed source data
     */
    virtual void unpack(const sptk::Buffer& sourceData) = 0;

protected:
    void read(bool& value)
    {
        if (*m_readPtr != 'T' && *m_readPtr != 'F')
        {
            throw sptk::Exception("Invalid data: expect bool");
        }
        value = *m_readPtr == 'T';
        ++m_readPtr;
    }

    void read(int8_t& value)
    {
        constexpr auto propertySize {2};
        if (*m_readPtr != 'c')
        {
            throw sptk::Exception("Invalid data: expect int8");
        }
        value = *std::bit_cast<int8_t*>(m_readPtr + 1);
        m_readPtr += propertySize;
    }

    void read(uint8_t& value)
    {
        constexpr auto propertySize {2};
        if (*m_readPtr != 'C')
        {
            throw sptk::Exception("Invalid data: expect uint8");
        }
        value = *(m_readPtr + 1);
        m_readPtr += propertySize;
    }

    void read(int16_t& value)
    {
        constexpr auto propertySize {3};
        if (*m_readPtr != 'd')
        {
            throw sptk::Exception("Invalid data: expect int16");
        }
        value = *std::bit_cast<int16_t*>(m_readPtr + 1);
        m_readPtr += propertySize;
    }

    void read(uint16_t& value)
    {
        constexpr auto propertySize {3};
        if (*m_readPtr != 'D')
        {
            throw sptk::Exception("Invalid data: expect uint16");
        }
        value = *std::bit_cast<uint16_t*>(m_readPtr + 1);
        m_readPtr += propertySize;
    }

    void read(int32_t& value)
    {
        constexpr auto propertySize {5};
        if (*m_readPtr != 'i')
        {
            throw sptk::Exception("Invalid data: expect int32");
        }
        value = *std::bit_cast<int32_t*>(m_readPtr + 1);
        m_readPtr += propertySize;
    }

    void read(uint32_t& value)
    {
        constexpr auto propertySize {5};
        if (*m_readPtr != 'I')
        {
            throw sptk::Exception("Invalid data: expect uint32");
        }
        value = *std::bit_cast<uint32_t*>(m_readPtr + 1);
        m_readPtr += propertySize;
    }

    void read(int64_t& value)
    {
        constexpr auto propertySize {9};
        if (*m_readPtr != 'l')
        {
            throw sptk::Exception("Invalid data: expect int64");
        }
        value = *std::bit_cast<int64_t*>(m_readPtr + 1);
        m_readPtr += propertySize;
    }

    void read(uint64_t& value)
    {
        constexpr auto propertySize {9};
        if (*m_readPtr != 'L')
        {
            throw sptk::Exception("Invalid data: expect uint64");
        }
        value = *std::bit_cast<uint64_t*>(m_readPtr + 1);
        m_readPtr += propertySize;
    }

    void read(sptk::DateTime& value)
    {
        constexpr auto propertySize {9};
        if (*m_readPtr != 'X')
        {
            throw sptk::Exception("Invalid data: expect DateTime");
        }
        const auto seconds = *std::bit_cast<uint64_t*>(m_readPtr + 1);
        value = sptk::DateTime(sptk::DateTime::duration(seconds));
        m_readPtr += propertySize;
    }

    void read(std::string& value)
    {
        constexpr auto propertyHeaderSize {5};
        if (*m_readPtr != 's')
        {
            throw sptk::Exception("Invalid data: expect string");
        }
        const auto size = *std::bit_cast<uint32_t*>(m_readPtr + 1);
        m_readPtr += propertyHeaderSize;
        value = std::string(std::bit_cast<const char*>(m_readPtr), size);
        m_readPtr += size;
    }

    void read(sptk::Buffer& buffer)
    {
        constexpr auto propertyHeaderSize {5};
        if (*m_readPtr != 'b')
        {
            throw sptk::Exception("Invalid data: expect buffer");
        }
        const auto size = *std::bit_cast<uint32_t*>(m_readPtr + 1);
        uint8_t*   data = m_readPtr + propertyHeaderSize;
        m_readPtr = data + size;
        buffer.set(data, size);
    }

    void read(uint8_t* buffer, size_t bufferSize)
    {
        constexpr auto propertyHeaderSize {5};
        if (*m_readPtr != 'b')
        {
            throw sptk::Exception("Invalid data: expect buffer");
        }
        const auto size = *std::bit_cast<uint32_t*>(m_readPtr + 1);
        if (size > bufferSize)
        {
            throw sptk::Exception("Invalid data: buffer too small");
        }
        uint8_t* data = m_readPtr + propertyHeaderSize;
        m_readPtr = data + size;
        memcpy(buffer, data, size);
    }

    void read(SubscriptionIdSet& uintSet)
    {
        constexpr auto propertyHeaderSize {5};
        if (*m_readPtr != 'a')
        {
            throw sptk::Exception("Invalid data: expect buffer");
        }
        const auto size = *std::bit_cast<uint32_t*>(m_readPtr + 1);
        auto*      data = std::bit_cast<uint32_t*>(m_readPtr + propertyHeaderSize);
        for (uint32_t i = 0; i < size; ++i)
        {
            uintSet.push_back(*data);
            ++data;
        }
        m_readPtr = std::bit_cast<uint8_t*>(data);
    }

    void read(MessageProperties& properties)
    {
        constexpr auto propertyHeaderSize {5};
        if (*m_readPtr != 'p')
        {
            throw sptk::Exception("Invalid data: expect properties");
        }
        const auto size = *std::bit_cast<uint32_t*>(m_readPtr + 1);
        uint8_t*   data = m_readPtr + propertyHeaderSize;
        m_readPtr = data + size;
        properties.read(data, size);
    }

    void startUnpacking(const std::string_view packetData)
    {
        m_readPtr = std::bit_cast<uint8_t*>(packetData.data());
        m_endPtr = m_readPtr + packetData.size();
    }

    /**
     * @brief Check whether any unread data remains in the record.
     *
     * Records are read back in the order they were written, with no length or version header, so
     * a field appended to a newer format is simply absent from a record written by an older
     * version. Guarding such a read with this lets both formats be read without a migration.
     *
     * @return True if at least one more byte is available.
     */
    [[nodiscard]] bool hasMoreData() const
    {
        return m_readPtr != nullptr && m_readPtr < m_endPtr;
    }

private:
    uint8_t*       m_readPtr {nullptr};
    const uint8_t* m_endPtr {nullptr};
};

/**
 * @brief Base class for all persistent objects
 */
class XMQ_EXPORT PersistentObject
    : public PersistentObjectPacker
    , public PersistentObjectUnpacker
    , public std::enable_shared_from_this<PersistentObject>
{
public:
    /**
     * @brief Constructor
     * @param redis             Redis.
     */
    explicit PersistentObject(sptk::SRedisConnect redis)
        : m_redis(std::move(redis))
    {
    }

    /**
     * @brief Copy constructor
     */
    PersistentObject(const PersistentObject&) = delete;

    /**
     * @brief Move constructor
     */
    PersistentObject(PersistentObject&&) = delete;

    /**
     * @brief Copy assignment
     */
    PersistentObject& operator=(const PersistentObject&) = delete;

    /**
     * @brief Move assignment
     */
    PersistentObject& operator=(PersistentObject&&) = delete;

    /**
     * @brief Destructor
     */
    ~PersistentObject() override = default;

    /**
     * @brief Remove this object from the storage
     */
    virtual void storeRecordAsync(const std::function<void()>& callback) = 0;

    /**
     * @brief Remove this object from the storage
     */
    virtual void removeRecordAsync(const std::function<void(const size_t&)>& callback)
    {
        if (m_redis)
        {
            m_redis->deleteKeysAsync({m_key}, callback);
        }
    }

    [[nodiscard]] virtual sptk::SRedisConnect getRedis() const
    {
        return m_redis;
    }

    virtual void clearStorage()
    {
        m_redis = nullptr;
    }

    virtual std::string getKey() const
    {
        return m_key;
    }

private:
    sptk::SRedisConnect m_redis;
    std::string         m_key;
};

} // namespace xmq
