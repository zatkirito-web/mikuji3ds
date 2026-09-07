// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <functional>
#include "core/hw/aes/arithmetic128.h"

namespace HW::AES {

AESKey Lrot128(const AESKey& in, u32 rot) {
    AESKey out;
    rot %= 128;
    const u32 byte_shift = rot / 8;
    const u32 bit_shift = rot % 8;

    for (u32 i = 0; i < 16; i++) {
        const u32 wrap_index_a = (i + byte_shift) % 16;
        const u32 wrap_index_b = (i + byte_shift + 1) % 16;
        out[i] = ((in[wrap_index_a] << bit_shift) | (in[wrap_index_b] >> (8 - bit_shift))) & 0xFF;
    }
    return out;
}

AESKey Rrot128(const AESKey& in, u32 rot) {
    return Lrot128(in, (128 - (rot % 128)) % 128);
}

AESKey Add128(const AESKey& a, const AESKey& b) {
    AESKey out;
    u32 carry = 0;
    u32 sum = 0;

    for (int i = 15; i >= 0; i--) {
        sum = a[i] + b[i] + carry;
        carry = sum >> 8;
        out[i] = static_cast<u8>(sum & 0xff);
    }

    return out;
}

AESKey Add128(const AESKey& a, u64 b) {
    AESKey out = a;
    u32 carry = 0;
    u32 sum = 0;

    for (int i = 15; i >= 8; i--) {
        sum = a[i] + static_cast<u8>((b >> ((15 - i) * 8)) & 0xff) + carry;
        carry = sum >> 8;
        out[i] = static_cast<u8>(sum & 0xff);
    }

    return out;
}

AESKey Sub128(const AESKey& a, const AESKey& b) {
    AESKey out;
    u32 borrow = 0;

    for (int i = 15; i >= 0; i--) {
        const s32 diff = static_cast<s32>(a[i]) - static_cast<s32>(b[i]) - static_cast<s32>(borrow);
        borrow = diff < 0 ? 1 : 0;
        out[i] = static_cast<u8>(diff & 0xff);
    }

    return out;
}

AESKey Xor128(const AESKey& a, const AESKey& b) {
    AESKey out;
    std::transform(a.cbegin(), a.cend(), b.cbegin(), out.begin(), std::bit_xor<>());
    return out;
}

} // namespace HW::AES
