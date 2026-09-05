// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/update_check.h>

#include <clientversion.h>
#include <node/ca_store.h>
#include <node/release_artifacts.h>
#include <rpc/semver.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/system.h>

#include <univalue.h>

// boost 1.71 predates OpenSSL 3.0 and its ssl wrapper still calls functions the
// 3.x series deprecated, such as RSA_free and SSL_CTX_use_RSAPrivateKey. The
// warnings come from boost rather than from anything here, and depends headers
// are reached with -I rather than -isystem, so they would otherwise break any
// build using -Werror.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

static std::string strDownloadLink = "https://download.reddcoin.com/bin/reddcoin-core-";
static std::string strGithubLink = "/repos/reddcoin-project/reddcoin/releases/latest";

namespace {
//! Value of one header, matched case insensitively as RFC 7230 requires, or an
//! empty string when the header is absent.
//!
//! headers is the status line plus the header block, without the blank line
//! that terminates it. Matching walks line by line rather than searching the
//! block for the name, so a name appearing inside some other header's value
//! cannot be mistaken for the header itself.
std::string GetHeader(const std::string& headers, const std::string& lower_name)
{
    std::string::size_type pos{headers.find("\r\n")};
    while (pos != std::string::npos) {
        const std::string::size_type start{pos + 2};
        std::string::size_type end{headers.find("\r\n", start)};
        const bool last_line{end == std::string::npos};
        if (last_line) end = headers.size();

        const std::string line{headers.substr(start, end - start)};
        const std::string::size_type colon{line.find(':')};
        if (colon != std::string::npos && ToLower(line.substr(0, colon)) == lower_name) {
            return TrimString(line.substr(colon + 1));
        }

        if (last_line) break;
        pos = end;
    }
    return "";
}

//! Parse a hexadecimal chunk size. Rejects an empty field, a non-hex
//! character, and anything wider than 64 bits.
bool ParseChunkSize(const std::string& str, uint64_t& out)
{
    if (str.empty() || str.size() > 16) return false;
    uint64_t value{0};
    for (const char c : str) {
        const signed char digit{HexDigit(c)};
        if (digit < 0) return false;
        value = (value << 4) | static_cast<uint64_t>(digit);
    }
    out = value;
    return true;
}

//! Reassemble a chunked body.
//!
//! Completeness is inherent here rather than checked afterwards: the loop only
//! returns on the terminating zero length chunk, so a body that stops early
//! throws instead of yielding what arrived. Chunk extensions and trailers are
//! ignored, neither being used by anything this talks to.
std::string DecodeChunkedBody(const std::string& body)
{
    std::string decoded;
    std::string::size_type pos{0};

    while (true) {
        const std::string::size_type eol{body.find("\r\n", pos)};
        if (eol == std::string::npos) {
            throw std::runtime_error("Chunked response ended before its final chunk");
        }

        std::string size_field{body.substr(pos, eol - pos)};
        const std::string::size_type extension{size_field.find(';')};
        if (extension != std::string::npos) size_field = size_field.substr(0, extension);

        uint64_t size{0};
        if (!ParseChunkSize(TrimString(size_field), size)) {
            throw std::runtime_error("Chunked response has an unusable chunk size");
        }
        pos = eol + 2;

        // The zero length chunk ends the body. Anything after it is trailers.
        if (size == 0) return decoded;

        // Each chunk is followed by its own CRLF, so both it and the data have
        // to be present. Compared this way round so neither side can overflow.
        const std::string::size_type available{body.size() - pos};
        if (size > available || available - size < 2) {
            throw std::runtime_error("Chunked response ended mid-chunk");
        }

        decoded.append(body, pos, static_cast<std::string::size_type>(size));
        pos += static_cast<std::string::size_type>(size);
        if (body.compare(pos, 2, "\r\n") != 0) {
            throw std::runtime_error("Chunked response has a malformed chunk terminator");
        }
        pos += 2;
    }
}

} // namespace

bool node::ChunkedDecoder::Feed(const char* data, std::size_t size, const Sink& sink)
{
    std::size_t pos{0};
    while (pos < size) {
        switch (m_state) {
        case State::Size: {
            // The size line can land across any number of feeds, so it is
            // accumulated rather than assumed to arrive whole.
            const char c{data[pos++]};
            m_pending.push_back(c);
            if (m_pending.size() >= 2 && m_pending.compare(m_pending.size() - 2, 2, "\r\n") == 0) {
                std::string field{m_pending.substr(0, m_pending.size() - 2)};
                m_pending.clear();
                const std::string::size_type extension{field.find(';')};
                if (extension != std::string::npos) field = field.substr(0, extension);
                if (!ParseChunkSize(TrimString(field), m_remaining)) return false;
                if (m_remaining == 0) {
                    // Trailers may follow; nothing here reads them.
                    m_state = State::Done;
                    return true;
                }
                m_state = State::Data;
            } else if (m_pending.size() > 64) {
                // A size line this long is not a size line.
                return false;
            }
            break;
        }
        case State::Data: {
            const std::size_t available{size - pos};
            const std::size_t take{static_cast<std::size_t>(
                std::min<uint64_t>(m_remaining, static_cast<uint64_t>(available)))};
            if (take > 0 && !sink(data + pos, take)) return false;
            pos += take;
            m_remaining -= take;
            if (m_remaining == 0) {
                m_terminator = 2;
                m_state = State::DataTerminator;
            }
            break;
        }
        case State::DataTerminator: {
            const char c{data[pos++]};
            if ((m_terminator == 2 && c != '\r') || (m_terminator == 1 && c != '\n')) return false;
            if (--m_terminator == 0) m_state = State::Size;
            break;
        }
        case State::Done:
            // Anything after the terminating chunk is trailers, ignored.
            return true;
        }
    }
    return true;
}

