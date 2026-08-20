// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CLIENTVERSION_H
#define BITCOIN_CLIENTVERSION_H

#include <util/macros.h>

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif //HAVE_CONFIG_H

// Check that required client information is defined
#if !defined(CLIENT_VERSION_MAJOR) || !defined(CLIENT_VERSION_MINOR) || !defined(CLIENT_VERSION_BUILD) || !defined(CLIENT_VERSION_REVISION) || !defined(CLIENT_VERSION_IS_RELEASE) || !defined(COPYRIGHT_YEAR)
#error Client version information missing: version is not defined by bitcoin-config.h or in any other way
#endif

//! Copyright string used in Windows .rc files
#define COPYRIGHT_STR "2009-" STRINGIZE(COPYRIGHT_YEAR) " " COPYRIGHT_HOLDERS_FINAL

/**
 * bitcoind-res.rc includes this file, but it cannot cope with real c++ code.
 * WINDRES_PREPROC is defined to indicate that its pre-processor is running.
 * Anything other than a define should be guarded below.
 */

#if !defined(WINDRES_PREPROC)

#include <string>
#include <vector>

/**
 * The client version as a single integer, e.g. 4.22.9.4 -> 4220904.
 *
 * Components are MAJOR.MINOR.REVISION.BUILD, the order Bitcoin Core used
 * historically. The weights leave two decimal digits each, matching the decode
 * in FormatVersion() and in test/functional/test_framework/test_framework.py.
 *
 * Every value is greater than the previous three-component encoding could
 * produce (10000*MAJOR + 100*MINOR + BUILD, always below 100000 for a
 * single-digit major), which matters because this integer drives upgrade
 * decisions rather than only display: see wallet/walletdb.cpp (rewrite the
 * stored version when last_client is older) and qt/optionsmodel.cpp (QSettings
 * migration). Any future reweighting must preserve that ordering.
 */
static const int CLIENT_VERSION =
                           1000000 * CLIENT_VERSION_MAJOR
                         +   10000 * CLIENT_VERSION_MINOR
                         +     100 * CLIENT_VERSION_REVISION
                         +       1 * CLIENT_VERSION_BUILD;

extern const std::string CLIENT_NAME;
extern const std::string CLIENT_BUILD;


std::string FormatFullVersion();
std::string FormatSubVersion(const std::string& name, int nClientVersion, const std::vector<std::string>& comments);

#endif // WINDRES_PREPROC

#endif // BITCOIN_CLIENTVERSION_H
