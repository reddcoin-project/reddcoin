// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <amount.h>
#include <id/auction.h>
#include <id/namespace.h>
#include <id/profile.h>
#include <id/reddid.h>
#include <id/reddid_p2p.h>
#include <net.h>
#include <protocol.h>
#include <streams.h>
#include <sync.h>
#include <test/reddid_p2p_test_util.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>
#include <version.h>

#include <chrono>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace reddid_test;

// The parts of ReddIDP2PManager that do not depend on the type of a message:
// its life cycle, what ProcessMessage checks before it looks for a handler,
// the rate limiter, the subscriptions, the relay and the announcements that
// the component managers make. The handlers are in reddid_p2p_handler_tests.

namespace {
/** A payload that no handler accepts. */
CDataStream Junk()
{
    CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    stream << uint8_t{0};
    return stream;
}

const std::vector<std::pair<std::string, int>> RATE_LIMITS{
    {"nsaucann", 10},
    {"uidaucann", 10},
    {"ridaucann", 10},
    {"nsaucbid", 20},
    {"uidaucbid", 20},
    {"ridaucbid", 20},
    {"ridprofup", 5},
    {"ridconn", 15},
    {"nsaucfin", 20},
    {"nsauccan", 20},
    {"nsconreq", 20},
    {"nsconres", 20},
    {"uidaucfin", 20},
    {"uidauccan", 20},
    {"ridaucfin", 20},
    {"ridprofreq", 20},
    {"ridprofres", 20},
    {"ridrep", 20},
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(reddid_p2p_tests, P2PTestingSetup)

BOOST_AUTO_TEST_CASE(manager_owns_a_running_p2p_manager)
{
    BOOST_CHECK(p2p->IsInitialized());
    BOOST_CHECK(p2p->IsRunning());
    BOOST_CHECK(p2p->IsReddIDEnabled());
    // The component managers announce through it
    BOOST_CHECK(nsMgr->GetP2P() == p2p);
    BOOST_CHECK(aMgr->GetP2P() == p2p);
}

BOOST_AUTO_TEST_CASE(life_cycle)
{
    ReddIDP2PManager manager(m_node);
    BOOST_CHECK(!manager.IsInitialized());
    BOOST_CHECK(!manager.IsRunning());

    // Nothing works before Init()
    BOOST_CHECK(!manager.Start());
    BOOST_CHECK(!manager.RegisterMessageHandlers());
    BOOST_CHECK(manager.Stop());
    manager.Interrupt();

    BOOST_CHECK(!manager.Init(nullptr));
    BOOST_CHECK(!manager.IsInitialized());
    BOOST_CHECK(manager.Init(reddidManager.get()));
    BOOST_CHECK(manager.IsInitialized());
    BOOST_CHECK(manager.Init(reddidManager.get()));

    // The handlers are registered only once the manager runs
    BOOST_CHECK(!manager.RegisterMessageHandlers());
    BOOST_CHECK(manager.Start());
    BOOST_CHECK(manager.IsRunning());
    BOOST_CHECK(manager.Start());
    BOOST_CHECK(manager.RegisterMessageHandlers());
    BOOST_CHECK(manager.RegisterMessageHandlers());

    manager.Interrupt();
    BOOST_CHECK(!manager.IsRunning());
    BOOST_CHECK(manager.IsInitialized());
    manager.Interrupt();

    // An interrupted manager can be started again
    BOOST_CHECK(manager.Start());
    BOOST_CHECK(manager.IsRunning());

    BOOST_CHECK(manager.Stop());
    BOOST_CHECK(!manager.IsRunning());
    BOOST_CHECK(!manager.IsInitialized());
    BOOST_CHECK(manager.Stop());
    BOOST_CHECK(!manager.Start());
}

BOOST_AUTO_TEST_CASE(handlers_need_a_connection_manager)
{
    auto savedConnman = std::move(m_node.connman);
    {
        ReddIDP2PManager manager(m_node);
        BOOST_CHECK(manager.Init(reddidManager.get()));
        BOOST_CHECK(manager.Start());
        BOOST_CHECK(!manager.RegisterMessageHandlers());

        // Without a connection manager nothing is relayed, and nothing crashes
        AuctionInfo auction = UserAuction("alice", NS, creator, 5000 * COIN, GetTime());
        BOOST_CHECK(manager.AnnounceUserIDAuction(auction));
        manager.RelayMessage("uidauccan", Payload(uint256::ONE));
        BOOST_CHECK(!manager.SendNamespaceConfig(nullptr, NS));
        BOOST_CHECK(!manager.RequestAllNamespaceConfigs());
    }
    m_node.connman = std::move(savedConnman);
}

BOOST_AUTO_TEST_CASE(interrupted_manager_ignores_messages_and_announces_nothing)
{
    CNode& peer = AddPeer();
    const CKeyID owner = MakeKeyID();
    p2p->Interrupt();

    BOOST_CHECK_EQUAL(SendMsg(peer, "nsconres", ConfigMessage("fresh", owner)), Outcome::NOT_RUNNING);
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("fresh"));

    AuctionInfo auction = GetAuction(aMgr, NewAuction("alice"));
    BidInfo bid;
    BOOST_CHECK(!p2p->AnnounceNamespaceAuction(auction));
    BOOST_CHECK(!p2p->AnnounceNamespaceBid(bid));
    BOOST_CHECK(!p2p->AnnounceNamespaceCancel(auction.auctionId));
    BOOST_CHECK(!p2p->AnnounceUserIDAuction(auction));
    BOOST_CHECK(!p2p->AnnounceUserIDBid(bid));
    BOOST_CHECK(!p2p->AnnounceUserIDCancel(auction.auctionId));
    BOOST_CHECK(!p2p->AnnounceProfileUpdate("alice", TestProfile("alice", owner)));
    BOOST_CHECK(!p2p->SendNamespaceConfig(&peer, NS));
    BOOST_CHECK(!p2p->RequestAllNamespaceConfigs(&peer));
    BOOST_CHECK(SentMessages(peer).empty());

    // The subscriptions are kept while the manager is interrupted, but a
    // peer that connects is not asked for the namespace configurations
    std::unique_ptr<CNode> late{NewTestPeer(100)};
    p2p->OnNodeConnected(late.get());
    BOOST_CHECK(p2p->IsNodeSubscribed(100));
    BOOST_CHECK(SentMessages(*late).empty());
    p2p->OnNodeDisconnected(100);
    BOOST_CHECK(!p2p->IsNodeSubscribed(100));

    BOOST_CHECK(p2p->Start());
    BOOST_CHECK_EQUAL(SendMsg(peer, "nsconres", ConfigMessage("fresh", owner)), Outcome::ACCEPTED);
    BOOST_CHECK(!nsMgr->IsNamespaceAvailable("fresh"));
}

BOOST_AUTO_TEST_CASE(message_from_peer_without_the_service_bit_is_ignored)
{
    CNode& plain = AddPeer(/* subscribe */ true, NODE_NETWORK);
    BOOST_CHECK_EQUAL(SendMsg(plain, "nsconres", ConfigMessage("fresh", MakeKeyID())), Outcome::INCOMPATIBLE);
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("fresh"));