namespace {
const std::string UPDATE_CHECK_HOST{"api.github.com"};



//! How long one step may make no progress before the exchange is abandoned.
//!
//! Reset by progress rather than counted from the start, so a slow but moving
//! transfer is not cut off the way a single overall budget cuts it off. That
//! distinction does not matter much for a few kilobytes of JSON; it is what
//! makes the same fetcher usable for an artifact, which is why it changes here
//! rather than in the phase that needs it.
constexpr std::chrono::seconds IDLE_TIMEOUT{10};

//! Ceiling on one exchange, redirects included.
//!
//! An idle timeout alone does not bound a server that sends a byte just often
//! enough to keep resetting it. At the response ceiling below that is patient
//! enough to run for months, so the pathological case needs its own bound. Set
//! far above any honest exchange, since the idle timeout is what ends a normal
//! failure.
constexpr std::chrono::seconds TOTAL_TIMEOUT{120};

//! The same ceiling for a download, which legitimately takes far longer. The
//! idle timeout is still what ends a stalled transfer; this only bounds one that
//! creeps along fast enough to keep resetting it.
constexpr std::chrono::seconds DOWNLOAD_TOTAL_TIMEOUT{3600};

//! Redirect hops to follow before giving up.
//!
//! Enough for the indirection a download host may grow, few enough that a
//! redirect loop ends promptly. Each hop is a fresh connection with its own
//! certificate verification against the host it actually reaches.
constexpr int MAX_REDIRECTS{5};

//! Ceiling on the whole response, headers included.
//!
//! The body is buffered in memory before it is parsed, so without a bound a
//! hostile or broken server can make this allocate until the process dies. The
//! release object this asks for runs to a few kilobytes, so a megabyte is two
//! orders of magnitude of headroom and still nowhere near enough to hurt.
//!
//! This is not the size limit for downloading an artifact. That path does not
//! exist yet, and when it does it must stream to disk rather than raise this.
constexpr std::size_t MAX_RESPONSE_BYTES{1024 * 1024};

//! Ceiling on a downloaded artifact.
//!
//! Streaming removes the memory problem, not the disk one: a server that never
//! stops sending would otherwise fill the filesystem. Set well above the largest
//! published artifact, which is 29 MB, so it bounds the pathological case
//! without second-guessing a legitimate release growing.
constexpr int64_t MAX_DOWNLOAD_BYTES{256 * 1024 * 1024};

//! Reason a streamed read stopped, so the caller can tell them apart.
enum class StreamResult {
    Complete,   //!< body ended, and framing proved it complete
    Cancelled,  //!< the caller asked to stop
    TooLarge,   //!< exceeded MAX_DOWNLOAD_BYTES
};

//! The request line and headers this client sends. Shared so a streamed request
//! is byte for byte the one the in-memory path sends.
std::string RequestFor(const std::string& method, const std::string& host, const std::string& target)
{
    std::ostringstream request;
    request << method << " " << target << " HTTP/1.1\r\n";
    request << "Host: " << host << "\r\n";
    request << "User-Agent: " << PACKAGE_NAME << "/" << PACKAGE_VERSION << "\r\n";
    request << "Accept: */*\r\n";
    request << "Connection: close\r\n\r\n";
    return request.str();
}

/**
 * One HTTPS exchange with one host.
 *
 * Boost's synchronous socket calls accept no timeout, which is why every step
 * here is issued asynchronously and driven by a bounded run of the io_context.
 * A stalled or unreachable server therefore costs IDLE_TIMEOUT rather than
 * however long the operating system takes to abandon the connection.
 *
 * One instance is one connection to one host. A redirect is a different host as
 * far as certificate verification is concerned, so following one means building
 * another of these rather than reusing this.
 */
class HttpsConnection
{
public:
    HttpsConnection(std::string host, std::chrono::steady_clock::time_point hard_deadline)
        : m_host{std::move(host)},
          m_hard_deadline{hard_deadline},
          m_ssl_ctx{boost::asio::ssl::context::tls_client},
          m_stream{m_ioc, m_ssl_ctx}
    {
        // Without this the connection is encrypted but unauthenticated: any
        // party able to intercept it can present any certificate and substitute
        // the response. The most useful thing that buys an attacker is silently
        // suppressing upgrade notices, which is the wrong failure mode for the
        // mechanism whose job is to get security fixes onto user machines.
        const std::string ca_error{node::LoadTrustedCACertificates(m_ssl_ctx.native_handle())};
        if (!ca_error.empty()) throw std::runtime_error(ca_error);

        // verify_peer rejects a certificate that does not chain to one of those
        // anchors; the callback additionally binds the certificate to the host
        // that was asked for, so a valid certificate for some other name is no
        // use. Boost 1.71 spells this rfc2818_verification; the
        // host_name_verification that replaced it arrived in 1.73.
        m_ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);
        m_ssl_ctx.set_verify_callback(boost::asio::ssl::rfc2818_verification(m_host));

