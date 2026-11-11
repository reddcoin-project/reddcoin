// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WARNINGS_H
#define BITCOIN_WARNINGS_H

#include <util/translation.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

/** Where the upgrade warnings send users for a new release. */
extern const char* const UPGRADE_URL;

void SetMiscWarning(const bilingual_str& warning);
/** Kinds of warning that call for the user to act, for code that needs to
 *  know which are up rather than show their text (the GUI's status bar). */
enum class WarningKind {
    UNKNOWN_RULE_SIGNALLING, //!< An unknown versionbit is set in most recent blocks.
    UNKNOWN_RULE_LOCKED_IN,  //!< An unknown rule is locked in.
    UNKNOWN_RULE_ACTIVE,     //!< An unknown rule is in effect.
    UNKNOWN_BLOCK_VERSION,   //!< Most recent blocks use a block version format this release doesn't recognise.
    INVALID_CHAIN,           //!< A chain with more work than ours contains a block that is invalid to us.
    HEAVIER_CHAIN,           //!< A chain with more work than ours isn't being followed: its blocks are missing or couldn't be verified.
};

struct UpgradeWarning {
    WarningKind kind;
    bilingual_str text;
};

/** Replace the current upgrade warnings (unknown rule changes, unknown block
 * versions). They are recomputed for every new tip, so each one goes away
 * once its cause does. */
void SetUpgradeWarnings(std::vector<UpgradeWarning> warnings);

/** Replace the current fork warnings (a chain with more work than ours that
 * this node doesn't follow). They are recomputed as blocks and headers arrive. */
void SetForkWarnings(std::vector<UpgradeWarning> warnings);

/** The kinds of warning that are up now. */
std::set<WarningKind> GetWarningKinds();

/** The upgrade warning texts. Each says what is happening, when, and what to do. */
bilingual_str UnknownRuleSignallingWarning(int bit, int percent, int blocks);
bilingual_str UnknownRuleLockedInWarning(int bit, int activation_height, int64_t activation_time);
bilingual_str UnknownRuleActiveWarning(int bit, int since_height);
bilingual_str UnknownBlockVersionWarning(int count, int blocks);

/** The fork warning texts. Each gives the fork height, both tips' heights,
 * why the other chain isn't followed, and what to do. */
bilingual_str InvalidChainWarning(int fork_height, int their_height, int our_height);
bilingual_str HeavierChainWarning(int fork_height, int their_height, int our_height, bool blocks_received);

/** Format a string that describes several potential problems detected by the core.
 * @param[in] verbose bool
 * - if true, get all warnings separated by <hr />
 * - if false, get the most important warning
 * @returns the warning string
 */
bilingual_str GetWarnings(bool verbose);

extern std::string strMintWarning;

#endif //  BITCOIN_WARNINGS_H
