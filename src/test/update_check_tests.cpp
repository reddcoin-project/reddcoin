// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/update_check.h>

#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/string.h>

#include <boost/test/unit_test.hpp>

#include <stdexcept>
#include <string>

using node::ExtractHttpBody;
using node::HttpResponse;
using node::ParseHttpResponse;
using node::ResolveRedirect;
using node::Url;

namespace {
//! The body every positive case expects back, shaped like the response the
//! update check actually parses.
const std::string BODY{"{\"tag_name\":\"v4.22.9.4\"}"};

//! Chunk sizes are hexadecimal.
std::string ToHex(std::string::size_type n)
{
    return strprintf("%x", n);
}

std::string WithLength(const std::string& body, const std::string& declared_length)
{
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: " +
           declared_length + "\r\n\r\n" + body;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(update_check_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(complete_body_is_returned)
{
    BOOST_CHECK_EQUAL(ExtractHttpBody(WithLength(BODY, ToString(BODY.size())), true), BODY);

    // A truncated stream is fine when Content-Length accounts for every byte:
    // plenty of servers close without a TLS shutdown.
    BOOST_CHECK_EQUAL(ExtractHttpBody(WithLength(BODY, ToString(BODY.size())), false), BODY);
}

BOOST_AUTO_TEST_CASE(short_body_is_rejected)
{
    // The regression this guards. Before the completeness check a body cut off
    // mid-flight was returned as a successful result, so the caller saw a well
    // formed but empty answer and reported no error at all: an upgrade notice
    // that silently never appears.
    const std::string cut{BODY.substr(0, BODY.size() - 5)};
    const std::string response{WithLength(cut, ToString(BODY.size()))};

    BOOST_CHECK_THROW(ExtractHttpBody(response, false), std::runtime_error);
    // Clean shutdown or not, a short body is still short.
    BOOST_CHECK_THROW(ExtractHttpBody(response, true), std::runtime_error);

    // The error has to name the shortfall, otherwise a truncated download is
    // indistinguishable from an unreachable server in the logs.
    try {
        ExtractHttpBody(response, false);
        BOOST_ERROR("expected a throw");
    } catch (const std::runtime_error& e) {
        const std::string what{e.what()};
        BOOST_CHECK(what.find("Incomplete response") != std::string::npos);
        BOOST_CHECK(what.find(ToString(cut.size())) != std::string::npos);
        BOOST_CHECK(what.find(ToString(BODY.size())) != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(overlong_body_is_rejected)
{
    BOOST_CHECK_THROW(ExtractHttpBody(WithLength(BODY + "trailing", ToString(BODY.size())), true),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(unusable_content_length_is_rejected)
{
    BOOST_CHECK_THROW(ExtractHttpBody(WithLength(BODY, "not-a-number"), true), std::runtime_error);
    BOOST_CHECK_THROW(ExtractHttpBody(WithLength(BODY, "-1"), true), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(connection_close_framing_needs_a_clean_shutdown)
{
    // No Content-Length, so the body runs until the connection closes and only
    // a clean shutdown proves all of it arrived.
    const std::string response{"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n" + BODY};

    BOOST_CHECK_EQUAL(ExtractHttpBody(response, true), BODY);
    BOOST_CHECK_THROW(ExtractHttpBody(response, false), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(chunked_body_is_reassembled)
{
    // api.github.com answers with Content-Length most of the time and switches
    // to chunked intermittently, so both have to work. Before this was decoded
    // the chunk sizes were handed back as part of the body, the JSON parse
    // failed, and the update check reported nothing at all.
    const std::string head{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"};

    // One chunk holding the whole body.
    const std::string single{head + ToHex(BODY.size()) + "\r\n" + BODY + "\r\n0\r\n\r\n"};
    BOOST_CHECK_EQUAL(ExtractHttpBody(single, true), BODY);

    // Split across chunks, which is the shape a real server sends.
    const std::string first{BODY.substr(0, 8)};
    const std::string rest{BODY.substr(8)};
    const std::string split{head + ToHex(first.size()) + "\r\n" + first + "\r\n" +
                            ToHex(rest.size()) + "\r\n" + rest + "\r\n0\r\n\r\n"};
    BOOST_CHECK_EQUAL(ExtractHttpBody(split, true), BODY);

    // Chunk extensions are ignored, and trailers after the final chunk too.
    const std::string extras{head + ToHex(BODY.size()) + ";name=value\r\n" + BODY +
                             "\r\n0\r\nX-Trailer: ignored\r\n\r\n"};
    BOOST_CHECK_EQUAL(ExtractHttpBody(extras, true), BODY);

    // Case of the header value must not decide whether it is decoded.
    const std::string upper{"HTTP/1.1 200 OK\r\nTransfer-Encoding: CHUNKED\r\n\r\n" +
                            ToHex(BODY.size()) + "\r\n" + BODY + "\r\n0\r\n\r\n"};
    BOOST_CHECK_EQUAL(ExtractHttpBody(upper, true), BODY);
}

BOOST_AUTO_TEST_CASE(incomplete_chunked_body_is_rejected)
{
    const std::string head{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"};

    // The terminating zero length chunk never arrived, so the body is short
    // even though everything received parsed cleanly.
    const std::string no_last_chunk{head + ToHex(BODY.size()) + "\r\n" + BODY + "\r\n"};
    BOOST_CHECK_THROW(ExtractHttpBody(no_last_chunk, true), std::runtime_error);

    // Cut off partway through a chunk's data.
    const std::string mid_chunk{head + ToHex(BODY.size()) + "\r\n" + BODY.substr(0, 6)};
    BOOST_CHECK_THROW(ExtractHttpBody(mid_chunk, true), std::runtime_error);

    // A chunk whose declared size overruns what is present.
    const std::string overrun{head + "ffff\r\n" + BODY + "\r\n0\r\n\r\n"};
    BOOST_CHECK_THROW(ExtractHttpBody(overrun, true), std::runtime_error);

    // Size field that is not hexadecimal.
    const std::string bad_size{head + "zz\r\n" + BODY + "\r\n0\r\n\r\n"};
    BOOST_CHECK_THROW(ExtractHttpBody(bad_size, true), std::runtime_error);

    // Chunk data not followed by its CRLF.
    const std::string bad_terminator{head + ToHex(BODY.size()) + "\r\n" + BODY + "XX0\r\n\r\n"};
    BOOST_CHECK_THROW(ExtractHttpBody(bad_terminator, true), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(other_transfer_encodings_are_rejected)
{
    // Only chunked is decoded. Anything else must not be passed off as a body.
    const std::string response{"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n" + BODY};
    BOOST_CHECK_THROW(ExtractHttpBody(response, true), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(headers_match_case_insensitively)
{
    // RFC 7230 field names are case insensitive, and the casing a server picks
    // must not decide whether the length is checked at all.
    const std::string lower{"HTTP/1.1 200 OK\r\ncontent-length: " + ToString(BODY.size()) + "\r\n\r\n" + BODY};
    const std::string upper{"HTTP/1.1 200 OK\r\nCONTENT-LENGTH: " + ToString(BODY.size()) + "\r\n\r\n" + BODY};
    BOOST_CHECK_EQUAL(ExtractHttpBody(lower, false), BODY);
    BOOST_CHECK_EQUAL(ExtractHttpBody(upper, false), BODY);

    const std::string short_lower{"HTTP/1.1 200 OK\r\ncontent-length: 999\r\n\r\n" + BODY};
    BOOST_CHECK_THROW(ExtractHttpBody(short_lower, false), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(header_name_inside_a_value_is_not_matched)
{
    // "content-length" appears in another header's value. Matching the block
    // rather than each field name would pick it up and check the wrong number.
    const std::string response{
        "HTTP/1.1 200 OK\r\n"
        "X-Note: content-length: 99999\r\n\r\n" +
        BODY};

    // No real Content-Length, so this falls through to the clean shutdown rule.
    BOOST_CHECK_EQUAL(ExtractHttpBody(response, true), BODY);
    BOOST_CHECK_THROW(ExtractHttpBody(response, false), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(malformed_responses_are_rejected)
{
    BOOST_CHECK_THROW(ExtractHttpBody("", true), std::runtime_error);
    BOOST_CHECK_THROW(ExtractHttpBody("not http at all\r\n\r\n", true), std::runtime_error);
    // Headers never terminated.
    BOOST_CHECK_THROW(ExtractHttpBody("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n", true), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(non_200_status_is_rejected)
{
    for (const std::string& status : {"404 Not Found", "403 Forbidden", "500 Internal Server Error"}) {
        const std::string response{"HTTP/1.1 " + status + "\r\nContent-Length: 0\r\n\r\n"};
        BOOST_CHECK_THROW(ExtractHttpBody(response, true), std::runtime_error);
    }
}

//! Redirects, added with phase 3a. The fetcher follows them now, so where a
//! Location points is a decision the client makes and therefore one to pin down.

BOOST_AUTO_TEST_CASE(an_absolute_redirect_changes_host_and_target)
{
    const Url next{ResolveRedirect("api.github.com", "/repos/x/releases/latest",
                                   "https://download.reddcoin.com/bin/file.tar.gz")};
    BOOST_CHECK_EQUAL(next.host, "download.reddcoin.com");
    BOOST_CHECK_EQUAL(next.target, "/bin/file.tar.gz");
}

BOOST_AUTO_TEST_CASE(an_absolute_path_keeps_the_host)
{
    const Url next{ResolveRedirect("download.reddcoin.com", "/bin/old/file", "/bin/new/file")};
    BOOST_CHECK_EQUAL(next.host, "download.reddcoin.com");
    BOOST_CHECK_EQUAL(next.target, "/bin/new/file");
}

BOOST_AUTO_TEST_CASE(a_relative_redirect_resolves_against_the_directory)
{
    // Relative to the directory of the current target, not to its full path,
    // which would otherwise produce /bin/reddcoin-core-4.22.9.4/SHA256SUMSother.
    const Url next{ResolveRedirect("download.reddcoin.com",
                                   "/bin/reddcoin-core-4.22.9.4/SHA256SUMS", "SHA256SUMS.sig")};
    BOOST_CHECK_EQUAL(next.target, "/bin/reddcoin-core-4.22.9.4/SHA256SUMS.sig");
}

BOOST_AUTO_TEST_CASE(a_protocol_relative_redirect_stays_on_https)
{
    const Url next{ResolveRedirect("api.github.com", "/x", "//example.org/y")};
    BOOST_CHECK_EQUAL(next.host, "example.org");
    BOOST_CHECK_EQUAL(next.target, "/y");
}

BOOST_AUTO_TEST_CASE(a_redirect_out_of_https_is_refused)
{
    // The one that matters. Following this would discard the certificate
    // verification the fetcher exists to perform, and a redirect is exactly
    // where an attacker would try to introduce it.
    BOOST_CHECK_THROW(ResolveRedirect("api.github.com", "/x", "http://example.org/y"),
                      std::runtime_error);
    BOOST_CHECK_THROW(ResolveRedirect("api.github.com", "/x", "ftp://example.org/y"),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(a_redirect_to_another_port_is_refused)
{
    // The port would have to be carried through to the connect. Ignoring it
    // would connect somewhere other than where the server asked.
    BOOST_CHECK_THROW(ResolveRedirect("api.github.com", "/x", "https://example.org:8443/y"),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(a_malformed_redirect_is_refused)
{
    BOOST_CHECK_THROW(ResolveRedirect("api.github.com", "/x", ""), std::runtime_error);
    BOOST_CHECK_THROW(ResolveRedirect("api.github.com", "/x", "https:///y"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(a_redirect_response_is_parsed_rather_than_rejected)
{
    // ExtractHttpBody throws on any non-200, which is right for a caller that
    // wants a body. The fetcher needs to see the status and Location instead.
    const std::string raw{
        "HTTP/1.1 302 Found\r\n"
        "Location: https://download.reddcoin.com/bin/file\r\n"
        "\r\n"};

    const HttpResponse response{ParseHttpResponse(raw, false)};
    BOOST_CHECK_EQUAL(response.status, 302U);
    BOOST_CHECK_EQUAL(response.location, "https://download.reddcoin.com/bin/file");

    BOOST_CHECK_THROW(ExtractHttpBody(raw, false), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(a_redirect_body_is_not_held_to_the_framing_rules)
{
    // No Content-Length and no clean shutdown. For a 200 that is a truncated
    // response and must be rejected; for a redirect the body is discarded, so
    // failing here would reject a valid redirect over bytes nothing reads.
    const std::string redirect{
        "HTTP/1.1 301 Moved Permanently\r\n"
        "Location: /elsewhere\r\n"
        "\r\n"
        "partial"};
    BOOST_CHECK_NO_THROW(ParseHttpResponse(redirect, false));

    const std::string ok{
        "HTTP/1.1 200 OK\r\n"
        "\r\n"
        "partial"};
    BOOST_CHECK_THROW(ParseHttpResponse(ok, false), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
