// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_UPDATE_CHECK_H
#define BITCOIN_NODE_UPDATE_CHECK_H

#include <fs.h>

#include <cstddef>
#include <cstdint>
#include <functional>
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

//! What the status line and headers say, before any body has been read.
struct HttpHeaders {
    unsigned int status{0};
    //! Location header, empty when absent.
    std::string location;
    //! Content-Length, or -1 when the server gave none.
    int64_t content_length{-1};
    bool chunked{false};
};

/**
 * Parse a status line and header block.
 *
 * Separate from ParseHttpResponse so a streaming read can decide what to do
 * with a body before any of it has arrived.
 *
 * @param[in] headers The status line and headers, without the blank line that
 *                    terminates them.
 * @throws std::runtime_error if the status line is malformed, the transfer
 *         encoding is not chunked, or Content-Length is unusable.
 *
 * Exposed for testing.
 */
HttpHeaders ParseHttpHeaders(const std::string& headers);

/**
 * Decodes a chunked body as it arrives, rather than all at once.
 *
 * An artifact that arrives chunked cannot be reassembled in memory first
 * without defeating the point of streaming it, so the framing has to come off
 * incrementally, across reads that land anywhere in the stream.
 *
 * Exposed for testing, which is where the boundary cases live: a chunk size
 * split across two reads, a chunk body split across several, and a terminating
 * chunk arriving on its own.
 */
class ChunkedDecoder
{
public:
    //! Called with each run of decoded body bytes. Returning false stops the
    //! decode, which is how a cancelled or failed write aborts it.
    using Sink = std::function<bool(const char* data, std::size_t size)>;

    /**
     * Feed raw bytes from the stream.
     *
     * @return false if the framing is malformed or the sink asked to stop.
     */
    bool Feed(const char* data, std::size_t size, const Sink& sink);

    //! Whether the terminating zero length chunk has been seen. A body that
    //! ends without it is incomplete, however many bytes arrived.
    bool Complete() const { return m_state == State::Done; }

private:
    enum class State { Size, Data, DataTerminator, Done };
    State m_state{State::Size};
    //! Partial size line carried between feeds.
    std::string m_pending;
    //! Bytes still to come in the chunk being read.
    uint64_t m_remaining{0};
    //! Bytes of the chunk's trailing CRLF still to consume.
    int m_terminator{0};
};

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

//! Host the release artifacts and their manifest are published on, as
//! distinct from the one that announces the version.
extern const std::string RELEASE_DOWNLOAD_HOST;

/**
 * Fetch a small file into memory over HTTPS.
 *
 * For the manifest and its signature, which are kilobytes and have to be held
 * whole to be verified. An artifact goes through DownloadToFile instead.
 *
 * @param[out] out   The body, when this returns true.
 * @param[out] error Why it failed.
 */
bool FetchToString(const std::string& host, const std::string& target, std::string& out,
                   std::string& error);

//! What a probe found out about a file on the server.
struct RemoteFile {
    //! Content-Length the server reports, or -1 when it gives none.
    int64_t size{-1};
};

/**
 * Answer to "is this file actually there?".
 *
 * Absent and Unknown are kept apart deliberately. A 404 is the server saying
 * the file is not there; a timeout or a 500 is the client failing to find out.
 * Collapsing the two would let a broken network be reported to the user as a
 * missing release, which is a different problem with a different remedy.
 */
enum class ProbeResult {
    Present,  //!< the server answered 200
    Absent,   //!< the server answered 404
    Unknown,  //!< could not be established
};

/**
 * Ask whether a file exists, without downloading it.
 *
 * Issues a HEAD request, following redirects on the same terms as everything
 * else here. Used to avoid naming an artifact that would 404, and to learn its
 * size before offering to fetch it.
 *
 * @param[out] out   Filled in when the result is Present.
 * @param[out] error Why the result is Unknown. Empty otherwise.
 *
 * A server is not obliged to answer HEAD the way it answers GET, so a Present
 * result is good evidence rather than a guarantee. It is the difference between
 * offering a link that is probably right and one that is probably wrong.
 */
ProbeResult ProbeRemoteFile(const std::string& host, const std::string& target,
                            RemoteFile& out, std::string& error);

//! Called as a download proceeds. total is -1 when the server gave no length.
using DownloadProgress = std::function<void(int64_t received, int64_t total)>;

//! Called between reads. Returning true abandons the download.
using DownloadCancel = std::function<bool()>;

/**
 * Download a file over HTTPS, streaming it to disk.
 *
 * The body is never held in memory, so this is what an artifact goes through
 * rather than the update check's in-memory fetch. Redirects are followed on the
 * same terms as elsewhere: a fresh certificate check per hop, and a refusal to
 * leave https.
 *
 * Written to `destination` with a `.part` suffix and renamed only once the body
 * is complete, so an interrupted download never leaves a file that looks
 * finished. On any failure the partial file is removed.
 *
 * @param[in]  host        Host to fetch from.
 * @param[in]  target      Path on that host.
 * @param[in]  destination Where the finished file goes.
 * @param[in]  progress    Optional, may be empty.
 * @param[in]  cancel      Optional, may be empty.
 * @param[out] error       Why it failed, when it returns false. Empty when the
 *                         caller cancelled, since that is not an error.
 * @return true only when the file is complete and in place.
 *
 * Does not verify what was downloaded. That is the caller's job, and until it
 * happens the file is untrusted bytes from the network.
 */
bool DownloadToFile(const std::string& host, const std::string& target,
                    const fs::path& destination, const DownloadProgress& progress,
                    const DownloadCancel& cancel, std::string& error);

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
