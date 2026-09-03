// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>

#include <clientversion.h>
#include <node/release_artifacts.h>
#include <node/ca_store.h>
#include <rpc/util.h>
#include <rpc/semver.h>
#include <shutdown.h>
#include <sync.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/signals2/signal.hpp>

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
#include <boost/asio/ssl.hpp>
#include <boost/asio/ip/tcp.hpp>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <boost/assign/list_of.hpp>

#include <openssl/ssl.h>
#include <openssl/tls1.h>

using boost::asio::ip::tcp;

#include <cassert>
#include <memory> // for unique_ptr
#include <stdexcept>
#include <mutex>
#include <regex>
#include <unordered_map>

static Mutex g_rpc_warmup_mutex;
static std::atomic<bool> g_rpc_running{false};
static bool fRPCInWarmup GUARDED_BY(g_rpc_warmup_mutex) = true;
static std::string rpcWarmupStatus GUARDED_BY(g_rpc_warmup_mutex) = "RPC server started";
/* Timer-creating functions */
static RPCTimerInterface* timerInterface = nullptr;
/* Map of name to timer. */
static Mutex g_deadline_timers_mutex;
static std::map<std::string, std::unique_ptr<RPCTimerBase> > deadlineTimers GUARDED_BY(g_deadline_timers_mutex);
static bool ExecuteCommand(const CRPCCommand& command, const JSONRPCRequest& request, UniValue& result, bool last_handler);

static std::string strDownloadLink = "https://download.reddcoin.com/bin/reddcoin-core-";
static std::string strGithubLink = "/repos/reddcoin-project/reddcoin/releases/latest";
//! Named once so the resolver, the SNI extension, the certificate check and the
//! Host header cannot drift apart from one another.
static const std::string strGithubHost = "api.github.com";

struct RPCCommandExecutionInfo
{
    std::string method;
    int64_t start;
};

struct RPCServerInfo
{
    Mutex mutex;
    std::list<RPCCommandExecutionInfo> active_commands GUARDED_BY(mutex);
};

static RPCServerInfo g_rpc_server_info;

struct RPCCommandExecution
{
    std::list<RPCCommandExecutionInfo>::iterator it;
    explicit RPCCommandExecution(const std::string& method)
    {
        LOCK(g_rpc_server_info.mutex);
        it = g_rpc_server_info.active_commands.insert(g_rpc_server_info.active_commands.end(), {method, GetTimeMicros()});
    }
    ~RPCCommandExecution()
    {
        LOCK(g_rpc_server_info.mutex);
        g_rpc_server_info.active_commands.erase(it);
    }
};

static struct CRPCSignals
{
    boost::signals2::signal<void ()> Started;
    boost::signals2::signal<void ()> Stopped;
} g_rpcSignals;

void RPCServer::OnStarted(std::function<void ()> slot)
{
    g_rpcSignals.Started.connect(slot);
}

void RPCServer::OnStopped(std::function<void ()> slot)
{
    g_rpcSignals.Stopped.connect(slot);
}

