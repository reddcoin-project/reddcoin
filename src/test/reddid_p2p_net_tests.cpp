// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <id/namespace.h>
#include <id/profile.h>
#include <id/reddid.h>
#include <id/reddid_p2p.h>
#include <net.h>
#include <net_processing.h>
#include <node/context.h>
#include <protocol.h>
#include <streams.h>
#include <sync.h>
#include <test/reddid_p2p_test_util.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>
#include <version.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <utility>
#include <vector>

using namespace reddid_test;

// The hooks in net_processing: PeerManager hands messages whose command starts
// with "ns", "uid" or "rid" to the ReddID P2P manager of the node, and tells
// it about peers that connect and disconnect. A node without ReddID, which is
// the default, has no manager in its NodeContext, and the hooks do nothing.

namespace {
const std::vector<std::string> REDDID_COMMANDS{
    "nsaucann", "nsaucbid", "nsaucfin", "nsauccan", "nsconreq", "nsconres",
    "uidaucann", "uidaucbid", "uidaucfin", "uidauccan",
    "ridaucann", "ridaucbid", "ridaucfin", "ridprofup", "ridconn", "ridprofreq", "ridprofres", "ridrep"};

/**
 * P2PTestingSetup with peers that the PeerManager of the fixture knows, and
 * with the ReddID manager either in the NodeContext, as in a node started
 * with -reddid=1, or not, as in a node started without it.
 */
struct NetTestingSetup : public P2PTestingSetup {
    PeerManager& peerman;
    std::atomic<bool> interrupt{false};
    std::vector<CNode*> m_initialized;

    NetTestingSetup() : peerman(*m_node.peerman) {}

    ~NetTestingSetup()
    {
        for (CNode* node : m_initialized) peerman.FinalizeNode(*node);
        Detach();
    }

    /** Give the manager to the node, as init does in step 9.1. */
    void Attach()
    {
        assert(reddidManager);
        m_node.reddid = std::move(reddidManager);
    }

    /** Take it back, so that the fixture stops and destroys it. */
    void Detach()
    {
        if (m_node.reddid) reddidManager = std::move(m_node.reddid);
    }

    /** A peer that connected and completed the version handshake. */
    CNode& Connect(ServiceFlags peerServices = REDDID_PEER_SERVICES)
    {
        CNode* node = NewTestPeer(m_next_id++, peerServices);
        peerman.InitializeNode(node);
        m_initialized.push_back(node);
        connman.AddTestNode(*node);
        return *node;
    }

    void Disconnect(CNode& node)
    {
        peerman.FinalizeNode(node);
        for (auto it = m_initialized.begin(); it != m_initialized.end(); ++it) {
            if (*it == &node) {
                m_initialized.erase(it);
                break;
            }
        }
    }

    /** Hand a message to the PeerManager as the message handler thread does. */
    void Deliver(CNode& peer, const std::string& command, const CDataStream& payload)
    {
        CDataStream stream(payload);
        peerman.ProcessMessage(peer, command, stream, GetTime<std::chrono::microseconds>(), interrupt);
    }

