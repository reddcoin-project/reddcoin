// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/update_check.h>

#include <clientversion.h>
#include <util/semver.h>
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

#include <chrono>
#include <cstddef>
#include <ostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

static std::string strDownloadLink = "https://download.reddcoin.com/bin/reddcoin-core-";
static std::string strGithubLink = "/repos/reddcoin-project/reddcoin/releases/latest";

namespace {
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
    // stream. Both a clean end of file and a connection dropped without a TLS
    // shutdown mean the response is complete.
    boost::asio::streambuf response;
    boost::asio::async_read(m_stream, response, boost::asio::transfer_all(),
        [&](const boost::system::error_code& ec, std::size_t) {
            m_ec = (ec == boost::asio::error::eof || ec == boost::asio::ssl::error::stream_truncated)
                       ? boost::system::error_code{}
                       : ec;
            m_done = true;
        });
    Await("reading the response from " + m_host);

    std::ostringstream raw;
    raw << &response;
    const std::string str_raw{raw.str()};

    // Status line.
    const std::string::size_type eol{str_raw.find("\r\n")};
    if (eol == std::string::npos) throw std::runtime_error("Invalid response");
    std::istringstream status_stream{str_raw.substr(0, eol)};
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
    const std::string::size_type body{str_raw.find("\r\n\r\n")};
    if (body == std::string::npos) throw std::runtime_error("Invalid response");
    return str_raw.substr(body + 4);
}
} // namespace

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

                /** "tag_name": "v4.22.0-alpha-1",
                 * "v(([0-9]+).([0-9]+).([0-9]+))((-?(alpha|beta|rc))(-?(.*))|$)"
                 *
                 * Match 0:    v4.22.0-alpha-1
                 * Match 1:    4.22.0
                 * Match 2:    4             <- this
                 * Match 3:    22            <- this
                 * Match 4:    0             <- this
                 * Match 5:    -alpha-1
                 * Match 6:    -alpha
                 * Match 7:    alpha         <- this
                 * Match 8:    -1
                 * Match 9:    1             <- this
                 *
                 */

                std::regex versionRgx("(([0-9]+).([0-9]+).([0-9]+))([-|.]?((alpha|beta|rc)([-|.]?(.*)))|$)");
                std::smatch remoteMatches;
                std::smatch localMatches;

                std::regex_search(installedVersion, localMatches, versionRgx);
                std::regex_search(repositoryVersion, remoteMatches, versionRgx);

                std::string strRemotePrerelease = "";
                if (remoteMatches[6] != "") {
                    strRemotePrerelease += "-" + remoteMatches[7].str() + "." + remoteMatches[9].str();
                }
                auto remoteV = semver::version::parse(remoteMatches[1].str() + strRemotePrerelease, false);
                remoteVersion = remoteV.str();

                std::string strLocalPrerelease = "";
                if (localMatches[6] != "") {
                    strLocalPrerelease += "-" + localMatches[7].str() + "." + localMatches[9].str();
                }
                auto localV = semver::version::parse(localMatches[1].str() + strLocalPrerelease, false);
                localVersion = localV.str();

                if (remoteV > localV) {
                    updateAvailable = true;
                    message = "Please download the latest version (" + remoteV.str() + ") from our official website";
                } else if (remoteV == localV) {
                    message = "You're running the most recent version of Reddcoin Core (" + localV.str() + ")";
                }

                // Build direct download link
                officialDownloadLink = strDownloadLink + remoteMatches[1].str();
                if (remoteV.is_prerelease()) {
                    officialDownloadLink += "/" + remoteMatches[7].str() + remoteMatches[9].str();
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