    // The bit that counts is the one that the peer advertised in its version
    // message, whatever else it advertised with it
    CNode& reddidOnly = AddPeer(/* subscribe */ false, NODE_REDDID);
    BOOST_CHECK_EQUAL(SendMsg(reddidOnly, "nsconres", ConfigMessage("fresh", MakeKeyID())), Outcome::ACCEPTED);
    BOOST_CHECK(!nsMgr->IsNamespaceAvailable("fresh"));
}

BOOST_AUTO_TEST_CASE(empty_and_oversized_payloads_are_ignored)
{
    CNode& peer = AddPeer();
    BOOST_CHECK_EQUAL(Send(peer, "nsconreq", CDataStream(SER_NETWORK, PROTOCOL_VERSION)), Outcome::EMPTY);

    // A payload of exactly the maximum size reaches the handler
    CDataStream largest(SER_NETWORK, PROTOCOL_VERSION);
    largest << std::string(MAX_REDDID_MESSAGE_SIZE - 5, 'a');
    BOOST_CHECK_EQUAL(largest.size(), MAX_REDDID_MESSAGE_SIZE);
    BOOST_CHECK_EQUAL(Send(peer, "nsconreq", largest), Outcome::REJECTED);

    CDataStream tooLarge(SER_NETWORK, PROTOCOL_VERSION);
    tooLarge << std::string(MAX_REDDID_MESSAGE_SIZE - 4, 'a');
    BOOST_CHECK_EQUAL(tooLarge.size(), MAX_REDDID_MESSAGE_SIZE + 1);
    BOOST_CHECK_EQUAL(Send(peer, "nsconreq", tooLarge), Outcome::OVERSIZED);

    BOOST_CHECK(SentMessages(peer).empty());
}

BOOST_AUTO_TEST_CASE(command_without_a_handler_is_ignored)
{
    CNode& peer = AddPeer();
    for (const std::string command : {"nsfoo", "ns", "uidfoo", "ridfoo", "nsaucannx", "NSAUCANN"}) {
        BOOST_CHECK_EQUAL(Send(peer, command, Junk()), Outcome::UNKNOWN);
    }
    BOOST_CHECK(SentMessages(peer).empty());
}

// The payload is copied before the handler reads it: the stream of the caller
// is left as it was.
BOOST_AUTO_TEST_CASE(process_message_does_not_consume_the_stream)
{
    CNode& peer = AddPeer();
    CDataStream stream = Payload(ConfigMessage("fresh", MakeKeyID()));
    const size_t size = stream.size();
    p2p->ProcessMessage(peer, "nsconres", stream, GetTimeMicros());
    BOOST_CHECK(!nsMgr->IsNamespaceAvailable("fresh"));
    BOOST_CHECK_EQUAL(stream.size(), size);
}

// --- Rate limiter ---