    template <typename T>
    void DeliverMsg(CNode& peer, const std::string& command, const T& msg)
    {
        Deliver(peer, command, Payload(msg));
    }
};

CDataStream Junk()
{
    CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    stream << uint8_t{0};
    return stream;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(reddid_p2p_net_tests, NetTestingSetup)

// No message of the node itself is taken for a ReddID message.
BOOST_AUTO_TEST_CASE(no_other_message_type_has_a_reddid_prefix)
{
    for (const std::string& type : getAllNetMessageTypes()) {
        BOOST_CHECK_MESSAGE(type.substr(0, 2) != "ns" && type.substr(0, 3) != "uid" && type.substr(0, 3) != "rid", type);
    }
}

BOOST_AUTO_TEST_CASE(connecting_and_disconnecting_peers_reach_the_manager)
{
    Attach();
    CNode& peer = Connect();
    // InitializeNode called OnNodeConnected: the peer is subscribed and was
    // asked for the configuration of the one namespace that the node knows
    BOOST_CHECK(p2p->IsNodeSubscribed(peer.GetId()));
    BOOST_CHECK(SentCommands(peer) == std::vector<std::string>{"nsconreq"});
    ClearSent(peer);

    // Use up the rate limit of one command
    for (int i = 0; i < MAX_PROFILE_UPDATES_PER_MINUTE; ++i) Deliver(peer, "ridprofup", Junk());
    {
        LogCounter limited("Rate limited message ridprofup from peer");
        Deliver(peer, "ridprofup", Junk());
        BOOST_CHECK_EQUAL(limited.count, 1);
    }

    // FinalizeNode called OnNodeDisconnected: the subscription and the rate
    // limits of the peer are gone
    Disconnect(peer);
    BOOST_CHECK(!p2p->IsNodeSubscribed(peer.GetId()));
    {
        LogCounter limited("Rate limited message ridprofup from peer");
        LogCounter processed("Processing message ridprofup from peer");
        CDataStream stream = Junk();
        p2p->ProcessMessage(peer, "ridprofup", stream, 0);
        BOOST_CHECK_EQUAL(limited.count, 0);
        BOOST_CHECK_EQUAL(processed.count, 1);
    }
}

// SUSPECTED BUG: InitializeNode runs when a connection has just been made. The
// requests for the namespace configurations are queued before the peer has
// said anything, and before the version message of this node when the peer
// connected to us. A peer of this software ignores what arrives before the
// version message.
BOOST_AUTO_TEST_CASE(namespace_requests_are_the_first_messages_on_a_connection)
{
    CreateTestNamespace("second", MakeKeyID());
    Attach();
    CNode* node = NewTestPeer(m_next_id++);
    node->nVersion = 0;
    node->nServices = NODE_NONE;
    node->fSuccessfullyConnected = false;
    peerman.InitializeNode(node);
    m_initialized.push_back(node);
    connman.AddTestNode(*node);

    BOOST_CHECK(p2p->IsNodeSubscribed(node->GetId()));
    BOOST_CHECK(SentCommands(*node) == (std::vector<std::string>{"nsconreq", "nsconreq"}));
}

BOOST_AUTO_TEST_CASE(message_of_a_connected_peer_reaches_the_manager)
{
    Attach();
    CNode& peer = Connect();
    const CKeyID owner = MakeKeyID();

    // One message for each of the three prefixes
    DeliverMsg(peer, "nsconres", ConfigMessage("fresh", owner));
    BOOST_CHECK(nsMgr->IsOwner("fresh", owner));

    DeliverMsg(peer, "uidaucann", UserAnnounce("alice", owner));
    AuctionInfo auction;
    BOOST_CHECK(aMgr->GetAuctionInfo(ExpectedAuctionId("alice", NS, T0), auction));

    DeliverMsg(peer, "ridprofup", ProfileMessage(TestProfile("alice", owner)));
    BOOST_CHECK(pMgr->ProfileExists("alice"));

    // Every ReddID command gets as far as the manager
    for (const std::string& command : REDDID_COMMANDS) {
        CNode& sender = Connect();
        LogCounter processed("Processing message " + command + " from peer " + ToString(sender.GetId()));
        LogCounter unknown("Unknown command");
        Deliver(sender, command, Junk());
        BOOST_CHECK_MESSAGE(processed.count == 1, command);
        BOOST_CHECK_EQUAL(unknown.count, 0);
    }

    // So does a command that only has the prefix: the manager finds no handler
    {
        LogCounter noHandler("Unknown message type: nsmadeup");
        Deliver(peer, "nsmadeup", Junk());
        BOOST_CHECK_EQUAL(noHandler.count, 1);
    }
}

BOOST_AUTO_TEST_CASE(message_before_the_handshake_does_not_reach_the_manager)
{
    Attach();
    CNode& peer = Connect();
    const CNamespaceConfig msg = ConfigMessage("fresh", MakeKeyID());

    // No version message yet
    peer.nVersion = 0;
    peer.fSuccessfullyConnected = false;
    {
        LogCounter early("non-version message before version handshake. Message \"nsconres\"");
        LogCounter processed("Processing message nsconres");
        DeliverMsg(peer, "nsconres", msg);
        BOOST_CHECK_EQUAL(early.count, 1);
        BOOST_CHECK_EQUAL(processed.count, 0);
    }
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("fresh"));

    // Version received, verack not yet
    peer.nVersion = PROTOCOL_VERSION;
    {
        LogCounter early("Unsupported message \"nsconres\" prior to verack");
        LogCounter processed("Processing message nsconres");
        DeliverMsg(peer, "nsconres", msg);
        BOOST_CHECK_EQUAL(early.count, 1);
        BOOST_CHECK_EQUAL(processed.count, 0);
    }
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("fresh"));

