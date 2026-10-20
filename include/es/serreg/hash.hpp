// es/serreg/hash.hpp — the fingerprint of the whole library (spec FR-H5: hash algorithm id 2 = XXH3-64, format 1.3)
// and the small constexpr string helpers used to build canonical descriptions (§6.2). Everything here is usable
// in constant evaluation; `fingerprint64` also runs at load time (dictionary checks, the program hash).
//
//   static_assert(es::serreg::fingerprint64("string") == 0xd8ad194648e774f1ull);
//
// XXH3-64 with seed 0 and the default secret. The algorithm and the secret are from xxHash by Yann Collet,
// https://github.com/Cyan4973/xxHash (BSD 2-Clause License, Copyright (c) 2012-2021 Yann Collet); this is an
// independent scalar implementation (no code or header is vendored). One function serves both worlds:
// `if consteval` assembles words from bytes (memcpy is not a constant expression), the run-time branch loads 8
// bytes with memcpy. tests/unit/test_hash.cpp pins known answers from the xxHash 0.8.3 reference in both modes.
#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace es::serreg
{

namespace detail::xxh3
{
static_assert(std::endian::native == std::endian::little,
              "the run-time loads assume a little-endian host");

using u8 = unsigned char;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline constexpr u8 secret[192] = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
    0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
    0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
    0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
    0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
    0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
    0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e};

inline constexpr u64 p32_1 = 0x9E3779B1U, p32_2 = 0x85EBCA77U, p32_3 = 0xC2B2AE3DU;
inline constexpr u64 p64_1 = 0x9E3779B185EBCA87ULL, p64_2 = 0xC2B2AE3D27D4EB4FULL,
                     p64_3 = 0x165667B19E3779F9ULL, p64_4 = 0x85EBCA77C2B2AE63ULL,
                     p64_5 = 0x27D4EB2F165667C5ULL;
inline constexpr u64 mx1 = 0x165667919E3779F9ULL, mx2 = 0x9FB21C651E98DF25ULL;

template<class C> constexpr u64 rd64(const C* const p) noexcept
{
    if consteval
    {
        u64 v = 0;
        for (int i = 7; i >= 0; --i)
            v = v << 8 | static_cast<u8>(p[i]);
        return v;
    }
    else
    {
        u64 v;
        __builtin_memcpy(&v, p, 8);
        return v;
    }
}
template<class C> constexpr u32 rd32(const C* const p) noexcept
{
    if consteval
    {
        u32 v = 0;
        for (int i = 3; i >= 0; --i)
            v = v << 8 | static_cast<u8>(p[i]);
        return v;
    }
    else
    {
        u32 v;
        __builtin_memcpy(&v, p, 4);
        return v;
    }
}
#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 u128; // __extension__: -Wpedantic accepts the GNU type here
#endif
constexpr u64 fold(const u64 a, const u64 b) noexcept // low ^ high of the 128-bit product
{
#if defined(__SIZEOF_INT128__)
    const u128 m = static_cast<u128>(a) * b;
    return static_cast<u64>(m) ^ static_cast<u64>(m >> 64);
#else
    const u64 al = a & 0xFFFFFFFFu, ah = a >> 32, bl = b & 0xFFFFFFFFu, bh = b >> 32;
    const u64 ll = al * bl, hl = ah * bl, lh = al * bh, hh = ah * bh;
    const u64 cross = (ll >> 32) + (hl & 0xFFFFFFFFu) + lh;
    return ((cross << 32) | (ll & 0xFFFFFFFFu)) ^ ((hl >> 32) + (cross >> 32) + hh);
#endif
}
constexpr u64 avalanche64(u64 h) noexcept
{
    h ^= h >> 33;
    h *= p64_2;
    h ^= h >> 29;
    h *= p64_3;
    return h ^ (h >> 32);
}
constexpr u64 avalanche(u64 h) noexcept
{
    h ^= h >> 37;
    h *= mx1;
    return h ^ (h >> 32);
}
constexpr u64 rrmxmx(u64 h, const u64 len) noexcept
{
    h ^= std::rotl(h, 49) ^ std::rotl(h, 24);
    h *= mx2;
    h ^= (h >> 35) + len;
    h *= mx2;
    return h ^ (h >> 28);
}
template<class C> constexpr u64 mix16(const C* const in, const u8* const s) noexcept
{
    return fold(rd64(in) ^ rd64(s), rd64(in + 8) ^ rd64(s + 8));
}

template<class C> constexpr u64 hash(const C* const in, const std::size_t len) noexcept
{
    const u8* const s = secret;
    if (len <= 16)
    {
        if (len > 8)
        {
            const u64 lo = rd64(in) ^ (rd64(s + 24) ^ rd64(s + 32));
            const u64 hi = rd64(in + len - 8) ^ (rd64(s + 40) ^ rd64(s + 48));
            return avalanche(len + std::byteswap(lo) + hi + fold(lo, hi));
        }
        if (len >= 4)
        {
            const u64 in64 = rd32(in + len - 4) + (static_cast<u64>(rd32(in)) << 32);
            return rrmxmx(in64 ^ (rd64(s + 8) ^ rd64(s + 16)), len);
        }
        if (len > 0)
        {
            const u32 c = (u32{static_cast<u8>(in[0])} << 16) |
                          (u32{static_cast<u8>(in[len >> 1])} << 24) |
                          u32{static_cast<u8>(in[len - 1])} | (static_cast<u32>(len) << 8);
            return avalanche64(u64{c} ^ u64{rd32(s) ^ rd32(s + 4)});
        }
        return avalanche64(rd64(s + 56) ^ rd64(s + 64));
    }
    if (len <= 128)
    {
        u64 acc = len * p64_1;
        if (len > 32)
        {
            if (len > 64)
            {
                if (len > 96)
                    acc += mix16(in + 48, s + 96) + mix16(in + len - 64, s + 112);
                acc += mix16(in + 32, s + 64) + mix16(in + len - 48, s + 80);
            }
            acc += mix16(in + 16, s + 32) + mix16(in + len - 32, s + 48);
        }
        acc += mix16(in, s) + mix16(in + len - 16, s + 16);
        return avalanche(acc);
    }
    if (len <= 240)
    {
        u64 acc = len * p64_1;
        for (std::size_t i = 0; i < 8; ++i)
            acc += mix16(in + 16 * i, s + 16 * i);
        u64 end = mix16(in + len - 16, s + 136 - 17);
        acc = avalanche(acc);
        for (std::size_t i = 8; i < len / 16; ++i)
            end += mix16(in + 16 * i, s + 16 * (i - 8) + 3);
        return avalanche(acc + end);
    }
    u64 acc[8] = {p32_3, p64_1, p64_2, p64_3, p64_4, p32_2, p64_5, p32_1};
    const auto stripe = [&acc](const C* const p, const u8* const k) noexcept {
        for (std::size_t l = 0; l < 8; ++l)
        {
            const u64 d = rd64(p + l * 8), key = d ^ rd64(k + l * 8);
            acc[l ^ 1] += d;
            acc[l] += (key & 0xFFFFFFFFu) * (key >> 32);
        }
    };
    constexpr std::size_t per_block = (sizeof secret - 64) / 8, block = 64 * per_block;
    const std::size_t blocks = (len - 1) / block;
    for (std::size_t n = 0; n < blocks; ++n)
    {
        for (std::size_t k = 0; k < per_block; ++k)
            stripe(in + n * block + k * 64, s + k * 8);
        for (std::size_t l = 0; l < 8; ++l)
            acc[l] = (acc[l] ^ (acc[l] >> 47) ^ rd64(s + sizeof secret - 64 + l * 8)) * p32_1;
    }
    const std::size_t stripes = ((len - 1) - block * blocks) / 64;
    for (std::size_t k = 0; k < stripes; ++k)
        stripe(in + blocks * block + k * 64, s + k * 8);
    stripe(in + len - 64, s + sizeof secret - 64 - 7);
    u64 r = len * p64_1;
    for (std::size_t i = 0; i < 4; ++i)
        r += fold(acc[2 * i] ^ rd64(s + 11 + 16 * i), acc[2 * i + 1] ^ rd64(s + 11 + 16 * i + 8));
    return avalanche(r);
}
} // namespace detail::xxh3