BOOST_AUTO_TEST_CASE(rate_limit_of_each_message_type)
{
    BOOST_CHECK_EQUAL(RATE_LIMITS.size(), 18U);
    for (const auto& [command, limit] : RATE_LIMITS) {
        CNode& peer = AddPeer(/* subscribe */ false);
        for (int i = 0; i < limit; ++i) {
            BOOST_CHECK_MESSAGE(Send(peer, command, Junk()) == Outcome::REJECTED, command << " message " << i);
        }
        BOOST_CHECK_MESSAGE(Send(peer, command, Junk()) == Outcome::RATE_LIMITED, command);
        BOOST_CHECK_MESSAGE(Send(peer, command, Junk()) == Outcome::RATE_LIMITED, command);
    }
}

BOOST_AUTO_TEST_CASE(rate_limit_window)
{
    CNode& peer = AddPeer(/* subscribe */ false);
    for (int i = 0; i < MAX_PROFILE_UPDATES_PER_MINUTE; ++i) {
        BOOST_CHECK_EQUAL(Send(peer, "ridprofup", Junk()), Outcome::REJECTED);
    }
    BOOST_CHECK_EQUAL(Send(peer, "ridprofup", Junk()), Outcome::RATE_LIMITED);

    // The window is counted from the first message, not from the last one,
    // and it is over when more than 60 seconds have passed
    SetMockTime(T0 + RATE_LIMIT_WINDOW_SECONDS);
    BOOST_CHECK_EQUAL(Send(peer, "ridprofup", Junk()), Outcome::RATE_LIMITED);
    SetMockTime(T0 + RATE_LIMIT_WINDOW_SECONDS + 1);
    for (int i = 0; i < MAX_PROFILE_UPDATES_PER_MINUTE; ++i) {
        BOOST_CHECK_EQUAL(Send(peer, "ridprofup", Junk()), Outcome::REJECTED);
    }
    BOOST_CHECK_EQUAL(Send(peer, "ridprofup", Junk()), Outcome::RATE_LIMITED);
}

