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
 * A chunked body is reassembled, and is complete only if its terminating zero
 * length chunk arrived. api.github.com answers with Content-Length most of the
 * time and switches to chunked intermittently, so both have to work.
 *
 * @param[in] raw_response The status line, headers and body as received.
 * @param[in] clean_eof    Whether the read ended with a clean end of file
 *                         rather than a truncated stream.
 * @return The response body.
 * @throws std::runtime_error if the response is malformed, reports a status
 *         other than 200, uses a transfer encoding other than chunked, or
 *         cannot be shown to have arrived in full.
 *
 * Exposed for testing.
 */
std::string ExtractHttpBody(const std::string& raw_response, bool clean_eof);

/**
 * A parsed HTTP response.
 *
 * body is only checked for completeness when status is 200. A redirect's body
 * is discarded, so rejecting the exchange because that body was framed loosely
 * would fail over something nothing reads.
 */
struct HttpResponse {
    unsigned int status{0};
    //! Location header, empty when absent.
    std::string location;
    std::string body;
};

/**
 * Parse a raw HTTP response into its status, Location and body.
 *
 * @param[in] raw_response The status line, headers and body as received.
 * @param[in] clean_eof    Whether the read ended with a clean end of file
 *                         rather than a truncated stream.
 * @throws std::runtime_error if the response is malformed, uses a transfer
 *         encoding other than chunked, or carries a 200 body that cannot be
 *         shown to have arrived in full.
 *
 * Exposed for testing.
 */
HttpResponse ParseHttpResponse(const std::string& raw_response, bool clean_eof);

//! A host and the path to ask it for.
struct Url {
    std::string host;
    std::string target;
};

/**
 * Where a Location header points, given the request that produced it.
 *
 * Handles absolute, protocol relative, absolute path and relative forms.
 *
 * @throws std::runtime_error if the redirect leaves https, names no host, or
 *         specifies a port. Following a redirect out of https would discard the
 *         certificate verification this client performs, and a redirect is
 *         where an attacker would try to introduce that.
 *
 * Exposed for testing.
 */
Url ResolveRedirect(const std::string& base_host, const std::string& base_target,
                    const std::string& location);

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