// Every 64-bit identity: schema/layout hashes, call hash, literal id, the scalar/"string" ARG hashes.
constexpr std::uint64_t fingerprint64(const std::string_view s) noexcept
{
    return detail::xxh3::hash(s.data(), s.size());
}
// The same over raw bytes (blob hashes in the golden tests).
constexpr std::uint64_t fingerprint64(const std::span<const std::byte> s) noexcept
{
    return detail::xxh3::hash(s.data(), s.size());
}
// Program hash (FR-H3): XXH3-64 of the sorted u64 values as little-endian bytes. Callers pass a *sorted* range,
// which makes the result independent of registration order.
constexpr std::uint64_t fingerprint64_u64s(const std::span<const std::uint64_t> sorted) noexcept
{
    if consteval
    {
        std::vector<unsigned char> b;
        b.reserve(8 * sorted.size());
        for (const std::uint64_t v : sorted)
            for (std::size_t k = 0; k < 8; ++k)
                b.push_back(static_cast<unsigned char>(v >> (8 * k)));
        return detail::xxh3::hash(b.data(), b.size());
    }
    else
    {
        return detail::xxh3::hash(reinterpret_cast<const unsigned char*>(sorted.data()),
                                  8 * sorted.size());
    }
}

// ---- CRC32C (Castagnoli, polynomial 0x1EDC6F41 reflected = 0x82F63B78) — format 1.7 frame trailers --------------
// Not an identity: an integrity check of a frame's bytes (frame flag crc). The software table is constexpr (the
// golden tests freeze known answers); at run time x86-64 hosts with SSE4.2 use the crc32 instruction, chosen once
// per process by __builtin_cpu_supports. Both produce the standard CRC32C (as in iSCSI, RFC 3720 §B.4):
// crc32c("123456789") == 0xE3069283.
namespace detail::crc32c_impl
{
constexpr std::array<std::uint32_t, 256> make_table() noexcept
{
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i)
    {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? 0x82F63B78u ^ (c >> 1) : c >> 1;
        t[i] = c;
    }
    return t;
}
inline constexpr std::array<std::uint32_t, 256> table = make_table();
constexpr std::uint32_t soft(std::uint32_t crc, const unsigned char* p, std::size_t n) noexcept
{
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
[[gnu::target("sse4.2")]] inline std::uint32_t hw(std::uint32_t crc, const unsigned char* p,
                                                  std::size_t n) noexcept
{
    crc = ~crc;
    while (n >= 8)
    {
        std::uint64_t v;
        __builtin_memcpy(&v, p, 8);
        crc = static_cast<std::uint32_t>(__builtin_ia32_crc32di(crc, v));
        p += 8;
        n -= 8;
    }
    while (n--)
        crc = __builtin_ia32_crc32qi(crc, *p++);
    return ~crc;
}
inline bool have_hw() noexcept
{
    static const bool yes = __builtin_cpu_supports("sse4.2");
    return yes;
}
#else
inline bool have_hw() noexcept
{
    return false;
}
inline std::uint32_t hw(const std::uint32_t crc, const unsigned char* const p,
                        const std::size_t n) noexcept
{
    return soft(crc, p, n);
}
#endif
} // namespace detail::crc32c_impl