BOOST_AUTO_TEST_CASE(rate_limit_is_kept_per_peer_and_per_command)
{
    CNode& peerA = AddPeer(/* subscribe */ false);
    CNode& peerB = AddPeer(/* subscribe */ false);
    for (int i = 0; i < MAX_PROFILE_UPDATES_PER_MINUTE; ++i) {
        BOOST_CHECK_EQUAL(Send(peerA, "ridprofup", Junk()), Outcome::REJECTED);
    }
    BOOST_CHECK_EQUAL(Send(peerA, "ridprofup", Junk()), Outcome::RATE_LIMITED);

    // Another peer, and another command of the same peer, have their own count
    BOOST_CHECK_EQUAL(Send(peerB, "ridprofup", Junk()), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(Send(peerA, "ridprofres", Junk()), Outcome::REJECTED);
    BOOST_CHECK_EQUAL(Send(peerA, "ridprofup", Junk()), Outcome::RATE_LIMITED);
}

BOOST_AUTO_TEST_CASE(rate_limit_is_forgotten_when_the_peer_disconnects)
{
    CNode& peer = AddPeer();
    for (int i = 0; i < MAX_PROFILE_UPDATES_PER_MINUTE; ++i) {
        BOOST_CHECK_EQUAL(Send(peer, "ridprofup", Junk()), Outcome::REJECTED);
    }
    BOOST_CHECK_EQUAL(Send(peer, "ridprofup", Junk()), Outcome::RATE_LIMITED);

    p2p->OnNodeDisconnected(peer.GetId());
    BOOST_CHECK(!p2p->IsNodeSubscribed(peer.GetId()));
    BOOST_CHECK_EQUAL(Send(peer, "ridprofup", Junk()), Outcome::REJECTED);
}

// Messages that the earlier checks turn away are not counted.
BOOST_AUTO_TEST_CASE(rate_limit_does_not_count_what_is_ignored_before_it)
{
    CNode& peer = AddPeer(/* subscribe */ false);
    CDataStream tooLarge(SER_NETWORK, PROTOCOL_VERSION);
    tooLarge << std::string(MAX_REDDID_MESSAGE_SIZE, 'a');
    for (int i = 0; i < 3 * MAX_PROFILE_UPDATES_PER_MINUTE; ++i) {
        BOOST_CHECK_EQUAL(Send(peer, "ridprofup", CDataStream(SER_NETWORK, PROTOCOL_VERSION)), Outcome::EMPTY);
        BOOST_CHECK_EQUAL(Send(peer, "ridprofup", tooLarge), Outcome::OVERSIZED);
    }
    BOOST_CHECK_EQUAL(Send(peer, "ridprofup", Junk()), Outcome::REJECTED);
}

// SUSPECTED BUG: the rate limiter runs before the handler is looked up and
// keeps one counter per peer and per command string as the peer sent it. A
// command that no handler exists for gets a counter of its own, so a peer
// that makes up commands starting with "ns", "uid" or "rid" adds an entry for
// each. The entries of a peer are removed only when it disconnects.
BOOST_AUTO_TEST_CASE(rate_limit_counts_commands_that_have_no_handler)
{
    CNode& peer = AddPeer(/* subscribe */ false);
    for (const std::string command : {"nsmadeup1", "nsmadeup2", "uidmadeup", "ridmadeup"}) {
        for (int i = 0; i < DEFAULT_MAX_MESSAGES_PER_MINUTE; ++i) {
            BOOST_CHECK_EQUAL(Send(peer, command, Junk()), Outcome::UNKNOWN);
        }
        // Only a stored counter can tell the 21st message from the first
        BOOST_CHECK_EQUAL(Send(peer, command, Junk()), Outcome::RATE_LIMITED);
    }
}

// --- Subscriptions ---

BOOST_AUTO_TEST_CASE(subscribe_and_unsubscribe)
{
    BOOST_CHECK(!p2p->IsNodeSubscribed(7));
    BOOST_CHECK(p2p->SubscribeNode(7));
    BOOST_CHECK(p2p->IsNodeSubscribed(7));
    BOOST_CHECK(p2p->SubscribeNode(7));
    BOOST_CHECK(p2p->IsNodeSubscribed(7));
    BOOST_CHECK(!p2p->IsNodeSubscribed(8));

    BOOST_CHECK(p2p->UnsubscribeNode(7));
    BOOST_CHECK(!p2p->IsNodeSubscribed(7));
    // Unsubscribing a peer that is not subscribed is not an error
    BOOST_CHECK(p2p->UnsubscribeNode(7));
    BOOST_CHECK(p2p->UnsubscribeNode(8));
}

BOOST_AUTO_TEST_CASE(connecting_peer_is_subscribed_and_asked_for_every_namespace)
{
    CreateTestNamespace("second", MakeKeyID());

    std::unique_ptr<CNode> peer{NewTestPeer(50)};
    p2p->OnNodeConnected(peer.get());
    BOOST_CHECK(p2p->IsNodeSubscribed(50));

    std::set<std::string> requested;
    const std::vector<SentMessage> sent = SentMessages(*peer);
    BOOST_CHECK_EQUAL(sent.size(), 2U);
    for (const SentMessage& message : sent) {
        BOOST_CHECK_EQUAL(message.command, "nsconreq");
        // The payload is the bare namespace ID
        requested.insert(Decode<std::string>(message));
    }
    BOOST_CHECK(requested == (std::set<std::string>{NS, "second"}));

    p2p->OnNodeDisconnected(50);
    BOOST_CHECK(!p2p->IsNodeSubscribed(50));
    // A peer that was never connected
    p2p->OnNodeDisconnected(51);
    p2p->OnNodeConnected(nullptr);
}

// SUSPECTED BUG: OnNodeConnected decides by CNode::GetLocalServices(), which
// is what this node advertised to the peer, not by what the peer advertised.
// Every peer of a node that runs ReddID is subscribed, whether it knows the
// messages or not.
BOOST_AUTO_TEST_CASE(connecting_peer_is_judged_by_our_own_services)
{
    // The peer does not advertise NODE_REDDID, this node does
    std::unique_ptr<CNode> plain{NewTestPeer(60, /* peerServices */ NODE_NETWORK, /* ourServices */ REDDID_PEER_SERVICES)};
    p2p->OnNodeConnected(plain.get());
    BOOST_CHECK(p2p->IsNodeSubscribed(60));
    BOOST_CHECK_EQUAL(CountSent(*plain, "nsconreq"), 1U);

    // The peer advertises NODE_REDDID, this node did not advertise it to the peer
    std::unique_ptr<CNode> capable{NewTestPeer(61, /* peerServices */ REDDID_PEER_SERVICES, /* ourServices */ NODE_NETWORK)};
    p2p->OnNodeConnected(capable.get());
    BOOST_CHECK(!p2p->IsNodeSubscribed(61));
    BOOST_CHECK(SentMessages(*capable).empty());
}

// SUSPECTED BUG: net_processing calls OnNodeConnected from InitializeNode,
// when the connection has just been made. Nothing has been received from the
// peer yet, and for an inbound peer not even our version message has been
// sent: the requests for the namespace configurations are the first messages
// on the connection, one for each namespace that this node knows.
BOOST_AUTO_TEST_CASE(connecting_peer_is_asked_before_the_version_handshake)
{
    std::unique_ptr<CNode> peer{NewTestPeer(70)};
    peer->nVersion = 0;
    peer->nServices = NODE_NONE;
    peer->fSuccessfullyConnected = false;

    p2p->OnNodeConnected(peer.get());
    BOOST_CHECK(p2p->IsNodeSubscribed(70));
    BOOST_CHECK(SentCommands(*peer) == std::vector<std::string>{"nsconreq"});
}

// A peer connects on a network thread that holds no lock, and the list of
// namespaces changes under cs_main: the list is read with cs_main held.
BOOST_AUTO_TEST_CASE(connecting_peer_waits_for_cs_main)
{
    std::unique_ptr<CNode> peer{NewTestPeer(80)};

    std::future<void> done;
    {
        LOCK(cs_main);
        done = std::async(std::launch::async, [&] { p2p->OnNodeConnected(peer.get()); });
        // Without the lock in RequestAllNamespaceConfigs this wait would end at once
        BOOST_CHECK(done.wait_for(std::chrono::milliseconds{300}) == std::future_status::timeout);
    }
    done.wait();
    BOOST_CHECK(p2p->IsNodeSubscribed(80));
    BOOST_CHECK_EQUAL(CountSent(*peer, "nsconreq"), 1U);
}

BOOST_AUTO_TEST_CASE(request_all_namespace_configs)
{
    CreateTestNamespace("second", MakeKeyID());
    CNode& subscribedA = AddPeer();
    CNode& subscribedB = AddPeer();
    CNode& other = AddPeer(/* subscribe */ false);

    // To one peer: only if it is subscribed
    BOOST_CHECK(!p2p->RequestAllNamespaceConfigs(&other));
    BOOST_CHECK(SentMessages(other).empty());
    BOOST_CHECK(p2p->RequestAllNamespaceConfigs(&subscribedA));
    BOOST_CHECK_EQUAL(CountSent(subscribedA, "nsconreq"), 2U);
    BOOST_CHECK(SentMessages(subscribedB).empty());
    ClearSent(subscribedA);

    // Current behaviour: without a target the request is meant for the first
    // subscribed peer, but the callback cannot stop CConnman::ForEachNode, so
    // every subscribed peer is asked for every namespace.
    BOOST_CHECK(p2p->RequestAllNamespaceConfigs());
    BOOST_CHECK_EQUAL(CountSent(subscribedA, "nsconreq"), 2U);
    BOOST_CHECK_EQUAL(CountSent(subscribedB, "nsconreq"), 2U);
    BOOST_CHECK(SentMessages(other).empty());
}

BOOST_AUTO_TEST_CASE(request_all_namespace_configs_without_peers)
{
    BOOST_CHECK(!p2p->RequestAllNamespaceConfigs());
}

// --- Relay ---

BOOST_AUTO_TEST_CASE(relay_reaches_subscribed_connected_peers_only)
{
    CNode& subscribed = AddPeer();
    CNode& notSubscribed = AddPeer(/* subscribe */ false);
    CNode& handshaking = AddPeer();
    handshaking.fSuccessfullyConnected = false;
    CNode& disconnecting = AddPeer();
    disconnecting.fDisconnect = true;
    CNode& excluded = AddPeer();
    // Subscribed without advertising the service: relayed to all the same
    CNode& plain = AddPeer(/* subscribe */ true, NODE_NETWORK);

    const uint256 auctionId = InsecureRand256();
    p2p->RelayMessage("uidauccan", Payload(auctionId), {excluded.GetId()});

    for (CNode* peer : {&subscribed, &plain}) {
        const std::vector<SentMessage> sent = SentMessages(*peer);
        BOOST_REQUIRE_EQUAL(sent.size(), 1U);
        BOOST_CHECK_EQUAL(sent[0].command, "uidauccan");
        BOOST_CHECK(sent[0].payload == PayloadBytes(auctionId));
    }
    for (CNode* peer : {&notSubscribed, &handshaking, &disconnecting, &excluded}) {
        BOOST_CHECK(SentMessages(*peer).empty());
    }

    // Without exceptions the excluded peer gets it too
    p2p->RelayMessage("uidauccan", Payload(auctionId));
    BOOST_CHECK_EQUAL(CountSent(subscribed, "uidauccan"), 2U);
    BOOST_CHECK_EQUAL(CountSent(excluded, "uidauccan"), 1U);
}

// The command is not checked: RelayMessage sends whatever it is given.
BOOST_AUTO_TEST_CASE(relay_sends_any_command_and_payload)
{
    CNode& peer = AddPeer();
    p2p->RelayMessage("nsmadeup", Junk());
    const std::vector<SentMessage> sent = SentMessages(peer);
    BOOST_REQUIRE_EQUAL(sent.size(), 1U);
    BOOST_CHECK_EQUAL(sent[0].command, "nsmadeup");
    BOOST_CHECK(sent[0].payload == std::vector<unsigned char>{0});
}

BOOST_AUTO_TEST_CASE(relay_needs_the_local_service_bit)
{
    CNode& peer = AddPeer();
    SetLocalServices(connman, m_node.peerman.get(), NODE_NETWORK);
    BOOST_CHECK(!p2p->IsReddIDEnabled());

    {
        LogCounter refused("Not relaying ReddID messages - service not enabled");
        p2p->RelayMessage("uidauccan", Payload(uint256::ONE));
        // The announcements report success although nothing is sent
        BOOST_CHECK(p2p->AnnounceUserIDCancel(uint256::ONE));
        BOOST_CHECK_EQUAL(refused.count, 2);
    }
    BOOST_CHECK(SentMessages(peer).empty());

    // A reply to one peer does not go through RelayMessage and is still sent
    BOOST_CHECK(p2p->SendNamespaceConfig(&peer, NS));
    BOOST_CHECK(SentCommands(peer) == std::vector<std::string>{"nsconres"});

    SetLocalServices(connman, m_node.peerman.get(), REDDID_PEER_SERVICES);
    BOOST_CHECK(p2p->IsReddIDEnabled());
}

BOOST_AUTO_TEST_CASE(relay_and_announcements_without_peers)
{
    LogCounter relayed("message to 0 nodes");
    p2p->RelayMessage("uidauccan", Payload(uint256::ONE));
    p2p->RelayMessage("uidauccan", Payload(uint256::ONE), {1, 2, 3});

    AuctionInfo auction = GetAuction(aMgr, NewAuction("alice"));
    BOOST_CHECK(p2p->AnnounceUserIDAuction(auction));
    BOOST_CHECK(p2p->AnnounceUserIDCancel(auction.auctionId));
    BOOST_CHECK(p2p->AnnounceProfileUpdate("alice", TestProfile("alice", MakeKeyID())));
    // NewAuction announced the auction as well
    BOOST_CHECK_EQUAL(relayed.count, 6);
}

// --- Announcements ---

BOOST_AUTO_TEST_CASE(announce_namespace_auction)
{
    CNode& peer = AddPeer();
    AuctionInfo auction;
    auction.auctionId = InsecureRand256();
    auction.namespaceId = "fresh";
    auction.creator = MakeKeyID();
    auction.startTime = T0 - 5;
    auction.endTime = T0 + 7 * DAY;
    auction.reservePrice = 12345 * COIN;
    auction.type = AUCTION_PREMIUM;
    auction.metadataHash = InsecureRand256();
    BOOST_CHECK(p2p->AnnounceNamespaceAuction(auction));

    const std::vector<SentMessage> sent = SentMessages(peer);
    BOOST_REQUIRE_EQUAL(sent.size(), 1U);
    BOOST_CHECK_EQUAL(sent[0].command, "nsaucann");
    const CNamespaceAuctionAnnounce msg = Decode<CNamespaceAuctionAnnounce>(sent[0]);
    BOOST_CHECK_EQUAL(msg.timestamp, T0);
    BOOST_CHECK(msg.auctionId == auction.auctionId);
    BOOST_CHECK_EQUAL(msg.namespaceId, "fresh");
    BOOST_CHECK(msg.creator == auction.creator);
    BOOST_CHECK_EQUAL(msg.startTime, T0 - 5);
    BOOST_CHECK_EQUAL(msg.endTime, T0 + 7 * DAY);
    BOOST_CHECK_EQUAL(msg.reservePrice, 12345 * COIN);
    BOOST_CHECK(msg.type == AUCTION_PREMIUM);
    // Current behaviour: the configuration hash of the message is never set
    BOOST_CHECK(msg.configHash.IsNull());
}

BOOST_AUTO_TEST_CASE(announce_user_id_auction)
{
    CNode& peer = AddPeer();
    AuctionInfo auction = UserAuction("alice", NS, creator, 5000 * COIN, T0 - 5);
    auction.auctionId = InsecureRand256();
    auction.type = AUCTION_VERIFIED;
    BOOST_CHECK(p2p->AnnounceUserIDAuction(auction));

    const std::vector<SentMessage> sent = SentMessages(peer);
    BOOST_REQUIRE_EQUAL(sent.size(), 1U);
    BOOST_CHECK_EQUAL(sent[0].command, "uidaucann");
    const CUserIDAuctionAnnounce msg = Decode<CUserIDAuctionAnnounce>(sent[0]);
    BOOST_CHECK_EQUAL(msg.timestamp, T0);
    BOOST_CHECK(msg.auctionId == auction.auctionId);
    BOOST_CHECK_EQUAL(msg.name, "alice");
    BOOST_CHECK_EQUAL(msg.namespaceId, NS);
    BOOST_CHECK(msg.creator == creator);
    BOOST_CHECK_EQUAL(msg.startTime, T0 - 5);
    BOOST_CHECK_EQUAL(msg.endTime, T0 - 5 + 7 * DAY);
    BOOST_CHECK_EQUAL(msg.reservePrice, 5000 * COIN);
    BOOST_CHECK(msg.type == AUCTION_VERIFIED);
}

BOOST_AUTO_TEST_CASE(announce_bids)
{
    CNode& peer = AddPeer();
    BidInfo bid;
    bid.bidId = InsecureRand256();
    bid.auctionId = InsecureRand256();
    bid.bidder = MakeKeyID();
    bid.bidAmount = 6000 * COIN;
    bid.depositAmount = 900 * COIN;
    bid.bidTime = T0 - 100;
    BOOST_CHECK(p2p->AnnounceNamespaceBid(bid));
    BOOST_CHECK(p2p->AnnounceUserIDBid(bid));

    const std::vector<SentMessage> sent = SentMessages(peer);
    BOOST_REQUIRE_EQUAL(sent.size(), 2U);
    BOOST_CHECK_EQUAL(sent[0].command, "nsaucbid");
    BOOST_CHECK_EQUAL(sent[1].command, "uidaucbid");
    // The same message under two commands
    BOOST_CHECK(sent[0].payload == sent[1].payload);
    const CAuctionBid msg = Decode<CAuctionBid>(sent[0]);
    // The timestamp is the time of the announcement, not the time of the bid
    BOOST_CHECK_EQUAL(msg.timestamp, T0);
    BOOST_CHECK(msg.auctionId == bid.auctionId);
    BOOST_CHECK(msg.bidId == bid.bidId);
    BOOST_CHECK(msg.bidder == bid.bidder);
    BOOST_CHECK_EQUAL(msg.bidAmount, 6000 * COIN);
    BOOST_CHECK_EQUAL(msg.depositAmount, 900 * COIN);
}

BOOST_AUTO_TEST_CASE(announce_cancellations)
{
    CNode& peer = AddPeer();
    const uint256 auctionId = InsecureRand256();
    BOOST_CHECK(p2p->AnnounceNamespaceCancel(auctionId));
    BOOST_CHECK(p2p->AnnounceUserIDCancel(auctionId));

    const std::vector<SentMessage> sent = SentMessages(peer);
    BOOST_REQUIRE_EQUAL(sent.size(), 2U);
    BOOST_CHECK_EQUAL(sent[0].command, "nsauccan");
    BOOST_CHECK_EQUAL(sent[1].command, "uidauccan");
    // The payload is the bare auction ID
    BOOST_CHECK(sent[0].payload == PayloadBytes(auctionId));
    BOOST_CHECK(sent[1].payload == PayloadBytes(auctionId));
}

BOOST_AUTO_TEST_CASE(announce_profile_update)
{
    CNode& peer = AddPeer();
    ReddIDProfile profile = TestProfile("alice", MakeKeyID());
    profile.socialData = "{\"x\":1}";
    BOOST_CHECK(p2p->AnnounceProfileUpdate("alice", profile));

    const std::vector<SentMessage> sent = SentMessages(peer);
    BOOST_REQUIRE_EQUAL(sent.size(), 1U);
    BOOST_CHECK_EQUAL(sent[0].command, "ridprofup");
    const CReddIDProfileUpdate msg = Decode<CReddIDProfileUpdate>(sent[0]);
    BOOST_CHECK_EQUAL(msg.timestamp, T0);
    BOOST_CHECK_EQUAL(msg.reddId, "alice");
    BOOST_CHECK(msg.profileHash == pMgr->CalculateProfileHash(profile));
    BOOST_CHECK(PayloadBytes(msg.profile) == PayloadBytes(profile));
}

BOOST_AUTO_TEST_CASE(send_namespace_config)
{
    CNode& peer = AddPeer(/* subscribe */ false);
    BOOST_CHECK(nsMgr->AddPricingTier(NS, 3, 7000 * COIN));
    BOOST_CHECK(nsMgr->AddPricingTier(NS, 6, 6000 * COIN));

    BOOST_CHECK(!p2p->SendNamespaceConfig(&peer, "unknown"));
    BOOST_CHECK(!p2p->SendNamespaceConfig(nullptr, NS));
    BOOST_CHECK(SentMessages(peer).empty());

    // The reply goes to the peer whether it is subscribed or not
    BOOST_CHECK(p2p->SendNamespaceConfig(&peer, NS));
    const std::vector<SentMessage> sent = SentMessages(peer);
    BOOST_REQUIRE_EQUAL(sent.size(), 1U);
    BOOST_CHECK_EQUAL(sent[0].command, "nsconres");
    const CNamespaceConfig msg = Decode<CNamespaceConfig>(sent[0]);
    BOOST_CHECK_EQUAL(msg.timestamp, T0);
    NamespaceInfo stored;
    BOOST_REQUIRE(nsMgr->GetNamespaceInfo(NS, stored));
    BOOST_CHECK(PayloadBytes(msg.config) == PayloadBytes(stored));
    BOOST_CHECK(PayloadBytes(msg.pricingTiers) == PayloadBytes(nsMgr->GetPricingTiers(NS)));
    BOOST_CHECK_EQUAL(msg.pricingTiers.size(), 2U);
}

// --- What the component managers announce ---

BOOST_AUTO_TEST_CASE(auction_manager_announces_user_id_auctions)
{
    CNode& peer = AddPeer();
    const CKeyID bidder = MakeKeyID();

    const uint256 auctionId = NewAuction("alice");
    BOOST_CHECK(SentCommands(peer) == std::vector<std::string>{"uidaucann"});
    BOOST_CHECK(Decode<CUserIDAuctionAnnounce>(SentMessages(peer)[0]).auctionId == auctionId);
    ClearSent(peer);

    uint256 bidId;
    BOOST_CHECK(aMgr->PlaceBid(auctionId, bidder, 6000 * COIN, bidId, 0));
    BOOST_CHECK(SentCommands(peer) == std::vector<std::string>{"uidaucbid"});
    const CAuctionBid bidMsg = Decode<CAuctionBid>(SentMessages(peer)[0]);
    BOOST_CHECK(bidMsg.auctionId == auctionId);
    BOOST_CHECK(bidMsg.bidId == bidId);
    BOOST_CHECK(bidMsg.bidder == bidder);
    BOOST_CHECK_EQUAL(bidMsg.bidAmount, 6000 * COIN);
    ClearSent(peer);

    // A refused bid is not announced
    BOOST_CHECK(!aMgr->PlaceBid(auctionId, MakeKeyID(), 6001 * COIN, bidId, 0));
    BOOST_CHECK(SentMessages(peer).empty());

    const uint256 second = NewAuction("bobby");
    ClearSent(peer);
    BOOST_CHECK(aMgr->CancelAuction(second, creator));
    BOOST_CHECK(SentCommands(peer) == std::vector<std::string>{"uidauccan"});
    BOOST_CHECK(SentMessages(peer)[0].payload == PayloadBytes(second));
}

BOOST_AUTO_TEST_CASE(namespace_manager_announces_namespace_auctions)
{
    CNode& peer = AddPeer();
    const CKeyID owner = MakeKeyID();
    const CKeyID bidder = MakeKeyID();

    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("fresh", owner, 20000 * COIN, 7, AUCTION_STANDARD, auctionId));
    BOOST_CHECK(SentCommands(peer) == std::vector<std::string>{"nsaucann"});
    const CNamespaceAuctionAnnounce announce = Decode<CNamespaceAuctionAnnounce>(SentMessages(peer)[0]);
    BOOST_CHECK(announce.auctionId == auctionId);
    BOOST_CHECK_EQUAL(announce.namespaceId, "fresh");
    BOOST_CHECK(announce.creator == owner);
    ClearSent(peer);

    uint256 bidId;
    BOOST_REQUIRE(nsMgr->BidOnNamespaceAuction(auctionId, bidder, 21000 * COIN, bidId));
    BOOST_CHECK(SentCommands(peer) == std::vector<std::string>{"nsaucbid"});
    const CAuctionBid bidMsg = Decode<CAuctionBid>(SentMessages(peer)[0]);
    BOOST_CHECK(bidMsg.auctionId == auctionId);
    BOOST_CHECK(bidMsg.bidId == bidId);
    BOOST_CHECK(bidMsg.bidder == bidder);
    ClearSent(peer);

    uint256 second;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("other", owner, 20000 * COIN, 7, AUCTION_STANDARD, second));
    ClearSent(peer);
    BOOST_CHECK(nsMgr->CancelNamespaceAuction(second, owner));
    BOOST_CHECK(SentCommands(peer) == std::vector<std::string>{"nsauccan"});
    BOOST_CHECK(SentMessages(peer)[0].payload == PayloadBytes(second));
}

