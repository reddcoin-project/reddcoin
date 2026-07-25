// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_UPDATE_CHECK_H
#define BITCOIN_NODE_UPDATE_CHECK_H

class UniValue;

namespace node {
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
