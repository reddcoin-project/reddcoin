// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <stdint.h>

// needed when linking transaction.cpp, since we are not going to pull real GetAdjustedTime from timedata.cpp
int64_t GetAdjustedTime()
{
    return 0;
}