// Current behaviour: NamespaceManager::UpdateNamespace announces the new
// configuration with SendNamespaceConfig(nullptr, ...), which sends nothing
// when it is given no peer. An updated configuration reaches no one.
BOOST_AUTO_TEST_CASE(namespace_update_is_not_announced)
{
    CNode& peer = AddPeer();
    NamespaceInfo config;
    BOOST_REQUIRE(nsMgr->GetNamespaceInfo(NS, config));
    config.maxLength = 20;
    {
        LogCounter failed("WARNING: NamespaceManager::UpdateNamespace: Failed to announce namespace update: " + NS);
        BOOST_CHECK(nsMgr->UpdateNamespace(config));
        BOOST_CHECK_EQUAL(failed.count, 1);
    }
    BOOST_CHECK(SentMessages(peer).empty());
}

BOOST_AUTO_TEST_CASE(profile_manager_announces_updates_of_existing_profiles)
{
    CNode& peer = AddPeer();
    ReddIDProfile profile = AddProfile("alice");
    // Creating a profile is not announced
    BOOST_CHECK(SentMessages(peer).empty());
    // Nor is the creation of a profile through UpdateProfile
    BOOST_CHECK(pMgr->UpdateProfile(TestProfile("bobby", MakeKeyID())));
    BOOST_CHECK(SentMessages(peer).empty());

    profile.bio = "changed";
    BOOST_CHECK(pMgr->UpdateProfile(profile));
    BOOST_CHECK(SentCommands(peer) == std::vector<std::string>{"ridprofup"});
    const CReddIDProfileUpdate msg = Decode<CReddIDProfileUpdate>(SentMessages(peer)[0]);
    BOOST_CHECK_EQUAL(msg.reddId, "alice");
    BOOST_CHECK_EQUAL(msg.profile.bio, "changed");
    BOOST_CHECK(msg.profileHash == pMgr->CalculateProfileHash(profile));
    ClearSent(peer);

    // A refused update is not announced
    profile.owner = MakeKeyID();
    BOOST_CHECK(!pMgr->UpdateProfile(profile));
    BOOST_CHECK(SentMessages(peer).empty());
}

BOOST_AUTO_TEST_SUITE_END()