// CRC32C of `s`, continuing from `seed` (0 for a fresh checksum). Constant-evaluable (software path); the hardware
// instruction at run time where present.
constexpr std::uint32_t crc32c(const std::span<const std::byte> s,
                               const std::uint32_t seed = 0) noexcept
{
    if consteval
    {
        std::vector<unsigned char> b(s.size());
        for (std::size_t i = 0; i < s.size(); ++i)
            b[i] = static_cast<unsigned char>(s[i]);
        return detail::crc32c_impl::soft(seed, b.data(), b.size());
    }
    else
    {
        const auto* const p = reinterpret_cast<const unsigned char*>(s.data());
        return detail::crc32c_impl::have_hw() ? detail::crc32c_impl::hw(seed, p, s.size())
                                              : detail::crc32c_impl::soft(seed, p, s.size());
    }
}
constexpr std::uint32_t crc32c(const std::string_view s, const std::uint32_t seed = 0) noexcept
{
    if consteval
    {
        std::vector<unsigned char> b(s.size());
        for (std::size_t i = 0; i < s.size(); ++i)
            b[i] = static_cast<unsigned char>(s[i]);
        return detail::crc32c_impl::soft(seed, b.data(), b.size());
    }
    else
    {
        return crc32c(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()),
            seed);
    }
}

namespace detail
{
constexpr void append_hex(std::string& out, const std::uint64_t v)
{
    constexpr char digits[] = "0123456789abcdef";
    out += "0x";
    for (int i = 15; i >= 0; --i)
        out += digits[(v >> (4 * i)) & 0xF];
}
constexpr void append_uint(std::string& out, std::uint64_t v)
{
    char buf[20];
    std::size_t n = 0;
    do
    {
        buf[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        out += buf[--n];
}
constexpr void append_int(std::string& out, const std::int64_t v)
{
    if (v < 0)
    {
        out += '-';
        append_uint(out, static_cast<std::uint64_t>(-(v + 1)) + 1);
    }
    else
        append_uint(out, static_cast<std::uint64_t>(v));
}
} // namespace detail

// Structural compile-time string (NTTP), used by literals (§6.6) and annotations.
template<std::size_t N> struct fixed_string
{
    char data[N]{};
    constexpr fixed_string(const char (&s)[N])
    {
        for (std::size_t i = 0; i < N; ++i)
            data[i] = s[i];
    }
    constexpr std::string_view view() const noexcept
    {
        return {data, N - 1};
    }
    static constexpr std::size_t size() noexcept
    {
        return N - 1;
    }
};

} // namespace es::serreg