    peer.fSuccessfullyConnected = true;
    DeliverMsg(peer, "nsconres", msg);
    BOOST_CHECK(!nsMgr->IsNamespaceAvailable("fresh"));
}

// A node without ReddID has no manager in its NodeContext.
BOOST_AUTO_TEST_CASE(node_without_reddid_ignores_the_messages)
{
    BOOST_REQUIRE(!m_node.reddid);
    CNode& peer = Connect();
    // No subscription and no request when the peer connected
    BOOST_CHECK(!p2p->IsNodeSubscribed(peer.GetId()));
    BOOST_CHECK(SentMessages(peer).empty());

    {
        LogCounter unavailable("ReddID P2P manager not available for message nsconres from peer=" + ToString(peer.GetId()));
        LogCounter processed("Processing message nsconres");
        LogCounter misbehaving("Misbehaving: peer=");
        DeliverMsg(peer, "nsconres", ConfigMessage("fresh", MakeKeyID()));
        BOOST_CHECK_EQUAL(unavailable.count, 1);
        BOOST_CHECK_EQUAL(processed.count, 0);
        BOOST_CHECK_EQUAL(misbehaving.count, 0);
    }
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("fresh"));

    // Any command with one of the prefixes, any payload
    {
        LogCounter unavailable("ReddID P2P manager not available for message");
        LogCounter misbehaving("Misbehaving: peer=");
        for (const std::string& command : REDDID_COMMANDS) {
            Deliver(peer, command, Junk());
            Deliver(peer, command, CDataStream(SER_NETWORK, PROTOCOL_VERSION));
        }
        Deliver(peer, "nsmadeup", Junk());
        BOOST_CHECK_EQUAL(unavailable.count, 2 * 18 + 1);
        BOOST_CHECK_EQUAL(misbehaving.count, 0);
    }
    BOOST_CHECK(!peer.fDisconnect);
    BOOST_CHECK(SentMessages(peer).empty());

    // The disconnect hook is inert as well
    Disconnect(peer);
}

// Commands without one of the prefixes are handled as before, with and
// without a manager.
BOOST_AUTO_TEST_CASE(other_commands_are_not_affected)
{
    for (bool withManager : {false, true}) {
        if (withManager) Attach();
        CNode& peer = Connect();
        ClearSent(peer);

        {
            LogCounter unknown("Unknown command \"madeup\" from peer=" + ToString(peer.GetId()));
            LogCounter reddid("ReddID");
            Deliver(peer, "madeup", Junk());
            // "n", "u", "ui" and "ri" are not prefixes
            Deliver(peer, "n", Junk());
            Deliver(peer, "uiaucann", Junk());
            Deliver(peer, "riprofup", Junk());
            BOOST_CHECK_EQUAL(unknown.count, 1);
            BOOST_CHECK_EQUAL(reddid.count, 0);
        }

        // A ping is answered
        CDataStream ping(SER_NETWORK, PROTOCOL_VERSION);
        ping << uint64_t{0x1122334455667788};
        Deliver(peer, NetMsgType::PING, ping);
        const std::vector<SentMessage> sent = SentMessages(peer);
        BOOST_REQUIRE_EQUAL(sent.size(), 1U);
        BOOST_CHECK_EQUAL(sent[0].command, "pong");
        BOOST_CHECK(sent[0].payload == PayloadBytes(uint64_t{0x1122334455667788}));
    }
}

