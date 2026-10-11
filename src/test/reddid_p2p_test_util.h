// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_REDDID_P2P_TEST_UTIL_H
#define BITCOIN_TEST_REDDID_P2P_TEST_UTIL_H

#include <id/namespace.h>
#include <id/reddid.h>
#include <id/reddid_p2p.h>
#include <net.h>
#include <net_processing.h>
#include <netaddress.h>
#include <protocol.h>
#include <serialize.h>
#include <streams.h>
#include <sync.h>
#include <test/reddid_profile_test_util.h>
#include <test/util/logging.h>
#include <test/util/net.h>
#include <uint256.h>
#include <util/time.h>
#include <version.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

/** Helpers shared by the unit tests of the ReddID peer-to-peer code. */
namespace reddid_test {

/** Counts the log lines that contain the message during the lifetime of the object. */
struct LogCounter {
    int count{0};
    DebugLogHelper helper;

    explicit LogCounter(const std::string& message)
        : helper(message, [this](const std::string* line) {
              if (line) ++count;
              return false;
          }) {}
};

/** Keeps every log line written during the lifetime of the object. */
struct LogCapture {
    std::vector<std::string> lines;
    DebugLogHelper helper;

    LogCapture()
        : helper("", [this](const std::string* line) {
              if (line) lines.push_back(*line);
              return false;
          }) {}

    int Count(const std::string& message) const
    {
        int count = 0;
        for (const std::string& line : lines) {
            if (line.find(message) != std::string::npos) ++count;
        }
        return count;
    }