        // Server Name Indication. Without it a host that serves several names
        // from one address, which includes anything behind a CDN, cannot tell
        // which certificate to present and may not serve the request at all.
        // This is a functional fix rather than a security one: the certificate
        // is checked against m_host above regardless of what SNI asked for.
        if (SSL_set_tlsext_host_name(m_stream.native_handle(), m_host.c_str()) != 1) {
            throw std::runtime_error("Could not set the TLS server name for " + m_host);
        }
    }

    //! Fetch target and return the raw response, status line and headers
    //! included, so the caller can act on a redirect. Throws std::runtime_error
    //! on any transport failure, including timeout.
    std::string Get(const std::string& target, bool& clean_eof);

    //! Resolve, connect and complete the TLS handshake.
    void Connect();

    //! Send the request and read only as far as the end of the headers, leaving
    //! the body unread on the stream. What arrived past the header terminator is
    //! returned in leftover, since a single read does not stop on a boundary.
    node::HttpHeaders GetHeaders(const std::string& method, const std::string& target,
                                 std::string& leftover);

    //! Stream the body that GetHeaders left on the stream into sink.
    StreamResult ReadBodyToSink(const node::HttpHeaders& headers, const std::string& leftover,
                                const std::function<bool(const char*, std::size_t)>& sink,
                                const std::function<void(int64_t, int64_t)>& progress,
                                const std::function<bool()>& cancel);

private:
    //! Pump the io_context until the outstanding operation reports back or the
    //! deadline passes, then rethrow whatever it reported.
    void Await(const std::string& what);

    //! Give the next step a fresh idle window, without letting it run past the
    //! ceiling on the exchange as a whole.
    void ArmIdleTimer()
    {
        m_deadline = std::min(std::chrono::steady_clock::now() + IDLE_TIMEOUT, m_hard_deadline);
    }

    std::string m_host;
    std::chrono::steady_clock::time_point m_hard_deadline;
    std::chrono::steady_clock::time_point m_deadline{};
    boost::asio::io_context m_ioc;
    boost::asio::ssl::context m_ssl_ctx;
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> m_stream;
    bool m_done{false};
    boost::system::error_code m_ec{};
};

void HttpsConnection::Await(const std::string& what)
{
    m_ioc.restart();
    while (!m_done) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= m_deadline) {
            // An outstanding asynchronous operation cannot simply be abandoned:
            // its handler holds references to objects owned by this class. Close
            // the socket so the operation fails, then let the handler run before
            // unwinding.
            boost::system::error_code ignored_error;
            m_stream.lowest_layer().close(ignored_error);
            m_ioc.run();
            throw std::runtime_error("Timed out while " + what);
        }
        m_ioc.run_one_for(m_deadline - now);
    }
    m_done = false;
    if (m_ec) throw std::runtime_error("Failed while " + what + ": " + m_ec.message());
}

void HttpsConnection::Connect()
{
    ArmIdleTimer();

    boost::asio::ip::tcp::resolver resolver{m_ioc};
    boost::asio::ip::tcp::resolver::results_type endpoints;
    resolver.async_resolve(m_host, "https",
        [&](const boost::system::error_code& ec, const boost::asio::ip::tcp::resolver::results_type& results) {
            m_ec = ec;
            endpoints = results;
            m_done = true;
        });
    Await("resolving " + m_host);

    ArmIdleTimer();
    boost::asio::async_connect(m_stream.lowest_layer(), endpoints,
        [&](const boost::system::error_code& ec, const boost::asio::ip::tcp::endpoint&) {
            m_ec = ec;
            m_done = true;
        });
    Await("connecting to " + m_host);

    ArmIdleTimer();
    m_stream.async_handshake(boost::asio::ssl::stream_base::client,
        [&](const boost::system::error_code& ec) {
            m_ec = ec;
            m_done = true;
        });
    Await("performing the TLS handshake with " + m_host);
}

