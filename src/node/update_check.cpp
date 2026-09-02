// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/update_check.h>

#include <clientversion.h>
#include <node/ca_store.h>
#include <util/semver.h>
#include <util/strencodings.h>
#include <util/string.h>

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

const std::string UPDATE_CHECK_HOST{"api.github.com"};

//! Budget for the whole exchange, from name resolution to the last byte of the
//! response. The update check is a background nicety, so it gives up rather
//! than holding anything up for long.
constexpr std::chrono::seconds UPDATE_CHECK_TIMEOUT{10};

/**
 * Minimal one-shot HTTPS GET with an overall deadline.
 *
 * Boost's synchronous socket calls accept no timeout, which is why every step
 * here is issued asynchronously and driven by a bounded run of the io_context.
 * A stalled or unreachable server therefore costs UPDATE_CHECK_TIMEOUT rather
 * than however long the operating system takes to abandon the connection.
 */
class HttpsFetcher
{
public:
    HttpsFetcher(std::string host, std::chrono::steady_clock::duration timeout)
        : m_host{std::move(host)},
          m_timeout{timeout},
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

    //! Fetch target and return the response body. Throws std::runtime_error on
    //! any failure, including timeout and a non-200 status.
    std::string Get(const std::string& target);

private:
    //! Pump the io_context until the outstanding operation reports back or the
    //! deadline passes, then rethrow whatever it reported.
    void Await(const std::string& what);

    std::string m_host;
    std::chrono::steady_clock::duration m_timeout;
    std::chrono::steady_clock::time_point m_deadline{};
    boost::asio::io_context m_ioc;
    boost::asio::ssl::context m_ssl_ctx;
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> m_stream;
    bool m_done{false};
    boost::system::error_code m_ec{};
};

void HttpsFetcher::Await(const std::string& what)
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

std::string HttpsFetcher::Get(const std::string& target)
{
    m_deadline = std::chrono::steady_clock::now() + m_timeout;

    boost::asio::ip::tcp::resolver resolver{m_ioc};
    boost::asio::ip::tcp::resolver::results_type endpoints;
    resolver.async_resolve(m_host, "https",
        [&](const boost::system::error_code& ec, const boost::asio::ip::tcp::resolver::results_type& results) {
            m_ec = ec;
            endpoints = results;
            m_done = true;
        });
    Await("resolving " + m_host);

    boost::asio::async_connect(m_stream.lowest_layer(), endpoints,
        [&](const boost::system::error_code& ec, const boost::asio::ip::tcp::endpoint&) {
            m_ec = ec;
            m_done = true;
        });
    Await("connecting to " + m_host);

    m_stream.async_handshake(boost::asio::ssl::stream_base::client,
        [&](const boost::system::error_code& ec) {
            m_ec = ec;
            m_done = true;
        });
    Await("performing the TLS handshake with " + m_host);

    boost::asio::streambuf request;
    std::ostream request_stream(&request);
    request_stream << "GET " << target << " HTTP/1.1\r\n";
    request_stream << "Host: " << m_host << "\r\n";
    request_stream << "User-Agent: " << PACKAGE_NAME << "/" << PACKAGE_VERSION << "\r\n";
    request_stream << "Accept: */*\r\n";
    request_stream << "Connection: close\r\n\r\n";

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
    // though, so which one arrived is carried through to the completeness check
    // rather than being flattened into success.
    bool clean_eof{false};
    boost::asio::streambuf response;
    boost::asio::async_read(m_stream, response, boost::asio::transfer_all(),
        [&](const boost::system::error_code& ec, std::size_t) {
            clean_eof = (ec == boost::asio::error::eof);
            m_ec = (ec == boost::asio::error::eof || ec == boost::asio::ssl::error::stream_truncated)
                       ? boost::system::error_code{}
                       : ec;
            m_done = true;
        });
    Await("reading the response from " + m_host);

    std::ostringstream raw;
    raw << &response;
    return node::ExtractHttpBody(raw.str(), clean_eof);
}
} // namespace

std::string node::ExtractHttpBody(const std::string& raw_response, bool clean_eof)
{
    // Status line.
    const std::string::size_type eol{raw_response.find("\r\n")};
    if (eol == std::string::npos) throw std::runtime_error("Invalid response");
    std::istringstream status_stream{raw_response.substr(0, eol)};
    std::string http_version;
    unsigned int status_code{0};
    status_stream >> http_version >> status_code;
    if (!status_stream || http_version.substr(0, 5) != "HTTP/") {
        throw std::runtime_error("Invalid response");
    }
    if (status_code != 200) {
        throw std::runtime_error("Response returned with status code " + ToString(status_code));
    }

    // Headers are terminated by a blank line; everything after it is the body.
    const std::string::size_type terminator{raw_response.find("\r\n\r\n")};
    if (terminator == std::string::npos) throw std::runtime_error("Invalid response");
    const std::string headers{raw_response.substr(0, terminator)};
    std::string body{raw_response.substr(terminator + 4)};

    // A chunked body carries its own framing, so the chunk sizes have to be
    // stripped out before anything downstream sees the content. api.github.com
    // does use this: it answers with Content-Length most of the time and
    // switches to chunked intermittently, so a client that cannot reassemble a
    // chunked body fails a fraction of its checks for no visible reason.
    //
    // Content-Length is not consulted in this case. RFC 7230 forbids sending
    // both, and the terminating chunk is what proves the body is complete.
    const std::string transfer_encoding{ToLower(GetHeader(headers, "transfer-encoding"))};
    if (!transfer_encoding.empty()) {
        if (transfer_encoding != "chunked") {
            throw std::runtime_error("Response uses an unsupported transfer encoding: " + transfer_encoding);
        }
        return DecodeChunkedBody(body);
    }

    const std::string content_length{GetHeader(headers, "content-length")};
    if (!content_length.empty()) {
        int64_t expected{0};
        if (!ParseInt64(content_length, &expected) || expected < 0) {
            throw std::runtime_error("Response has an unusable Content-Length: " + content_length);
        }
        if (body.size() != static_cast<std::string::size_type>(expected)) {
            throw std::runtime_error("Incomplete response: got " + ToString(body.size()) +
                                     " of " + content_length + " bytes");
        }
    } else if (!clean_eof) {
        // Without a Content-Length the body runs until the connection closes,
        // so a clean shutdown is the only evidence that all of it arrived.
        throw std::runtime_error("Response gave no Content-Length and ended without a clean shutdown");
    }

    return body;
}

std::string node::SslVersion()
{
    // OpenSSL_version has been available since 1.1.0, which predates every
    // version this can be built against.
    const char* version{OpenSSL_version(OPENSSL_VERSION)};
    return version ? version : "unknown";
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

    try {
        HttpsFetcher fetcher{UPDATE_CHECK_HOST, UPDATE_CHECK_TIMEOUT};
        const std::string str_response{fetcher.Get(strGithubLink)};

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
    result.pushKV("errors", errors);
}