// SUSPECTED BUG: net_processing adds 10 to the misbehaviour score of a peer
// when the ReddID P2P manager throws while it processes a message. The
// manager catches what its handlers throw, so nothing reaches net_processing:
// a peer can send any number of malformed, oversized, empty, unknown or
// rate-limited ReddID messages and is never penalised or disconnected.
BOOST_AUTO_TEST_CASE(malformed_messages_are_never_penalised)
{
    Attach();
    CNode& peer = Connect();
    ClearSent(peer);

    CDataStream tooLarge(SER_NETWORK, PROTOCOL_VERSION);
    tooLarge << std::string(MAX_REDDID_MESSAGE_SIZE, 'a');
    // A length prefix that promises more than the payload holds
    CDataStream truncated(SER_NETWORK, PROTOCOL_VERSION);
    truncated << uint8_t{0xfd} << uint8_t{0xff} << uint8_t{0xff};

    {
        LogCounter misbehaving("Misbehaving: peer=");
        LogCounter errors("Error processing ReddID message");
        for (const std::string& command : REDDID_COMMANDS) {
            // More of each than any rate limit allows
            for (int i = 0; i < 12; ++i) {
                Deliver(peer, command, Junk());
                Deliver(peer, command, truncated);
            }
            Deliver(peer, command, tooLarge);
            Deliver(peer, command, CDataStream(SER_NETWORK, PROTOCOL_VERSION));
        }
        Deliver(peer, "nsmadeup", Junk());
        BOOST_CHECK_EQUAL(misbehaving.count, 0);
        BOOST_CHECK_EQUAL(errors.count, 0);
    }
    BOOST_CHECK(!peer.fDisconnect);
    BOOST_CHECK(SentMessages(peer).empty());

    // The score is recorded in the log when there is one
    {
        LogCounter misbehaving("Misbehaving: peer=" + ToString(peer.GetId()) + " (0 -> 1)");
        peerman.Misbehaving(peer.GetId(), 1, "test");
        BOOST_CHECK_EQUAL(misbehaving.count, 1);
    }
}

// The handlers run with cs_main held, as the chain tip hooks and the periodic
// task do, so that a message cannot change the state of the component
// managers while a block is being applied or undone.
BOOST_AUTO_TEST_CASE(message_waits_for_cs_main)
{
    CNode& peer = AddPeer();
    const CKeyID owner = MakeKeyID();
    CDataStream stream = Payload(ConfigMessage("fresh", owner));

    std::future<void> done;
    {
        LOCK(cs_main);
        done = std::async(std::launch::async, [&] { p2p->ProcessMessage(peer, "nsconres", stream, 0); });
        // Without the lock in ProcessMessage this wait would end at once
        BOOST_CHECK(done.wait_for(std::chrono::milliseconds{300}) == std::future_status::timeout);
        BOOST_CHECK(nsMgr->IsNamespaceAvailable("fresh"));
    }
    done.wait();
    BOOST_CHECK(nsMgr->IsOwner("fresh", owner));
}

// The same through PeerManager, which holds no lock when it reaches the hook:
// the thread that delivers a ReddID message is parked on cs_main, holding
// nothing else, while this thread holds cs_main and relays as the chain tip
// hooks do. That shows only that the two do not block each other here; the
// relay's order cs_main, peer list, send lock is checked against the rest of
// the program by DEBUG_LOCKORDER, which aborts on an inversion.
BOOST_AUTO_TEST_CASE(relay_under_cs_main_while_a_message_is_processed)
{
    Attach();
    CNode& sender = Connect();
    CNode& listener = Connect();
    ClearSent(sender);
    ClearSent(listener);
    const CKeyID owner = MakeKeyID();
    const CDataStream stream = Payload(ConfigMessage("fresh", owner));

    std::future<void> done;
    {
        LOCK(cs_main);
        done = std::async(std::launch::async, [&] { Deliver(sender, "nsconres", stream); });
        BOOST_CHECK(done.wait_for(std::chrono::milliseconds{200}) == std::future_status::timeout);
        // cs_main, then the lock of the peer list, then the send lock of a peer
        for (int i = 0; i < 100; ++i) p2p->AnnounceUserIDCancel(uint256::ONE);
    }
    done.wait();
    BOOST_CHECK(nsMgr->IsOwner("fresh", owner));
    BOOST_CHECK_EQUAL(CountSent(listener, "uidauccan"), 100U);
    BOOST_CHECK_EQUAL(CountSent(listener, "nsconres"), 1U);
    BOOST_CHECK_EQUAL(CountSent(sender, "nsconres"), 0U);
}

BOOST_AUTO_TEST_SUITE_END()
