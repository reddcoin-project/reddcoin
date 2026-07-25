// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/update_check.h>

#include <clientversion.h>
#include <util/semver.h>

#include <univalue.h>

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>

#include <regex>
#include <sstream>
#include <string>

static std::string strDownloadLink = "https://download.reddcoin.com/bin/reddcoin-core-";
static std::string strGithubLink = "/repos/reddcoin-project/reddcoin/releases/latest";

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
        boost::asio::io_service svc;
        boost::asio::ssl::context ctx(boost::asio::ssl::context::method::sslv23_client);
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> ssock(svc, ctx);
        boost::asio::ip::tcp::resolver resolver(svc);
        boost::asio::ip::tcp::resolver::query query("api.github.com", "https");
        boost::asio::ip::tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);

        // Establish a connection.
        boost::asio::connect(ssock.lowest_layer(), endpoint_iterator);
        ssock.handshake(boost::asio::ssl::stream_base::handshake_type::client);

        // Send request
        boost::asio::streambuf request;
        std::ostream request_stream(&request);
        request_stream << "GET " << strGithubLink << " HTTP/1.1\r\n"; // note that you can change it if you wish to HTTP/1.0
        request_stream << "Host: api.github.com\r\n";
        request_stream << "User-Agent: C/1.0\r\n";
        request_stream << "Content-Type: application/json; charset=utf-8\r\n";
        request_stream << "Accept: */*\r\n";
        request_stream << "Connection: close\r\n\r\n";

        boost::asio::write(ssock, request);

        // Read the response status line. The response streambuf will automatically
        // grow to accommodate the entire line. The growth may be limited by passing
        // a maximum size to the streambuf constructor.
        boost::asio::streambuf response;
        boost::asio::read_until(ssock, response, "\r\n");

        // Check that response is OK.
        std::istream response_stream(&response);
        std::string http_version;
        response_stream >> http_version;
        unsigned int status_code;
        response_stream >> status_code;
        std::string status_message;
        std::getline(response_stream, status_message);
        if (!response_stream || http_version.substr(0, 5) != "HTTP/") {
            errors = "Invalid response";
        }
        if (status_code != 200) {
            errors = "Response returned with status code " + std::to_string(status_code);
        }

        // Read the response headers, which are terminated by a blank line.
        boost::asio::read_until(ssock, response, "\r\n\r\n");

        std::string header;
        while (std::getline(response_stream, header) && header != "\r") {
            // cout << header << endl;
        }

        // Write whatever content we already have to output.
        std::ostringstream ostringstream_content;
        if (response.size() > 0) {
            ostringstream_content << &response;
        }

        // Read until EOF, writing data to output as we go.
        boost::system::error_code error;
        while (true) {
            size_t n = boost::asio::read(ssock, response, boost::asio::transfer_at_least(1), error);
            if (!error) {
                if (n) {
                    ostringstream_content << &response;
                }
            }
            if (error == boost::asio::error::eof) {
                break;
            }
            if (error) {
                std::string errorMsg = error.message();
                errors = errorMsg + " " + std::to_string(error.value());
                break;
            }
        }
        // read response into Univalue Obj
        UniValue obj_response(UniValue::VOBJ);

        auto str_response = ostringstream_content.str();

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