std::string HttpsConnection::Get(const std::string& target, bool& clean_eof)
{
    Connect();

    const std::string request_text{RequestFor("GET", m_host, target)};
    boost::asio::streambuf request;
    std::ostream request_stream(&request);
    request_stream << request_text;

    ArmIdleTimer();
    boost::asio::async_write(m_stream, request,
        [&](const boost::system::error_code& ec, std::size_t) {
            m_ec = ec;
            m_done = true;
        });
    Await("sending the request to " + m_host);

    // The server was asked to close when done, so read to the end of the
    // stream. Neither ending is an error here: a clean end of file means the
    // peer sent close_notify, and a truncated stream means the connection went
    // away without one, which plenty of servers do. They are not equivalent
    // though, so which one arrived is carried out to the completeness check
    // rather than being flattened into success.
    //
    // Read in a loop rather than in one call. Two things follow from that: each
    // arrival of data is progress, so the idle window restarts, and the ceiling
    // is enforced while reading rather than inspected afterwards.
    //
    // Enforcing it while reading is not a tidiness preference. A bounded
    // streambuf does not report the bound being hit: asio stops once prepare()
    // can yield nothing more and completes as though the stream had ended, so a
    // truncated body arrives with no error at all. Growing the buffer under an
    // explicit test removes the failure mode rather than detecting it after the
    // fact.
    clean_eof = false;
    std::string raw;
    std::array<char, 16 * 1024> chunk{};
    for (;;) {
        ArmIdleTimer();
        std::size_t got{0};
        m_stream.async_read_some(boost::asio::buffer(chunk),
            [&](const boost::system::error_code& ec, std::size_t bytes) {
                got = bytes;
                clean_eof = (ec == boost::asio::error::eof);
                m_ec = (ec == boost::asio::error::eof || ec == boost::asio::ssl::error::stream_truncated)
                           ? boost::system::error_code{}
                           : ec;
                m_done = true;
            });
        const bool finished{[&] {
            Await("reading the response from " + m_host);
            return clean_eof || got == 0;
        }()};

        if (got > 0) {
            if (raw.size() + got > MAX_RESPONSE_BYTES) {
                throw std::runtime_error("Response from " + m_host + " exceeded " +
                                         ToString(MAX_RESPONSE_BYTES) + " bytes");
            }
            raw.append(chunk.data(), got);
        }
        if (finished) break;
    }

    return raw;
}

node::HttpHeaders HttpsConnection::GetHeaders(const std::string& method, const std::string& target,
                                              std::string& leftover)
{
    Connect();

    const std::string request_text{RequestFor(method, m_host, target)};
    boost::asio::streambuf request;
    std::ostream request_stream(&request);
    request_stream << request_text;

    ArmIdleTimer();
    boost::asio::async_write(m_stream, request,
        [&](const boost::system::error_code& ec, std::size_t) {
            m_ec = ec;
            m_done = true;
        });
    Await("sending the request to " + m_host);

    // Read only until the headers are complete. A read does not stop on a
    // boundary, so whatever arrived past the terminator is handed back rather
    // than dropped: for a small body it can be the whole thing.
    std::string raw;
    std::array<char, 16 * 1024> chunk{};
    std::string::size_type terminator{std::string::npos};
    for (;;) {
        ArmIdleTimer();
        std::size_t got{0};
        bool ended{false};
        m_stream.async_read_some(boost::asio::buffer(chunk),
            [&](const boost::system::error_code& ec, std::size_t bytes) {
                got = bytes;
                ended = (ec == boost::asio::error::eof ||
                         ec == boost::asio::ssl::error::stream_truncated);
                m_ec = ended ? boost::system::error_code{} : ec;
                m_done = true;
            });
        Await("reading the response headers from " + m_host);

        if (got > 0) {
            raw.append(chunk.data(), got);
            terminator = raw.find("\r\n\r\n");
            if (terminator != std::string::npos) break;
            if (raw.size() > MAX_RESPONSE_BYTES) {
                throw std::runtime_error("Response headers from " + m_host + " exceeded " +
                                         ToString(MAX_RESPONSE_BYTES) + " bytes");
            }
        }
        if (ended || got == 0) break;
    }

    if (terminator == std::string::npos) throw std::runtime_error("Invalid response");
    leftover = raw.substr(terminator + 4);
    return node::ParseHttpHeaders(raw.substr(0, terminator));
}

StreamResult HttpsConnection::ReadBodyToSink(const node::HttpHeaders& headers,
                                             const std::string& leftover,
                                             const std::function<bool(const char*, std::size_t)>& sink,
                                             const std::function<void(int64_t, int64_t)>& progress,
                                             const std::function<bool()>& cancel)
{
    int64_t received{0};
    bool sink_failed{false};

    node::ChunkedDecoder decoder;
    const auto deliver = [&](const char* data, std::size_t size) {
        if (!sink(data, size)) {
            sink_failed = true;
            return false;
        }
        received += static_cast<int64_t>(size);
        return true;
    };

    // Feeds one run of raw stream bytes through the framing, if any, into the
    // sink. Returns false to stop the read.
    const auto consume = [&](const char* data, std::size_t size) {
        if (headers.chunked) return decoder.Feed(data, size, deliver);
        return deliver(data, size);
    };

    if (!leftover.empty() && !consume(leftover.data(), leftover.size())) {
        if (sink_failed) throw std::runtime_error("Could not write the downloaded data");
        throw std::runtime_error("Malformed chunked response from " + m_host);
    }
    if (progress) progress(received, headers.content_length);

    const bool have_all{[&] {
        if (headers.chunked) return decoder.Complete();
        return headers.content_length >= 0 && received >= headers.content_length;
    }()};

    std::array<char, 64 * 1024> chunk{};
    bool clean_eof{false};
    while (!have_all) {
        if (cancel && cancel()) return StreamResult::Cancelled;

        ArmIdleTimer();
        std::size_t got{0};
        bool ended{false};
        m_stream.async_read_some(boost::asio::buffer(chunk),
            [&](const boost::system::error_code& ec, std::size_t bytes) {
                got = bytes;
                clean_eof = (ec == boost::asio::error::eof);
                ended = (ec == boost::asio::error::eof ||
                         ec == boost::asio::ssl::error::stream_truncated);
                m_ec = ended ? boost::system::error_code{} : ec;
                m_done = true;
            });
        Await("reading the response body from " + m_host);

        if (got > 0) {
            if (received + static_cast<int64_t>(got) > MAX_DOWNLOAD_BYTES) return StreamResult::TooLarge;
            if (!consume(chunk.data(), got)) {
                if (sink_failed) throw std::runtime_error("Could not write the downloaded data");
                throw std::runtime_error("Malformed chunked response from " + m_host);
            }
            if (progress) progress(received, headers.content_length);
        }

        if (headers.chunked ? decoder.Complete()
                            : (headers.content_length >= 0 && received >= headers.content_length)) {
            break;
        }
        if (ended || got == 0) {
            // The framing decides whether this was the end or a truncation.
            if (headers.chunked && !decoder.Complete()) {
                throw std::runtime_error("Chunked response ended before its final chunk");
            }
            if (headers.content_length >= 0 && received < headers.content_length) {
                throw std::runtime_error("Incomplete download: got " + ToString(received) +
                                         " of " + ToString(headers.content_length) + " bytes");
            }
            if (headers.content_length < 0 && !headers.chunked && !clean_eof) {
                throw std::runtime_error(
                    "Download gave no Content-Length and ended without a clean shutdown");
            }
            break;
        }
    }

    return StreamResult::Complete;
}