    bool Has(const std::string& message) const { return Count(message) > 0; }
};

/** What ReddIDP2PManager::ProcessMessage did with a message, as far as its log tells. */
enum class Outcome {
    NOT_RUNNING,   //!< the manager was not running
    INCOMPATIBLE,  //!< the peer does not advertise NODE_REDDID
    EMPTY,         //!< the payload was empty
    OVERSIZED,     //!< the payload was larger than MAX_REDDID_MESSAGE_SIZE
    RATE_LIMITED,  //!< the rate limiter refused the message
    UNKNOWN,       //!< no handler is registered for the command
    REJECTED,      //!< the handler returned false
    ACCEPTED,      //!< the handler returned true
};

inline std::ostream& operator<<(std::ostream& os, Outcome outcome)
{
    switch (outcome) {
    case Outcome::NOT_RUNNING: return os << "NOT_RUNNING";
    case Outcome::INCOMPATIBLE: return os << "INCOMPATIBLE";
    case Outcome::EMPTY: return os << "EMPTY";
    case Outcome::OVERSIZED: return os << "OVERSIZED";
    case Outcome::RATE_LIMITED: return os << "RATE_LIMITED";
    case Outcome::UNKNOWN: return os << "UNKNOWN";
    case Outcome::REJECTED: return os << "REJECTED";
    case Outcome::ACCEPTED: return os << "ACCEPTED";
    }
    return os << "?";
}

/** A message that was queued for a peer. */
struct SentMessage {
    std::string command;
    std::vector<unsigned char> payload;
};

/**
 * The messages queued for the peer and not sent yet. A test peer has no
 * socket, so everything that was pushed to it stays in its send queue, as a
 * 24 byte header followed, unless the payload is empty, by the payload.
 */
inline std::vector<SentMessage> SentMessages(CNode& node)
{
    std::vector<SentMessage> result;
    LOCK(node.cs_vSend);
    for (size_t i = 0; i < node.vSendMsg.size(); ++i) {
        const std::vector<unsigned char>& header = node.vSendMsg[i];
        assert(header.size() == CMessageHeader::HEADER_SIZE);
        SentMessage message;
        const char* command = reinterpret_cast<const char*>(header.data()) + CMessageHeader::MESSAGE_START_SIZE;
        message.command = std::string(command, strnlen(command, CMessageHeader::COMMAND_SIZE));
        const uint32_t size = ReadLE32(header.data() + CMessageHeader::MESSAGE_START_SIZE + CMessageHeader::COMMAND_SIZE);
        if (size > 0) {
            ++i;
            assert(i < node.vSendMsg.size());
            assert(node.vSendMsg[i].size() == size);
            message.payload = node.vSendMsg[i];
        }
        result.push_back(std::move(message));
    }
    return result;
}

/** The commands of the messages queued for the peer, in the order in which they were pushed. */
inline std::vector<std::string> SentCommands(CNode& node)
{
    std::vector<std::string> commands;
    for (const SentMessage& message : SentMessages(node)) commands.push_back(message.command);
    return commands;
}

inline size_t CountSent(CNode& node, const std::string& command)
{
    size_t count = 0;
    for (const SentMessage& message : SentMessages(node)) {
        if (message.command == command) ++count;
    }
    return count;
}

/** Forget what was queued for the peer. */
inline void ClearSent(CNode& node)
{
    LOCK(node.cs_vSend);
    node.vSendMsg.clear();
    node.nSendSize = 0;
    node.nSendOffset = 0;
}

/** The network serialization of an object. */
template <typename T>
CDataStream Payload(const T& obj)
{
    CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    stream << obj;
    return stream;
}

template <typename T>
std::vector<unsigned char> PayloadBytes(const T& obj)
{
    CDataStream stream = Payload(obj);
    return std::vector<unsigned char>(UCharCast(stream.data()), UCharCast(stream.data()) + stream.size());
}

/** Read an object from the payload of a queued message and require that nothing is left over. */
template <typename T>
T Decode(const SentMessage& message)
{
    CDataStream stream(message.payload, SER_NETWORK, PROTOCOL_VERSION);
    T obj;
    stream >> obj;
    assert(stream.empty());
    return obj;
}

static const ServiceFlags REDDID_PEER_SERVICES = ServiceFlags(NODE_NETWORK | NODE_REDDID);

/**
 * A peer as the network code sees it once the version handshake is complete,
 * with no socket. The caller owns it unless it hands it to a ConnmanTestMsg.
 *
 * ourServices is what this node advertised to the peer, peerServices what the
 * peer advertised to this node.
 */
inline CNode* NewTestPeer(NodeId id, ServiceFlags peerServices = REDDID_PEER_SERVICES,
                          ServiceFlags ourServices = REDDID_PEER_SERVICES)
{
    in_addr ip;
    ip.s_addr = htonl(0x0a000001 + static_cast<uint32_t>(id));
    CAddress addr(CService(ip, 7777), NODE_NONE);
    CNode* node = new CNode(id, ourServices, INVALID_SOCKET, addr, /* nKeyedNetGroupIn */ 0,
                            /* nLocalHostNonceIn */ 0, CAddress(), /* addrNameIn */ "",
                            ConnectionType::INBOUND, /* inbound_onion */ false);
    node->nServices = peerServices;
    node->nVersion = PROTOCOL_VERSION;
    node->SetCommonVersion(PROTOCOL_VERSION);
    node->fSuccessfullyConnected = true;
    return node;
}

/** Make the connection manager advertise the services, as a node does that was started with or without -reddid. */
inline void SetLocalServices(CConnman& connman, NetEventsInterface* msgproc, ServiceFlags services)
{
    CConnman::Options options;
    options.m_msgproc = msgproc;
    options.nLocalServices = services;
    connman.Init(options);
}

} // namespace reddid_test

/**
 * ProfileTestingSetup (clock stopped at T0, the namespace NS, shorthands for
 * auctions, bids and profiles) with test peers and shorthands for handing
 * messages to the ReddID P2P manager.
 *
 * The connection manager of the fixture advertises NODE_REDDID, as that of a
 * node started with -reddid=1 does. The peers have no socket: what is pushed
 * to them stays in their send queues, where the tests read it.
 */
struct P2PTestingSetup : public ProfileTestingSetup {
    ReddIDP2PManager* p2p;
    NamespaceManager* nsMgr;
    ConnmanTestMsg& connman;
    NodeId m_next_id{0};

    P2PTestingSetup()
        : p2p(reddidManager->GetP2PManager()),
          nsMgr(reddidManager->GetNamespaceManager()),
          connman(static_cast<ConnmanTestMsg&>(*m_node.connman))
    {
        assert(p2p != nullptr);
        reddid_test::SetLocalServices(connman, m_node.peerman.get(), reddid_test::REDDID_PEER_SERVICES);
    }

    ~P2PTestingSetup()
    {
        // The connection manager owns the peers that were added to it
        connman.ClearTestNodes();
    }

