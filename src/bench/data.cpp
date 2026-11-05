// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/data.h>

namespace benchmark {
namespace data {

#include <bench/data/block4953811.raw.h>
const std::vector<uint8_t> block4953811{std::begin(block4953811_raw), std::end(block4953811_raw)};

} // namespace data
} // namespace benchmark