std::string CRPCTable::help(const std::string& strCommand, const JSONRPCRequest& helpreq) const
{
    std::string strRet;
    std::string category;
    std::set<intptr_t> setDone;
    std::vector<std::pair<std::string, const CRPCCommand*> > vCommands;

    for (const auto& entry : mapCommands)
        vCommands.push_back(make_pair(entry.second.front()->category + entry.first, entry.second.front()));
    sort(vCommands.begin(), vCommands.end());

    JSONRPCRequest jreq = helpreq;
    jreq.mode = JSONRPCRequest::GET_HELP;
    jreq.params = UniValue();

    for (const std::pair<std::string, const CRPCCommand*>& command : vCommands)
    {
        const CRPCCommand *pcmd = command.second;
        std::string strMethod = pcmd->name;
        if ((strCommand != "" || pcmd->category == "hidden") && strMethod != strCommand)
            continue;
        jreq.strMethod = strMethod;
        try
        {
            UniValue unused_result;
            if (setDone.insert(pcmd->unique_id).second)
                pcmd->actor(jreq, unused_result, true /* last_handler */);
        }
        catch (const std::exception& e)
        {
            // Help text is returned in an exception
            std::string strHelp = std::string(e.what());
            if (strCommand == "")
            {
                if (strHelp.find('\n') != std::string::npos)
                    strHelp = strHelp.substr(0, strHelp.find('\n'));

                if (category != pcmd->category)
                {
                    if (!category.empty())
                        strRet += "\n";
                    category = pcmd->category;
                    strRet += "== " + Capitalize(category) + " ==\n";
                }
            }
            strRet += strHelp + "\n";
        }
    }
    if (strRet == "")
        strRet = strprintf("help: unknown command: %s\n", strCommand);
    strRet = strRet.substr(0,strRet.size()-1);
    return strRet;
}

static RPCHelpMan help()
{
    return RPCHelpMan{"help",
                "\nList all commands, or get help for a specified command.\n",
                {
                    {"command", RPCArg::Type::STR, RPCArg::DefaultHint{"all commands"}, "The command to get help on"},
                },
                {
                    RPCResult{RPCResult::Type::STR, "", "The help text"},
                    RPCResult{RPCResult::Type::ANY, "", ""},
                },
                RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& jsonRequest) -> UniValue
{
    std::string strCommand;
    if (jsonRequest.params.size() > 0) {
        strCommand = jsonRequest.params[0].get_str();
    }
    if (strCommand == "dump_all_command_conversions") {
        // Used for testing only, undocumented
        return tableRPC.dumpArgMap(jsonRequest);
    }

    return tableRPC.help(strCommand, jsonRequest);
},
    };
}

static RPCHelpMan stop()
{
    static const std::string RESULT{PACKAGE_NAME " stopping"};
    return RPCHelpMan{"stop",
    // Also accept the hidden 'wait' integer argument (milliseconds)
    // For instance, 'stop 1000' makes the call wait 1 second before returning
    // to the client (intended for testing)
                "\nRequest a graceful shutdown of " PACKAGE_NAME ".",
                {
                    {"wait", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "how long to wait in ms", "", {}, /* hidden */ true},
                },
                RPCResult{RPCResult::Type::STR, "", "A string with the content '" + RESULT + "'"},
                RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& jsonRequest) -> UniValue
{
    // Event loop will exit after current HTTP requests have been handled, so
    // this reply will get back to the client.
    StartShutdown();
    if (jsonRequest.params[0].isNum()) {
        UninterruptibleSleep(std::chrono::milliseconds{jsonRequest.params[0].get_int()});
    }
    return RESULT;
},
    };
}