    /**
     * A peer that completed the version handshake and that the connection
     * manager knows. It is subscribed to ReddID messages directly, without
     * OnNodeConnected, so that nothing is queued for it yet.
     */
    CNode& AddPeer(bool subscribe = true, ServiceFlags peerServices = reddid_test::REDDID_PEER_SERVICES)
    {
        CNode* node = reddid_test::NewTestPeer(m_next_id++, peerServices);
        connman.AddTestNode(*node);
        if (subscribe) p2p->SubscribeNode(node->GetId());
        return *node;
    }

    /** Hand a message to the P2P manager, as net_processing does, and tell what became of it. */
    reddid_test::Outcome Send(CNode& from, const std::string& command, const CDataStream& payload)
    {
        using reddid_test::Outcome;
        reddid_test::LogCapture log;
        CDataStream stream(payload);
        p2p->ProcessMessage(from, command, stream, GetTimeMicros());
        if (log.Has("Ignoring message " + command + " - manager not running")) return Outcome::NOT_RUNNING;
        if (log.Has("Ignoring ReddID message " + command + " from incompatible peer")) return Outcome::INCOMPATIBLE;
        if (log.Has("Empty message " + command + " from peer")) return Outcome::EMPTY;
        if (log.Has("Oversized message " + command + " (")) return Outcome::OVERSIZED;
        if (log.Has("Rate limited message " + command + " from peer")) return Outcome::RATE_LIMITED;
        assert(log.Has("Processing message " + command + " from peer"));
        if (log.Has("Unknown message type: " + command)) return Outcome::UNKNOWN;
        if (log.Has("Handler returned false for message " + command)) return Outcome::REJECTED;
        return Outcome::ACCEPTED;
    }

    template <typename T>
    reddid_test::Outcome SendMsg(CNode& from, const std::string& command, const T& msg)
    {
        return Send(from, command, reddid_test::Payload(msg));
    }

    /** Send from a peer that has sent nothing before, so that the rate limiter does not interfere. */
    template <typename T>
    reddid_test::Outcome SendFromNewPeer(const std::string& command, const T& msg)
    {
        return SendMsg(AddPeer(/* subscribe */ false), command, msg);
    }

    /** A namespace configuration that ProcessNamespaceConfigResponse accepts. */
    CNamespaceConfig ConfigMessage(const std::string& nsId, const CKeyID& owner)
    {
        CNamespaceConfig msg;
        msg.timestamp = GetTime();
        msg.config.id = nsId;
        msg.config.owner = owner;
        msg.config.configHash = nsMgr->CalculateConfigHash(msg.config);
        return msg;
    }

    /** A user ID auction announcement in NS that ProcessUserIDAuctionAnnounce accepts. */
    CUserIDAuctionAnnounce UserAnnounce(const std::string& name, const CKeyID& auctionCreator)
    {
        CUserIDAuctionAnnounce msg;
        msg.timestamp = GetTime();
        msg.name = name;
        msg.namespaceId = NS;
        msg.creator = auctionCreator;
        msg.startTime = GetTime();
        msg.endTime = GetTime() + 7 * reddid_test::DAY;
        msg.reservePrice = 5000 * COIN;
        msg.type = AUCTION_STANDARD;
        msg.auctionId = reddid_test::ExpectedAuctionId(name, NS, msg.startTime);
        return msg;
    }

    CAuctionBid BidMessage(const uint256& auctionId, const CKeyID& bidder, CAmount amount)
    {
        CAuctionBid msg;
        msg.timestamp = GetTime();
        msg.auctionId = auctionId;
        msg.bidder = bidder;
        msg.bidAmount = amount;
        msg.depositAmount = 0;
        msg.bidId = reddid_test::ExpectedBidId(auctionId, bidder, amount, msg.timestamp);
        return msg;
    }

    CReddIDProfileUpdate ProfileMessage(const ReddIDProfile& profile)
    {
        CReddIDProfileUpdate msg;
        msg.timestamp = GetTime();
        msg.reddId = profile.reddId;
        msg.profile = profile;
        msg.profileHash = pMgr->CalculateProfileHash(profile);
        return msg;
    }
};

#endif // BITCOIN_TEST_REDDID_P2P_TEST_UTIL_H