/**
 * Fetch a target from a host, following redirects, and return the body.
 *
 * Each hop is a new connection and therefore a new certificate check against
 * the host actually reached. A redirect that leaves https is refused rather
 * than followed: silently continuing over plaintext would discard the
 * verification this fetcher exists to perform.
 */
std::string HttpsGet(const std::string& start_host, const std::string& start_target)
{
    const auto hard_deadline{std::chrono::steady_clock::now() + TOTAL_TIMEOUT};
    std::string host{start_host};
    std::string target{start_target};

    for (int hop{0}; hop <= MAX_REDIRECTS; ++hop) {
        HttpsConnection connection{host, hard_deadline};
        bool clean_eof{false};
        const std::string raw{connection.Get(target, clean_eof)};

        const node::HttpResponse response{node::ParseHttpResponse(raw, clean_eof)};
        if (response.status == 200) return response.body;

        if (response.status < 300 || response.status >= 400) {
            throw std::runtime_error("Response returned with status code " + ToString(response.status));
        }
        if (response.location.empty()) {
            throw std::runtime_error("Response returned status code " + ToString(response.status) +
                                     " without a Location header");
        }
        const node::Url next{node::ResolveRedirect(host, target, response.location)};
        host = next.host;
        target = next.target;
    }

    throw std::runtime_error("Gave up after " + ToString(MAX_REDIRECTS) + " redirects");
}
} // namespace

node::HttpHeaders node::ParseHttpHeaders(const std::string& headers)
{
    HttpHeaders out;

    // Status line.
    const std::string::size_type eol{headers.find("\r\n")};
    std::istringstream status_stream{eol == std::string::npos ? headers : headers.substr(0, eol)};
    std::string http_version;
    unsigned int status_code{0};
    status_stream >> http_version >> status_code;
    if (!status_stream || http_version.substr(0, 5) != "HTTP/") {
        throw std::runtime_error("Invalid response");
    }
    out.status = status_code;
    out.location = GetHeader(headers, "location");

    const std::string transfer_encoding{ToLower(GetHeader(headers, "transfer-encoding"))};
    if (!transfer_encoding.empty()) {
        if (transfer_encoding != "chunked") {
            throw std::runtime_error("Response uses an unsupported transfer encoding: " + transfer_encoding);
        }
        out.chunked = true;
    }

    // Content-Length is not consulted on a chunked body. RFC 7230 forbids
    // sending both, and the terminating chunk is what proves completeness.
    const std::string content_length{GetHeader(headers, "content-length")};
    if (!out.chunked && !content_length.empty()) {
        int64_t expected{0};
        if (!ParseInt64(content_length, &expected) || expected < 0) {
            throw std::runtime_error("Response has an unusable Content-Length: " + content_length);
        }
        out.content_length = expected;
    }
    return out;
}

node::HttpResponse node::ParseHttpResponse(const std::string& raw_response, bool clean_eof)
{
    HttpResponse out;

    // Headers are terminated by a blank line; everything after it is the body.
    const std::string::size_type terminator{raw_response.find("\r\n\r\n")};
    if (terminator == std::string::npos) throw std::runtime_error("Invalid response");
    const std::string headers{raw_response.substr(0, terminator)};
    std::string body{raw_response.substr(terminator + 4)};

    const HttpHeaders parsed{ParseHttpHeaders(headers)};
    out.status = parsed.status;
    out.location = parsed.location;

    // A chunked body carries its own framing, so the chunk sizes have to be
    // stripped out before anything downstream sees the content. api.github.com
    // does use this: it answers with Content-Length most of the time and
    // switches to chunked intermittently, so a client that cannot reassemble a
    // chunked body fails a fraction of its checks for no visible reason.
    //
    // Content-Length is not consulted in this case. RFC 7230 forbids sending
    // both, and the terminating chunk is what proves the body is complete.
    // A redirect's body is discarded, so its framing is not checked. Rejecting
    // a valid redirect because the server closed without a Content-Length would
    // fail the exchange over a body nothing reads.
    if (out.status != 200) {
        out.body = body;
        return out;
    }

    if (parsed.chunked) {
        out.body = DecodeChunkedBody(body);
        return out;
    }

    if (parsed.content_length >= 0) {
        if (body.size() != static_cast<std::string::size_type>(parsed.content_length)) {
            throw std::runtime_error("Incomplete response: got " + ToString(body.size()) +
                                     " of " + ToString(parsed.content_length) + " bytes");
        }
    } else if (!clean_eof) {
        // Without a Content-Length the body runs until the connection closes,
        // so a clean shutdown is the only evidence that all of it arrived.
        throw std::runtime_error("Response gave no Content-Length and ended without a clean shutdown");
    }

    out.body = body;
    return out;
}

