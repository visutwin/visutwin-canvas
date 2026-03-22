// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace visutwin::canvas
{
    class ReadStream
    {
    public:
        explicit ReadStream(std::vector<uint8_t> data) : _data(std::move(data)) {}

        [[nodiscard]] size_t remainingBytes() const
        {
            return _data.size() - _offset;
        }

        void reset(const size_t offset = 0)
        {
            _offset = offset;
        }

        void skip(const size_t bytes)
        {
            _offset += bytes;
        }

        void align(const size_t bytes)
        {
            _offset = (_offset + bytes - 1) & (~(bytes - 1));
        }

        char readChar()
        {
            return static_cast<char>(readU8());
        }

        std::string readChars(const size_t numChars)
        {
            std::string result;
            result.reserve(numChars);
            for (size_t i = 0; i < numChars; ++i) {
                result += readChar();
            }
            return result;
        }

        uint8_t readU8()
        {
            ensure(1);
            return _data[_offset++];
        }

        uint16_t readU16()
        {
            const auto index = increment(2);
            return static_cast<uint16_t>(_data[index] | (_data[index + 1] << 8));
        }

        uint32_t readU32()
        {
            const auto index = increment(4);
            return static_cast<uint32_t>(_data[index] |
                (_data[index + 1] << 8) |
                (_data[index + 2] << 16) |
                (_data[index + 3] << 24));
        }

        uint64_t readU64()
        {
            const uint64_t low = readU32();
            const uint64_t high = readU32();
            return low + (high << 32);
        }

        uint32_t readU32be()
        {
            const auto index = increment(4);
            return static_cast<uint32_t>((_data[index] << 24) |
                (_data[index + 1] << 16) |
                (_data[index + 2] << 8) |
                _data[index + 3]);
        }

        void readArray(std::vector<uint8_t>& result)
        {
            for (auto& value : result) {
                value = readU8();
            }
        }

        std::string readLine()
        {
            std::string result;
            while (_offset < _data.size()) {
                const char c = readChar();
                if (c == '\n') {
                    break;
                }
                result += c;
            }
            return result;
        }

    private:
        void ensure(const size_t amount) const
        {
            if (_offset + amount > _data.size()) {
                throw std::out_of_range("ReadStream overflow");
            }
        }

        size_t increment(const size_t amount)
        {
            ensure(amount);
            _offset += amount;
            return _offset - amount;
        }

        std::vector<uint8_t> _data;
        size_t _offset = 0;
    };
}
