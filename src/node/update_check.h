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
 * Split an HTTP/1.x response into its body, rejecting anything that cannot be
 * shown to be complete.
 *
 * Completeness has to be checked explicitly. The response is read to the end of
 * the stream, and at that layer a connection that died mid-body looks exactly
 * like one that finished, so a truncated body would otherwise be returned as a
 * successful result. For the update check that surfaces as an upgrade notice
 * that silently never appears; for anything that later downloads a file it
 * would mean a partial download reported as a complete one.
 *
 * A body is accepted when its length matches Content-Length, or, when the
 * response carries no Content-Length and is therefore delimited by the
 * connection closing, when the stream ended with a clean TLS shutdown.
 *
 * @param[in] raw_response The status line, headers and body as received.
 * @param[in] clean_eof    Whether the read ended with a clean end of file
 *                         rather than a truncated stream.
 * @return The response body.
 * @throws std::runtime_error if the response is malformed, reports a status
 *         other than 200, uses a transfer encoding this does not decode, or
 *         cannot be shown to have arrived in full.
 *
 * Exposed for testing.
 */
std::string ExtractHttpBody(const std::string& raw_response, bool clean_eof);

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
