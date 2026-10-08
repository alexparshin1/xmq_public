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

#include <base/xmq.h>

#include <algorithm>
#include <array>
#include <sptk5/cnet>
#include <stdexcept>
#include <utility>
#include <vector>

namespace xmq {

/**
 * @brief Generic protocol packet.
 */
class Packet final
{
public:
    /**
     * @brief Constructor.
     * @param dataSize          Packet data size.
     */
    explicit Packet(const size_t dataSize = 0)
        : m_data(dataSize)
    {
    }

    /**
     * @brief Constructor.
     * @param header            Packet header.
     * @param headerSize        Packet.
     * @param dataSize          Packet data size
     */
    Packet(const uint8_t* header, const uint16_t headerSize, const size_t dataSize)
        : m_data(dataSize)
    {
        setHeader(header, headerSize);
    }

    Packet(const uint8_t* header, const uint16_t headerSize, const uint8_t* payload, const size_t dataSize)
        : m_data(payload, payload + dataSize)
    {
        setHeader(header, headerSize);
    }

    /**
     * @brief Copy constructor
     * @param other             The other packet
     */
    Packet(const Packet& other) = delete;
    Packet(Packet&& other) noexcept = default;
    Packet& operator=(const Packet& other) = delete;
    Packet& operator=(Packet&& other) noexcept = default;

    ~Packet() = default;

    [[nodiscard]] Packet clone() const;

    [[nodiscard]] uint8_t* data() { return m_data.data(); }
    [[nodiscard]] const uint8_t* data() const { return m_data.data(); }
    [[nodiscard]] size_t bytes() const { return m_data.size(); }
    void bytes(const size_t size) { m_data.resize(size); }

    /**
     * @brief Release the packet body, leaving the read state behind.
     *
     * PublishMessage keeps only the body once it has been parsed; taking the
     * vector moves it out without a copy.
     */
    std::vector<uint8_t> takeData() && { return std::move(m_data); }

    [[nodiscard]] size_t fullSize() const
    {
        return m_headerSize + bytes();
    }

    void setHeaderSize(const size_t headerSize)
    {
        m_headerSize = headerSize;
    }

    /**
     * @brief Get packet header
     */
    template<typename T>
    const T& header() const
    {
        return *std::bit_cast<const T*>(m_header.data());
    }

    /**
     * Receive 8-bit integer
     * @returns 8-bit value
     */
    uint8_t readByte()
    {
        if (m_offset >= bytes())
        {
            throw sptk::Exception("MQTT packet overflow");
        }
        const auto value = *(data() + m_offset);
        ++m_offset;
        return value;
    }

    /**
     * Receive a 16-bit integer and swaps its bytes
     * @returns 16-bit value
     */
    uint16_t readShortInteger()
    {
        const auto value = *std::bit_cast<uint16_t*>(data() + m_offset);
        m_offset += sizeof(uint16_t);
        return ntohs(value);
    }

    /**
     * @brief Receive message id as a 16-bit integer
     * @return short integer
     */
    uint16_t readId()
    {
        return readShortInteger();
    }

    /**
     * @brief Read data
     * @param destination       Read destination
     */
    template<typename T>
    void readData(T& destination)
    {
        memcpy(&destination, data() + m_offset, sizeof(T));
        m_offset += sizeof(T);
    }

    /**
     * @brief Read data
     * @param destination       Read destination
     * @param length            Read length
     */
    template<typename T>
    void readData(T* destination, const size_t length)
    {
        if (m_offset + length > bytes())
        {
            throw sptk::Exception("MQTT packet overflow");
        }
        memcpy(destination, data() + m_offset, length);
        m_offset += length;
    }

    /**
     * Receive character vector as the 16-bit length (Big Endian) followed by the string characters
     * @returns received char vector
     */
    std::string_view readString()
    {
        const uint16_t         len = readShortInteger();
        const std::string_view buffer(std::bit_cast<char*>(data() + m_offset), len);
        m_offset += len;
        return buffer;
    }

    /**
     * @brief Read variable length
     * @param multiplier        Multiplier: 1 if reading from the first byte, 128 if reading from the second getByte
     * @return Variable length
     */
    uint32_t readVariableLength(unsigned multiplier = 1);

    [[nodiscard]] const std::array<uint8_t, 2>& header() const
    {
        return m_header;
    }

private:
    void setHeader(const uint8_t* header, const uint16_t headerSize)
    {
        if (headerSize != m_header.size())
        {
            throw std::invalid_argument("Packet fixed header must be two bytes");
        }
        std::copy_n(header, m_header.size(), m_header.begin());
    }

    std::vector<uint8_t> m_data; ///< Packet body
    size_t m_headerSize {0}; ///< Full wire header size, including variable-length bytes
    std::array<uint8_t, 2> m_header {}; ///< MQTT fixed header
    size_t m_offset {0};     ///< Packet read offset
};

} // namespace xmq
