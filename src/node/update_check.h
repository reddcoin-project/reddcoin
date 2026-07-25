// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_UPDATE_CHECK_H
#define BITCOIN_NODE_UPDATE_CHECK_H

#include <string>

class UniValue;

namespace node {
/**
 * Version of the TLS library this build is running against, for display and
 * logging, for example "OpenSSL 3.5.7 30 Sep 2025".
 *
 * It lives here because the update check is the only thing in the tree that
 * uses TLS, and so the only reason the library is linked at all. Reported at
 * run time rather than from the headers, so a build against a shared library
 * names what is actually loaded.
 */
std::string SslVersion();

/**
 * Ask the release server which version is current and report how it compares
 * with this build.
 *
 * Accumulates into result: localversion, remoteversion, updateavailable,
 * message, warning, officialDownloadLink and errors. Network and parsing
 * failures are reported through the errors field rather than by throwing.
 *
 * The request is performed synchronously and has no timeout, so this can block
 * for as long as the operating system takes to give up on the connection. Do
 * not call it from a thread that must stay responsive.
 */
void CheckForUpdates(UniValue& result);
} // namespace node

#endif // BITCOIN_NODE_UPDATE_CHECK_H
