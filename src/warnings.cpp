// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <warnings.h>

#include <sync.h>
#include <tinyformat.h>
#include <util/string.h>
#include <util/system.h>
#include <util/time.h>
#include <util/translation.h>

#include <utility>
#include <vector>

const char* const UPGRADE_URL = "https://download.reddcoin.com/";

std::string strMintWarning;

static Mutex g_warnings_mutex;
static bilingual_str g_misc_warnings GUARDED_BY(g_warnings_mutex);
static bool fLargeWorkInvalidChainFound GUARDED_BY(g_warnings_mutex) = false;
static std::vector<UpgradeWarning> g_upgrade_warnings GUARDED_BY(g_warnings_mutex);

void SetMiscWarning(const bilingual_str& warning)
{
    LOCK(g_warnings_mutex);
    g_misc_warnings = warning;
}

void SetfLargeWorkInvalidChainFound(bool flag)
{
    LOCK(g_warnings_mutex);
    fLargeWorkInvalidChainFound = flag;
}

void SetUpgradeWarnings(std::vector<UpgradeWarning> warnings)
{
    LOCK(g_warnings_mutex);
    g_upgrade_warnings = std::move(warnings);
}

std::set<WarningKind> GetWarningKinds()
{
    LOCK(g_warnings_mutex);
    std::set<WarningKind> kinds;
    if (fLargeWorkInvalidChainFound) kinds.insert(WarningKind::INVALID_CHAIN);
    for (const UpgradeWarning& warning : g_upgrade_warnings) kinds.insert(warning.kind);
    return kinds;
}

bilingual_str UnknownRuleSignallingWarning(int bit, int percent, int blocks)
{
    return strprintf(_("A network rule change that this version does not know about is being signalled (versionbit %i, %i%% of the last %i blocks). Upgrade Reddcoin Core: %s"),
                     bit, percent, blocks, UPGRADE_URL);
}

bilingual_str UnknownRuleLockedInWarning(int bit, int activation_height, int64_t activation_time)
{
    return strprintf(_("A network rule change that this version does not know about (versionbit %i) is locked in and takes effect at block %i (about %s). Upgrade Reddcoin Core before then: %s"),
                     bit, activation_height, FormatISO8601Date(activation_time), UPGRADE_URL);
}

bilingual_str UnknownRuleActiveWarning(int bit, int since_height)
{
    return strprintf(_("A network rule change that this version does not know about (versionbit %i) is in effect since block %i. This version may not follow the network. Upgrade Reddcoin Core now: %s"),
                     bit, since_height, UPGRADE_URL);
}

bilingual_str UnknownBlockVersionWarning(int count, int blocks)
{
    return strprintf(_("%i of the last %i blocks use a block version that this version does not recognise. The network may be changing its rules. Upgrade Reddcoin Core: %s"),
                     count, blocks, UPGRADE_URL);
}

bilingual_str GetWarnings(bool verbose)
{
    bilingual_str warnings_concise;
    std::vector<bilingual_str> warnings_verbose;

    LOCK(g_warnings_mutex);

    // Pre-release build warning
    if (!CLIENT_VERSION_IS_RELEASE) {
        warnings_concise = _("This is a pre-release test build - use at your own risk - do not use for mining, staking or merchant applications");
        warnings_verbose.emplace_back(warnings_concise);
    }

    // reddcoin: wallet warnings for staking
    if (strMintWarning != "")
    {
        warnings_concise = Untranslated(strMintWarning);
        warnings_verbose.emplace_back(warnings_concise);
    }

    // Misc warnings like out of disk space and clock is wrong
    if (!g_misc_warnings.empty()) {
        warnings_concise = g_misc_warnings;
        warnings_verbose.emplace_back(warnings_concise);
    }

    if (fLargeWorkInvalidChainFound) {
        warnings_concise = _("Warning: We do not appear to fully agree with our peers! You may need to upgrade, or other nodes may need to upgrade.");
        warnings_verbose.emplace_back(warnings_concise);
    }

    // Upgrade warnings come last, so the concise form shows one of them: they
    // are the ones that need the user to act.
    for (const UpgradeWarning& warning : g_upgrade_warnings) {
        warnings_concise = warning.text;
        warnings_verbose.emplace_back(warning.text);
    }

    if (verbose) {
        return Join(warnings_verbose, Untranslated("<hr />"));
    }

    return warnings_concise;
}
