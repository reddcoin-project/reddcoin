// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WARNINGS_H
#define BITCOIN_WARNINGS_H

#include <cstdint>
#include <string>
#include <vector>

struct bilingual_str;

/** Where the upgrade warnings send users for a new release. */
extern const char* const UPGRADE_URL;

void SetMiscWarning(const bilingual_str& warning);
void SetfLargeWorkInvalidChainFound(bool flag);
/** Replace the current upgrade warnings (unknown rule changes, unknown block
 * versions). They are recomputed for every new tip, so each one goes away
 * once its cause does. */
void SetUpgradeWarnings(std::vector<bilingual_str> warnings);

/** The upgrade warning texts. Each says what is happening, when, and what to do. */
bilingual_str UnknownRuleSignallingWarning(int bit, int percent, int blocks);
bilingual_str UnknownRuleLockedInWarning(int bit, int activation_height, int64_t activation_time);
bilingual_str UnknownRuleActiveWarning(int bit, int since_height);
bilingual_str UnknownBlockVersionWarning(int count, int blocks);

/** Format a string that describes several potential problems detected by the core.
 * @param[in] verbose bool
 * - if true, get all warnings separated by <hr />
 * - if false, get the most important warning
 * @returns the warning string
 */
bilingual_str GetWarnings(bool verbose);

extern std::string strMintWarning;

#endif //  BITCOIN_WARNINGS_H
