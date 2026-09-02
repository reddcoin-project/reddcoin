// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/update_check.h>

#include <test/util/setup_common.h>
#include <util/string.h>

#include <boost/test/unit_test.hpp>

#include <stdexcept>
#include <string>

using node::ExtractHttpBody;

namespace {
//! The body every positive case expects back, shaped like the response the
//! update check actually parses.
const std::string BODY{"{\"tag_name\":\"v4.22.9.4\"}"};

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

BOOST_AUTO_TEST_CASE(chunked_encoding_is_rejected)
{
    // Chunk sizes are interleaved with the content and are not decoded here,
    // so the body must not be handed back as if it were the payload.
    const std::string response{
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "17\r\n" +
        BODY + "\r\n0\r\n\r\n"};

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

BOOST_AUTO_TEST_SUITE_END()