std::string node::ExtractHttpBody(const std::string& raw_response, bool clean_eof)
{
    const HttpResponse response{ParseHttpResponse(raw_response, clean_eof)};
    if (response.status != 200) {
        throw std::runtime_error("Response returned with status code " + ToString(response.status));
    }
    return response.body;
}

const std::string node::RELEASE_DOWNLOAD_HOST{"download.reddcoin.com"};

bool node::FetchToString(const std::string& host, const std::string& target, std::string& out,
                         std::string& error)
{
    error.clear();
    try {
        out = HttpsGet(host, target);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

node::ProbeResult node::ProbeRemoteFile(const std::string& host_in, const std::string& target_in,
                                        RemoteFile& out, std::string& error)
{
    error.clear();

    // A probe is a question, not a transfer, so it gets the short ceiling
    // rather than the download one.
    const auto hard_deadline{std::chrono::steady_clock::now() + TOTAL_TIMEOUT};
    std::string host{host_in};
    std::string target{target_in};

    try {
        for (int hop{0}; hop <= MAX_REDIRECTS; ++hop) {
            HttpsConnection connection{host, hard_deadline};
            std::string leftover;
            const HttpHeaders headers{connection.GetHeaders("HEAD", target, leftover)};

            if (headers.status >= 300 && headers.status < 400) {
                if (headers.location.empty()) {
                    error = "Response returned status code " + ToString(headers.status) +
                            " without a Location header";
                    return ProbeResult::Unknown;
                }
                const Url next{ResolveRedirect(host, target, headers.location)};
                host = next.host;
                target = next.target;
                continue;
            }

            if (headers.status == 200) {
                // Content-Length on a HEAD describes the body that a GET would
                // return, which is exactly the size being asked about.
                out.size = headers.content_length;
                return ProbeResult::Present;
            }
            if (headers.status == 404 || headers.status == 410) {
                return ProbeResult::Absent;
            }

            error = "Response returned with status code " + ToString(headers.status);
            return ProbeResult::Unknown;
        }
        error = "Gave up after " + ToString(MAX_REDIRECTS) + " redirects";
    } catch (const std::exception& e) {
        error = e.what();
    }
    return ProbeResult::Unknown;
}

bool node::DownloadToFile(const std::string& host_in, const std::string& target_in,
                          const fs::path& destination, const DownloadProgress& progress,
                          const DownloadCancel& cancel, std::string& error)
{
    error.clear();

    // Written beside the destination rather than to a temporary directory, so
    // the rename at the end cannot cross a filesystem boundary and turn into a
    // copy that can itself fail half way.
    const fs::path partial{destination.string() + ".part"};

    const auto hard_deadline{std::chrono::steady_clock::now() + DOWNLOAD_TOTAL_TIMEOUT};
    std::string host{host_in};
    std::string target{target_in};

    try {
        for (int hop{0}; hop <= MAX_REDIRECTS; ++hop) {
            HttpsConnection connection{host, hard_deadline};
            std::string leftover;
            const HttpHeaders headers{connection.GetHeaders("GET", target, leftover)};

            if (headers.status >= 300 && headers.status < 400) {
                if (headers.location.empty()) {
                    error = "Response returned status code " + ToString(headers.status) +
                            " without a Location header";
                    return false;
                }
                const Url next{ResolveRedirect(host, target, headers.location)};
                host = next.host;
                target = next.target;
                continue;
            }
            if (headers.status != 200) {
                error = "Response returned with status code " + ToString(headers.status);
                return false;
            }

            // Refuse before writing anything, when the server is willing to say
            // how much it intends to send.
            if (headers.content_length > MAX_DOWNLOAD_BYTES) {
                error = "Refusing a download of " + ToString(headers.content_length) +
                        " bytes, over the " + ToString(MAX_DOWNLOAD_BYTES) + " byte limit";
                return false;
            }

            fsbridge::ofstream out{partial, std::ios::binary | std::ios::trunc};
            if (!out.good()) {
                error = "Could not open " + partial.string() + " for writing";
                return false;
            }

            const auto sink = [&out](const char* data, std::size_t size) {
                out.write(data, static_cast<std::streamsize>(size));
                return out.good();
            };

            const StreamResult result{
                connection.ReadBodyToSink(headers, leftover, sink, progress, cancel)};
            out.close();

            if (result != StreamResult::Complete) {
                fs::remove(partial);
                if (result == StreamResult::TooLarge) {
                    error = "Download exceeded " + ToString(MAX_DOWNLOAD_BYTES) + " bytes";
                }
                // A cancelled download leaves error empty: the caller asked for
                // this and does not need telling it happened.
                return false;
            }

            if (!RenameOver(partial, destination)) {
                fs::remove(partial);
                error = "Could not move the download into place at " + destination.string();
                return false;
            }
            return true;
        }
        error = "Gave up after " + ToString(MAX_REDIRECTS) + " redirects";
    } catch (const std::exception& e) {
        error = e.what();
    }

    fs::remove(partial);
    return false;
}

node::Url node::ResolveRedirect(const std::string& base_host, const std::string& base_target,
                                const std::string& location)
{
    if (location.empty()) throw std::runtime_error("Redirect has an empty Location");

    // Absolute, or protocol relative. Anything that is not https is refused
    // rather than followed: continuing over plaintext would quietly discard the
    // certificate verification this client exists to perform, and a redirect is
    // exactly where an attacker would try to introduce that.
    std::string rest;
    if (location.compare(0, 8, "https://") == 0) {
        rest = location.substr(8);
    } else if (location.compare(0, 2, "//") == 0) {
        rest = location.substr(2);
    } else if (location.find("://") != std::string::npos) {
        throw std::runtime_error("Refusing to follow a redirect that leaves https: " + location);
    } else {
        // Same host. An absolute path replaces the target; anything else is
        // relative to the directory the current target sits in.
        if (location[0] == '/') return Url{base_host, location};
        const std::string::size_type slash{base_target.rfind('/')};
        const std::string dir{slash == std::string::npos ? "/" : base_target.substr(0, slash + 1)};
        return Url{base_host, dir + location};
    }

    const std::string::size_type slash{rest.find('/')};
    const std::string host{slash == std::string::npos ? rest : rest.substr(0, slash)};
    const std::string target{slash == std::string::npos ? "/" : rest.substr(slash)};
    if (host.empty()) throw std::runtime_error("Redirect has no host: " + location);

    // A port would need carrying through to the connect, which nothing this
    // talks to requires. Refusing is honest; quietly ignoring it would connect
    // somewhere other than where the server asked.
    if (host.find(':') != std::string::npos) {
        throw std::runtime_error("Refusing to follow a redirect to a non-default port: " + location);
    }
    return Url{host, target};
}

void node::CheckForUpdates(UniValue& result)
{
    std::string installedVersion = PACKAGE_VERSION;
    std::string repositoryVersion = "";
    std::string localVersion = "";
    std::string remoteVersion = "";
    bool updateAvailable = false;
    std::string message = "";
    std::string warning = "";
    std::string officialDownloadLink = "";
    std::string errors = "";
    std::string platform = "";
    std::string guiArtifact = "";
    std::string guiArtifactLink = "";
    std::string daemonArtifact = "";
    std::string daemonArtifactLink = "";
    int64_t artifactBytes = -1;

    try {
        const std::string str_response{HttpsGet(UPDATE_CHECK_HOST, strGithubLink)};

        // read response into Univalue Obj
        UniValue obj_response(UniValue::VOBJ);

        auto success = obj_response.read(str_response);

        if (success) {
            if (obj_response.exists("tag_name")) {
                repositoryVersion = obj_response["tag_name"].get_str();

                /** Accepts three- and four-component versions, with or without a
                 * leading "v" and an optional alpha/beta/rc suffix:
                 *
                 *   v4.22.9      v4.22.9.4      v4.22.0-alpha-1      v4.22.5-rc.1
                 *
                 * Match 1: major        Match 2: minor        Match 3: revision
                 * Match 4: build, the point-release number, absent on a
                 *          three-component version
                 * Match 5: prerelease tag       Match 6: prerelease number
                 *
                 * Anchoring is deliberate. An unanchored search for three
                 * components slides past the major on a four-component string,
                 * so "4.22.9.4" matched as "22.9.4" and compared greater than
                 * any real release, silently suppressing the update notice.
                 *
                 * semver has no fourth component, so it orders the first three
                 * plus any prerelease tag and the build number breaks ties.
                 */
                std::regex versionRgx(R"(^v?([0-9]+)\.([0-9]+)\.([0-9]+)(?:\.([0-9]+))?(?:[-.]?(alpha|beta|rc)[-.]?([0-9A-Za-z.-]*))?$)");

                struct ParsedVersion {
                    semver::version core;   //!< first three components plus prerelease
                    int build{0};           //!< fourth component, 0 when absent
                    std::string numeric;    //!< "4.22.9" or "4.22.9.4"
                    std::string text;       //!< as supplied, without a leading "v"
                    std::string prerelease_tag;
                    std::string prerelease_num;
                };

                const auto parse_version = [&versionRgx](const std::string& raw) {
                    std::smatch m;
                    if (!std::regex_match(raw, m, versionRgx)) {
                        throw std::runtime_error("unrecognised version string: " + raw);
                    }
                    ParsedVersion parsed;
                    parsed.numeric = m[1].str() + "." + m[2].str() + "." + m[3].str();
                    std::string core{parsed.numeric};
                    if (m[5].matched && !m[5].str().empty()) {
                        parsed.prerelease_tag = m[5].str();
                        parsed.prerelease_num = m[6].str();
                        core += "-" + parsed.prerelease_tag;
                        if (!parsed.prerelease_num.empty()) core += "." + parsed.prerelease_num;
                    }
                    parsed.core = semver::version::parse(core, false);
                    if (m[4].matched && !m[4].str().empty()) {
                        // std::stoi is locale dependent and rejected by
                        // test/lint/lint-locale-dependence.sh.
                        int32_t build{0};
                        if (!ParseInt32(m[4].str(), &build)) {
                            throw std::runtime_error("unparsable version component in: " + raw);
                        }
                        parsed.build = build;
                        parsed.numeric += "." + m[4].str();
                    }
                    parsed.text = (!raw.empty() && raw.front() == 'v') ? raw.substr(1) : raw;
                    return parsed;
                };

                const ParsedVersion local{parse_version(installedVersion)};
                const ParsedVersion remote{parse_version(repositoryVersion)};
                localVersion = local.text;
                remoteVersion = remote.text;

                const bool remote_is_newer{remote.core > local.core ||
                                           (remote.core == local.core && remote.build > local.build)};
                const bool same_version{remote.core == local.core && remote.build == local.build};

                if (remote_is_newer) {
                    updateAvailable = true;
                    message = "Please download the latest version (" + remote.text + ") from our official website";
                } else if (same_version) {
                    message = "You're running the most recent version of Reddcoin Core (" + local.text + ")";
                }

                // Build direct download link
                officialDownloadLink = strDownloadLink + remote.numeric;
                if (remote.core.is_prerelease()) {
                    officialDownloadLink += "/" + remote.prerelease_tag + remote.prerelease_num;
                }

                // Name the exact file this host needs, so a caller can offer a
                // download rather than a directory to read.
                //
                // Not attempted for a prerelease. No prerelease has ever been
                // published, so the naming convention for one is unverified,
                // and naming a file that does not exist is worse than naming
                // none: it turns a working notice into a broken link.
                if (!remote.core.is_prerelease()) {
                    ReleaseArtifacts artifacts;
                    if (GetReleaseArtifactsForThisHost(remote.numeric, artifacts)) {
                        platform = artifacts.platform;
                        guiArtifact = artifacts.gui;
                        daemonArtifact = artifacts.daemon;
                        guiArtifactLink = officialDownloadLink + "/" + artifacts.gui;
                        daemonArtifactLink = officialDownloadLink + "/" + artifacts.daemon;

                        // Confirm the file is actually there before naming it.
                        // The name is derived from a convention rather than read
                        // from the server, so a release published under a
                        // different scheme would otherwise produce a confident
                        // link to nothing.
                        //
                        // Fails open, deliberately. Only a definite 404 withdraws
                        // the name; a timeout or a 500 leaves it in place, since
                        // a probe that could not reach the server is no evidence
                        // that the artifact is missing, and degrading a working
                        // notice on a network hiccup would be the worse error.
                        std::string probe_error;
                        RemoteFile remote_file;
                        const std::string probe_target{"/bin/reddcoin-core-" + remote.numeric +
                                                       "/" + artifacts.gui};
                        const ProbeResult probed{
                            ProbeRemoteFile(RELEASE_DOWNLOAD_HOST, probe_target, remote_file, probe_error)};
                        if (probed == ProbeResult::Absent) {
                            guiArtifact.clear();
                            daemonArtifact.clear();
                            guiArtifactLink.clear();
                            daemonArtifactLink.clear();
                        } else if (probed == ProbeResult::Present) {
                            artifactBytes = remote_file.size;
                        }
                    }
                }
            }
        }

        std::string preleaseWarning = "";

        // Display pre-release note if the installed version is a pre-release version
        if (!CLIENT_VERSION_IS_RELEASE) {
            warning = "This is a pre-release test build - use at your own risk - do not use for staking or merchant applications";
        }

    } catch (std::exception& e) {
        errors = e.what();
    }

    result.pushKV("localversion", localVersion);
    result.pushKV("remoteversion", remoteVersion);
    result.pushKV("updateavailable", updateAvailable);
    result.pushKV("message", message);
    result.pushKV("warning", warning);
    result.pushKV("officialDownloadLink", officialDownloadLink);
    // Empty rather than absent when the host publishes no build, or when the
    // check did not get far enough to know, so the shape of the result does not
    // depend on whether it succeeded.
    result.pushKV("hosttriplet", HostTriplet());
    // -1 when the probe did not run, could not reach the server, or the server
    // gave no Content-Length. A size of zero would be a real answer, so absence
    // needs its own value rather than being folded into it.
    result.pushKV("artifactbytes", artifactBytes);
    result.pushKV("platform", platform);
    result.pushKV("guiartifact", guiArtifact);
    result.pushKV("guiartifactlink", guiArtifactLink);
    result.pushKV("daemonartifact", daemonArtifact);
    result.pushKV("daemonartifactlink", daemonArtifactLink);
    result.pushKV("errors", errors);
}