static RPCHelpMan uptime()
{
    return RPCHelpMan{"uptime",
                "\nReturns the total uptime of the server.\n",
                            {},
                            RPCResult{
                                RPCResult::Type::NUM, "", "The number of seconds that the server has been running"
                            },
                RPCExamples{
                    HelpExampleCli("uptime", "")
                + HelpExampleRpc("uptime", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    return GetTime() - GetStartupTime();
}
    };
}

static RPCHelpMan getrpcinfo()
{
    return RPCHelpMan{"getrpcinfo",
                "\nReturns details of the RPC server.\n",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::ARR, "active_commands", "All active commands",
                        {
                            {RPCResult::Type::OBJ, "", "Information about an active command",
                            {
                                 {RPCResult::Type::STR, "method", "The name of the RPC command"},
                                 {RPCResult::Type::NUM, "duration", "The running time in microseconds"},
                            }},
                        }},
                        {RPCResult::Type::STR, "logpath", "The complete file path to the debug log"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("getrpcinfo", "")
                + HelpExampleRpc("getrpcinfo", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    LOCK(g_rpc_server_info.mutex);
    UniValue active_commands(UniValue::VARR);
    for (const RPCCommandExecutionInfo& info : g_rpc_server_info.active_commands) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("method", info.method);
        entry.pushKV("duration", GetTimeMicros() - info.start);
        active_commands.push_back(entry);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("active_commands", active_commands);

    const std::string path = LogInstance().m_file_path.string();
    UniValue log_path(UniValue::VSTR, path);
    result.pushKV("logpath", log_path);

    return result;
}
    };
}

static RPCHelpMan checkupdates()
{
    return RPCHelpMan{"checkupdates",
        "\nReturns details of the latest software update available.\n",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "", {
                {RPCResult::Type::NUM, "localversion", "The local version number"},
                {RPCResult::Type::NUM, "remoteversion", "The remote version number"},
                {RPCResult::Type::BOOL, "updateavailable", "Is a remote update available"},
                {RPCResult::Type::STR, "message", "Message confirming if you are on latest release version and where to download the latest version from"},
                {RPCResult::Type::STR, "warning", "Any warning messages"},
                {RPCResult::Type::STR, "officialDownloadLink", "Official direct download link"},
                {RPCResult::Type::STR, "hosttriplet", "The host triplet this build targets, empty if the build did not record one"},
                {RPCResult::Type::STR, "platform", "Short platform name used in artifact filenames, empty if no build is published for this host"},
                {RPCResult::Type::STR, "guiartifact", "Filename of the build a reddcoin-qt user should install, empty if unknown"},
                {RPCResult::Type::STR, "guiartifactlink", "Direct download link for guiartifact, empty if unknown"},
                {RPCResult::Type::STR, "daemonartifact", "Filename of the build a reddcoind user should install, empty if unknown. Equal to guiartifact on Linux, where one tarball carries both"},
                {RPCResult::Type::STR, "daemonartifactlink", "Direct download link for daemonartifact, empty if unknown"},
                {RPCResult::Type::STR, "errors", "Any error messages"},
            }},
        RPCExamples{HelpExampleCli("checkupdates", "") + HelpExampleRpc("checkupdates", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{

    UniValue result(UniValue::VOBJ);
    checkforupdatesinfo(result);
    return result;
}
    };
}

void checkforupdatesinfo(UniValue& result)
{
    std::string installedVersion = PACKAGE_VERSION;
    std::string repositoryVersion = "";
    std::string localVersion = "";
    std::string remoteVersion = "";
    bool updateAvailable = false;
    std::string message = "";
    std::string warning = "";
    std::string officialDownloadLink = "";
    std::string platform = "";
    std::string guiArtifact = "";
    std::string guiArtifactLink = "";
    std::string daemonArtifact = "";
    std::string daemonArtifactLink = "";
    std::string errors = "";

    try {
        boost::asio::io_service svc;

        // tls_client rather than the sslv23_client this used to ask for. Both
        // negotiate the best protocol both ends support, but the sslv23 spelling
        // names methods the 3.x series deprecated.
        boost::asio::ssl::context ctx(boost::asio::ssl::context::method::tls_client);

        // Without this the connection is encrypted but unauthenticated: any
        // party able to intercept it can present any certificate and substitute
        // the response. The most useful thing that buys an attacker is silently
        // suppressing upgrade notices, which is the wrong failure mode for the
        // mechanism whose job is to get security fixes onto user machines.
        //
        // Failing to obtain trust anchors is fatal to the fetch rather than a
        // downgrade to an unverified one. The catch below reports it like any
        // other failure.
        const std::string ca_error = node::LoadTrustedCACertificates(ctx.native_handle());
        if (!ca_error.empty()) throw std::runtime_error(ca_error);

        // verify_peer rejects a certificate that does not chain to one of those
        // anchors; the callback additionally binds the certificate to the host
        // that was asked for, so a valid certificate for some other name is no
        // use. boost 1.71 spells this rfc2818_verification; the
        // host_name_verification that replaced it arrived in 1.73.
        ctx.set_verify_mode(boost::asio::ssl::verify_peer);
        ctx.set_verify_callback(boost::asio::ssl::rfc2818_verification(strGithubHost));

        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> ssock(svc, ctx);

        // Server Name Indication. Without it a host that serves several names
        // from one address, which includes anything behind a CDN, cannot tell
        // which certificate to present and may not serve the request at all.
        // This is a functional fix rather than a security one: the certificate
        // is checked against strGithubHost above regardless of what SNI asked
        // for.
        if (SSL_set_tlsext_host_name(ssock.native_handle(), strGithubHost.c_str()) != 1) {
            throw std::runtime_error("Could not set the TLS server name for " + strGithubHost);
        }

        boost::asio::ip::tcp::resolver resolver(svc);
        boost::asio::ip::tcp::resolver::query query(strGithubHost, "https");
        boost::asio::ip::tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);

        // Establish a connection.
        boost::asio::connect(ssock.lowest_layer(), endpoint_iterator);
        ssock.handshake(boost::asio::ssl::stream_base::handshake_type::client);

        // Send request
        boost::asio::streambuf request;
        std::ostream request_stream(&request);
        request_stream << "GET " << strGithubLink << " HTTP/1.1\r\n"; // note that you can change it if you wish to HTTP/1.0
        request_stream << "Host: " << strGithubHost << "\r\n";
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
                    node::ReleaseArtifacts artifacts;
                    if (node::GetReleaseArtifactsForThisHost(remote.numeric, artifacts)) {
                        platform = artifacts.platform;
                        guiArtifact = artifacts.gui;
                        daemonArtifact = artifacts.daemon;
                        guiArtifactLink = officialDownloadLink + "/" + artifacts.gui;
                        daemonArtifactLink = officialDownloadLink + "/" + artifacts.daemon;
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
    result.pushKV("hosttriplet", node::HostTriplet());
    result.pushKV("platform", platform);
    result.pushKV("guiartifact", guiArtifact);
    result.pushKV("guiartifactlink", guiArtifactLink);
    result.pushKV("daemonartifact", daemonArtifact);
    result.pushKV("daemonartifactlink", daemonArtifactLink);
    result.pushKV("errors", errors);
}

// clang-format off
static const CRPCCommand vRPCCommands[] =
{ //  category               actor (function)
  //  ---------------------  -----------------------
    /* Overall control/query calls */
    { "control",             &getrpcinfo,             },
    { "control",             &help,                   },
    { "control",             &stop,                   },
    { "control",             &uptime,                 },
    { "control",             &checkupdates,           },
};
// clang-format on

CRPCTable::CRPCTable()
{
    for (const auto& c : vRPCCommands) {
        appendCommand(c.name, &c);
    }
}

void CRPCTable::appendCommand(const std::string& name, const CRPCCommand* pcmd)
{
    CHECK_NONFATAL(!IsRPCRunning()); // Only add commands before rpc is running

    mapCommands[name].push_back(pcmd);
}

bool CRPCTable::removeCommand(const std::string& name, const CRPCCommand* pcmd)
{
    auto it = mapCommands.find(name);
    if (it != mapCommands.end()) {
        auto new_end = std::remove(it->second.begin(), it->second.end(), pcmd);
        if (it->second.end() != new_end) {
            it->second.erase(new_end, it->second.end());
            return true;
        }
    }
    return false;
}

void StartRPC()
{
    LogPrint(BCLog::RPC, "Starting RPC\n");
    g_rpc_running = true;
    g_rpcSignals.Started();
}

void InterruptRPC()
{
    static std::once_flag g_rpc_interrupt_flag;
    // This function could be called twice if the GUI has been started with -server=1.
    std::call_once(g_rpc_interrupt_flag, []() {
        LogPrint(BCLog::RPC, "Interrupting RPC\n");
        // Interrupt e.g. running longpolls
        g_rpc_running = false;
    });
}

void StopRPC()
{
    static std::once_flag g_rpc_stop_flag;
    // This function could be called twice if the GUI has been started with -server=1.
    assert(!g_rpc_running);
    std::call_once(g_rpc_stop_flag, []() {
        LogPrint(BCLog::RPC, "Stopping RPC\n");
        WITH_LOCK(g_deadline_timers_mutex, deadlineTimers.clear());
        DeleteAuthCookie();
        g_rpcSignals.Stopped();
    });
}

bool IsRPCRunning()
{
    return g_rpc_running;
}

void RpcInterruptionPoint()
{
    if (!IsRPCRunning()) throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
}

void SetRPCWarmupStatus(const std::string& newStatus)
{
    LOCK(g_rpc_warmup_mutex);
    rpcWarmupStatus = newStatus;
}

void SetRPCWarmupFinished()
{
    LOCK(g_rpc_warmup_mutex);
    assert(fRPCInWarmup);
    fRPCInWarmup = false;
}

bool RPCIsInWarmup(std::string *outStatus)
{
    LOCK(g_rpc_warmup_mutex);
    if (outStatus)
        *outStatus = rpcWarmupStatus;
    return fRPCInWarmup;
}

bool IsDeprecatedRPCEnabled(const std::string& method)
{
    const std::vector<std::string> enabled_methods = gArgs.GetArgs("-deprecatedrpc");

    return find(enabled_methods.begin(), enabled_methods.end(), method) != enabled_methods.end();
}

static UniValue JSONRPCExecOne(JSONRPCRequest jreq, const UniValue& req)
{
    UniValue rpc_result(UniValue::VOBJ);

    try {
        jreq.parse(req);

        UniValue result = tableRPC.execute(jreq);
        rpc_result = JSONRPCReplyObj(result, NullUniValue, jreq.id);
    }
    catch (const UniValue& objError)
    {
        rpc_result = JSONRPCReplyObj(NullUniValue, objError, jreq.id);
    }
    catch (const std::exception& e)
    {
        rpc_result = JSONRPCReplyObj(NullUniValue,
                                     JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
    }

    return rpc_result;
}

std::string JSONRPCExecBatch(const JSONRPCRequest& jreq, const UniValue& vReq)
{
    UniValue ret(UniValue::VARR);
    for (unsigned int reqIdx = 0; reqIdx < vReq.size(); reqIdx++)
        ret.push_back(JSONRPCExecOne(jreq, vReq[reqIdx]));

    return ret.write() + "\n";
}

/**
 * Process named arguments into a vector of positional arguments, based on the
 * passed-in specification for the RPC call's arguments.
 */
static inline JSONRPCRequest transformNamedArguments(const JSONRPCRequest& in, const std::vector<std::string>& argNames)
{
    JSONRPCRequest out = in;
    out.params = UniValue(UniValue::VARR);
    // Build a map of parameters, and remove ones that have been processed, so that we can throw a focused error if
    // there is an unknown one.
    const std::vector<std::string>& keys = in.params.getKeys();
    const std::vector<UniValue>& values = in.params.getValues();
    std::unordered_map<std::string, const UniValue*> argsIn;
    for (size_t i=0; i<keys.size(); ++i) {
        argsIn[keys[i]] = &values[i];
    }
    // Process expected parameters.
    int hole = 0;
    for (const std::string &argNamePattern: argNames) {
        std::vector<std::string> vargNames;
        boost::algorithm::split(vargNames, argNamePattern, boost::algorithm::is_any_of("|"));
        auto fr = argsIn.end();
        for (const std::string & argName : vargNames) {
            fr = argsIn.find(argName);
            if (fr != argsIn.end()) {
                break;
            }
        }
        if (fr != argsIn.end()) {
            for (int i = 0; i < hole; ++i) {
                // Fill hole between specified parameters with JSON nulls,
                // but not at the end (for backwards compatibility with calls
                // that act based on number of specified parameters).
                out.params.push_back(UniValue());
            }
            hole = 0;
            out.params.push_back(*fr->second);
            argsIn.erase(fr);
        } else {
            hole += 1;
        }
    }
    // If there are still arguments in the argsIn map, this is an error.
    if (!argsIn.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown named parameter " + argsIn.begin()->first);
    }
    // Return request with named arguments transformed to positional arguments
    return out;
}

static bool ExecuteCommands(const std::vector<const CRPCCommand*>& commands, const JSONRPCRequest& request, UniValue& result)
{
    for (const auto& command : commands) {
        if (ExecuteCommand(*command, request, result, &command == &commands.back())) {
            return true;
        }
    }
    return false;
}

UniValue CRPCTable::execute(const JSONRPCRequest &request) const
{
    // Return immediately if in warmup
    {
        LOCK(g_rpc_warmup_mutex);
        if (fRPCInWarmup)
            throw JSONRPCError(RPC_IN_WARMUP, rpcWarmupStatus);
    }

    // Find method
    auto it = mapCommands.find(request.strMethod);
    if (it != mapCommands.end()) {
        UniValue result;
        if (ExecuteCommands(it->second, request, result)) {
            return result;
        }
    }
    throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found");
}

static bool ExecuteCommand(const CRPCCommand& command, const JSONRPCRequest& request, UniValue& result, bool last_handler)
{
    try
    {
        RPCCommandExecution execution(request.strMethod);
        // Execute, convert arguments to array if necessary
        if (request.params.isObject()) {
            return command.actor(transformNamedArguments(request, command.argNames), result, last_handler);
        } else {
            return command.actor(request, result, last_handler);
        }
    }
    catch (const std::exception& e)
    {
        throw JSONRPCError(RPC_MISC_ERROR, e.what());
    }
}

std::vector<std::string> CRPCTable::listCommands() const
{
    std::vector<std::string> commandList;
    for (const auto& i : mapCommands) commandList.emplace_back(i.first);
    return commandList;
}

UniValue CRPCTable::dumpArgMap(const JSONRPCRequest& args_request) const
{
    JSONRPCRequest request = args_request;
    request.mode = JSONRPCRequest::GET_ARGS;

    UniValue ret{UniValue::VARR};
    for (const auto& cmd : mapCommands) {
        UniValue result;
        if (ExecuteCommands(cmd.second, request, result)) {
            for (const auto& values : result.getValues()) {
                ret.push_back(values);
            }
        }
    }
    return ret;
}

void RPCSetTimerInterfaceIfUnset(RPCTimerInterface *iface)
{
    if (!timerInterface)
        timerInterface = iface;
}

void RPCSetTimerInterface(RPCTimerInterface *iface)
{
    timerInterface = iface;
}

void RPCUnsetTimerInterface(RPCTimerInterface *iface)
{
    if (timerInterface == iface)
        timerInterface = nullptr;
}

void RPCRunLater(const std::string& name, std::function<void()> func, int64_t nSeconds)
{
    if (!timerInterface)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "No timer handler registered for RPC");
    LOCK(g_deadline_timers_mutex);
    deadlineTimers.erase(name);
    LogPrint(BCLog::RPC, "queue run of timer %s in %i seconds (using %s)\n", name, nSeconds, timerInterface->Name());
    deadlineTimers.emplace(name, std::unique_ptr<RPCTimerBase>(timerInterface->NewTimer(func, nSeconds*1000)));
}

int RPCSerializationFlags()
{
    int flag = 0;
    if (gArgs.GetArg("-rpcserialversion", DEFAULT_RPC_SERIALIZE_VERSION) == 0)
        flag |= SERIALIZE_TRANSACTION_NO_WITNESS;
    return flag;
}

CRPCTable tableRPC;
