// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_UTIL_INT128_H
#define DIGIBYTE_UTIL_INT128_H

#if defined(__SIZEOF_INT128__)
namespace util {
using int128_t = __int128;
using uint128_t = unsigned __int128;
} // namespace util
#elif defined(_MSC_VER)
#include <boost/multiprecision/cpp_int.hpp>
namespace util {
using int128_t = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
    127, 127, boost::multiprecision::signed_magnitude,
    boost::multiprecision::unchecked, void>>;
using uint128_t = boost::multiprecision::uint128_t;
} // namespace util
#else
#error "DigiByte requires a fixed-width 128-bit integer implementation"
#endif

#endif // DIGIBYTE_UTIL_INT128_H
