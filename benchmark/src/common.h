#pragma once

#include <array>
#include <ostream>

using lru_key_t   = uint64_t;
using lru_value_t = std::array<uint64_t, 2>;

template <typename T, size_t Size>
std::ostream& operator<<(std::ostream& out, const std::array<T, Size>& arr) {
    out << '[';
    for (size_t i = 0; i < Size; i++) {
        if (i > 0) {
            out << ", ";
        }
        out << arr[i];
    }
    out << ']';
    return out;
}
