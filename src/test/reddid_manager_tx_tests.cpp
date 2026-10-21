// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <amount.h>
#include <crypto/common.h>
#include <id/auction.h>
#include <id/namespace.h>
#include <id/profile.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <id/transaction_handler.h>
#include <key.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/reddid_auction_test_util.h>
#include <test/reddid_profile_test_util.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/time.h>

#include <algorithm>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

using namespace reddid_test;

namespace {
/** A transaction with one zero-value output for each of the given scripts, spending an output nobody has. */
CMutableTransaction TxWith(const std::vector<CScript>& scripts)
{
    CMutableTransaction mtx;
    mtx.vin.emplace_back(COutPoint(InsecureRand256(), 0));
    for (const CScript& script : scripts) {
        mtx.vout.emplace_back(0, script);
    }
    return mtx;
}

/** A coinbase transaction that carries the given script in its only output. */
CMutableTransaction CoinbaseWith(const CScript& script)
{
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vout.emplace_back(0, script);
    assert(CTransaction(mtx).IsCoinBase());
    return mtx;
}

/** A coinstake transaction that carries the given script in its second output. */
CMutableTransaction CoinstakeWith(const CScript& script)
{
    CMutableTransaction mtx;
    mtx.vin.emplace_back(COutPoint(InsecureRand256(), 0));
    mtx.vout.resize(1);
    mtx.vout[0].SetEmpty();
    mtx.vout.emplace_back(0, script);
    assert(CTransaction(mtx).IsCoinStake());
    return mtx;
}

/** 'R' + OP_NAMESPACE_AUCTION_BID + auction ID + amount */
CScript NamespaceBidRecord(const uint256& auctionId, CAmount amount)
{
    std::vector<unsigned char> body(auctionId.begin(), auctionId.end());
    body.resize(40);
    WriteLE64(body.data() + 32, amount);
    return RecordScript(OP_NAMESPACE_AUCTION_BID, body);
}

/** A record of the auction manager that it rejects when processing and passes over when undoing. */
CScript RejectedAuctionRecord()
{
    return RecordScript(OP_AUCTION_CANCEL, uint256S("0bad"));
}
} // namespace

/**
 * ProfileTestingSetup with shorthands for namespace auctions and for handing
 * transactions to ReddIDManager, which passes them on to the namespace,
 * auction or profile manager.
 */
struct ManagerTxTestingSetup : public ProfileTestingSetup {
    NamespaceManager* nsMgr;

    ManagerTxTestingSetup() : nsMgr(reddidManager->GetNamespaceManager()) {}

    /** Create an auction for the namespace at its minimum price. The creator is the creator of the fixture. */
    uint256 NewNamespaceAuction(const std::string& namespaceId)
    {
        uint256 auctionId;
        bool created = nsMgr->CreateNamespaceAuction(namespaceId, creator, nsMgr->CalculateMinPrice(namespaceId), 3, AUCTION_STANDARD, auctionId);
        assert(created);
        return auctionId;
    }

    /**
     * A transaction that bids the minimum price on a namespace auction. The
     * namespace manager takes the bidder from the first output that pays to
     * a public key hash.
     */
    CMutableTransaction NamespaceBidTx(const uint256& auctionId, const CKeyID& bidder)
    {
        AuctionInfo auction;
        bool found = nsMgr->GetAuctionInfo(auctionId, auction);
        assert(found);
        return TxWith({NamespaceBidRecord(auctionId, auction.reservePrice), P2PKH(bidder)});
    }

    bool ManagerProcess(const CMutableTransaction& mtx, int height = 1)
    {
        return reddidManager->ProcessTransaction(CTransaction(mtx), height);
    }

    bool ManagerUndo(const CMutableTransaction& mtx, int height = 1)
    {
        return reddidManager->UndoTransaction(CTransaction(mtx), height);
    }

    AuctionState NamespaceAuctionState(const uint256& auctionId)
    {
        AuctionInfo auction;
        bool found = nsMgr->GetAuctionInfo(auctionId, auction);
        assert(found);
        return auction.state;
    }

    int64_t LastActivity(const std::string& reddId) { return GetProfile(reddId).lastActivityTime; }
};

/**
 * ReddIDManager::ProcessTransaction, UndoTransaction and UndoBlock, and
 * ProcessReddIDTransaction: which component manager a transaction is handed
 * to. What each component manager then does with a
 * record is covered by reddid_namespace_tests, reddid_auction_tx_tests and
 * reddid_profile_state_tests.
 *
 * Nothing here goes through block or mempool validation.
 */
BOOST_FIXTURE_TEST_SUITE(reddid_manager_tx_tests, ManagerTxTestingSetup)

// ===================================================================
// Category 1: The OP_RETURN members
// ===================================================================

BOOST_AUTO_TEST_CASE(op_return_members_forward_to_the_free_functions)
{
    uint256 id1 = uint256S("a1");
    uint256 id2 = uint256S("b2");
    CKeyID key = MakeKeyID();

    BOOST_CHECK(reddidManager->CreateNamespaceAuctionOpReturn("redd") == CreateNamespaceAuctionOpReturn("redd"));
    BOOST_CHECK(reddidManager->CreateUserIDAuctionOpReturn("alice", "redd") == CreateUserIDAuctionOpReturn("alice", "redd"));
    BOOST_CHECK(reddidManager->CreateUserIDBidOpReturn(id1, 6000 * COIN) == CreateUserIDBidOpReturn(id1, 6000 * COIN));
    BOOST_CHECK(reddidManager->CreateProfileUpdateOpReturn("alice", id1) == CreateProfileUpdateOpReturn("alice", id1));
    BOOST_CHECK(reddidManager->CreateConnectionOpReturn("alice", "bob", CONNECTION_ENDORSE) == CreateConnectionOpReturn("alice", "bob", CONNECTION_ENDORSE));
    BOOST_CHECK(reddidManager->CreateBidDepositOpReturn(id1, 6000 * COIN, 1, key) == CreateBidDepositOpReturn(id1, 6000 * COIN, 1, key));
    BOOST_CHECK(reddidManager->CreateAuctionSettleOpReturn(id1, id2) == CreateAuctionSettleOpReturn(id1, id2));

    // None of them is empty: a member that called itself would not return at all
    BOOST_CHECK(!reddidManager->CreateNamespaceAuctionOpReturn("redd").empty());
    BOOST_CHECK(!reddidManager->CreateAuctionSettleOpReturn(id1, id2).empty());
    // And a payload that is too long gives an empty script, as with the free function
    BOOST_CHECK(reddidManager->CreateUserIDAuctionOpReturn(std::string(70, 'a'), std::string(15, 'b')).empty());

    unsigned char opCode = 0;
    std::vector<unsigned char> data;
    BOOST_CHECK(reddidManager->ParseReddIDOpReturn(CreateAuctionSettleOpReturn(id1, id2), opCode, data));
    BOOST_CHECK_EQUAL(opCode, OP_AUCTION_SETTLE);
    BOOST_CHECK_EQUAL(data.size(), 64U);
    BOOST_CHECK(std::equal(id1.begin(), id1.end(), data.begin()));
    BOOST_CHECK(std::equal(id2.begin(), id2.end(), data.begin() + 32));
    BOOST_CHECK(!reddidManager->ParseReddIDOpReturn(P2PKH(key), opCode, data));
    BOOST_CHECK(!reddidManager->ParseReddIDOpReturn(CScript() << OP_RETURN << std::vector<unsigned char>{'X', OP_AUCTION_SETTLE}, opCode, data));
}

// ===================================================================
// Category 2: ProcessTransaction
// ===================================================================

BOOST_AUTO_TEST_CASE(namespace_records_reach_the_namespace_manager)
{
    uint256 bidAuction = NewNamespaceAuction("bidns");
    uint256 cancelAuction = NewNamespaceAuction("cancelns");
    CKeyID bidder = MakeKeyID();

    BOOST_CHECK(ManagerProcess(NamespaceBidTx(bidAuction, bidder), 5));

    std::vector<BidInfo> bids = nsMgr->GetAuctionBids(bidAuction);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK(bids[0].bidder == bidder);
    BOOST_CHECK_EQUAL(bids[0].bidAmount, nsMgr->CalculateMinPrice("bidns"));
    BOOST_CHECK_EQUAL(NamespaceAuctionState(bidAuction), AUCTION_ACTIVE);

    BOOST_CHECK(ManagerProcess(TxWith({RecordScript(OP_NAMESPACE_AUCTION_CANCEL, cancelAuction), P2PKH(creator)}), 6));
    BOOST_CHECK_EQUAL(NamespaceAuctionState(cancelAuction), AUCTION_CANCELED);

    // A create record is passed on as well; see reddid_namespace_tests,
    // process_create_tx_never_creates_an_auction, for why nothing comes of it
    BOOST_CHECK(ManagerProcess(TxWith({CreateNamespaceAuctionOpReturn("createns"), P2PKH(creator)}), 7));
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("createns"));

    // The auction manager was not involved: it holds no auction with a bid.
    // (Its lookups by auction ID fall back to the database, which the two
    // managers share, so they do find the namespace auctions.)
    BOOST_CHECK(aMgr->GetActiveAuctions().empty());
    BOOST_CHECK(aMgr->GetAuctionsByNamespace("bidns").empty());
}

BOOST_AUTO_TEST_CASE(auction_records_reach_the_auction_manager)
{
    // OP_AUCTION_BID
    uint256 legacyAuction = NewAuction("carol");
    CKeyID legacyBidder = MakeKeyID();
    BOOST_CHECK(ManagerProcess(TxFrom(legacyBidder, reddidManager->CreateUserIDBidOpReturn(legacyAuction, 6000 * COIN)), 5));
    std::vector<BidInfo> bids = reddidManager->GetAuctionBids(legacyAuction);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK(bids[0].bidder == legacyBidder);

    // OP_AUCTION_CANCEL
    uint256 canceledAuction = NewAuction("bob");
    BOOST_CHECK(ManagerProcess(TxFrom(creator, RecordScript(OP_AUCTION_CANCEL, canceledAuction)), 5));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, canceledAuction).state, AUCTION_CANCELED);

    // OP_BID_DEPOSIT, OP_AUCTION_FINALIZE and OP_AUCTION_SETTLE: an auction from the first bid to the registered name
    uint256 auctionId = NewAuction("alice");
    DepositBid bid = MakeDepositBid(auctionId, MakeKeyID(), 8000 * COIN, 1200 * COIN);
    BOOST_CHECK(ManagerProcess(bid.tx, 5));
    BOOST_CHECK(aMgr->IsDepositLocked(bid.utxo));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).currentBid, 8000 * COIN);

    EndAuction(auctionId);
    BOOST_CHECK(ManagerProcess(TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, auctionId)), 6));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_SETTLEMENT);

    CMutableTransaction settleTx = TxFrom(MakeKeyID(), reddidManager->CreateAuctionSettleOpReturn(auctionId, bid.bidId));
    settleTx.vin.emplace_back(bid.utxo);
    BOOST_CHECK(ManagerProcess(settleTx, 7));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_COMPLETED);
    UserIDInfo userId;
    BOOST_REQUIRE(reddidManager->GetUserIDInfo("alice", NS, userId));
    BOOST_CHECK(userId.owner == bid.bidder);

    // The namespace manager was not involved: it holds no auction with a bid
    BOOST_CHECK(nsMgr->GetActiveAuctions().empty());
}

// OP_AUCTION_DEFAULT and OP_DEPOSIT_FORFEIT go to the auction manager too:
// the winner of an auction does not settle, someone records the default, and
// the lock on the forfeited deposit is then removed. With a settlement window
// of 0 blocks at height 0 the default can be recorded at once (see
// reddid_auction_tx_tests, default_record_forfeits_deposit_and_creates_re_auction).
BOOST_AUTO_TEST_CASE(default_and_forfeit_records_reach_the_auction_manager)
{
    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    config.settlementWindowBlocks = 0;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    uint256 auctionId = NewAuction("alice");
    DepositBid bid = MakeDepositBid(auctionId, MakeKeyID(), 8000 * COIN, 1200 * COIN);
    BOOST_REQUIRE(ManagerProcess(bid.tx));
    EndAuction(auctionId);
    BOOST_REQUIRE(ManagerProcess(TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, auctionId))));
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_SETTLEMENT);

    BOOST_CHECK(ManagerProcess(TxFrom(MakeKeyID(), RecordScript(OP_AUCTION_DEFAULT, auctionId, bid.bidId))));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_DEFAULTED);
    BOOST_CHECK_EQUAL(GetLock(aMgr, bid.utxo).state, DEPOSIT_FORFEITED);

    BOOST_CHECK(ManagerProcess(TxFrom(MakeKeyID(), RecordScript(OP_DEPOSIT_FORFEIT, auctionId, bid.bidId))));
    DepositLock lock;
    BOOST_CHECK(!aMgr->GetDepositLock(bid.utxo, lock));
}

BOOST_AUTO_TEST_CASE(profile_records_reach_the_profile_manager)
{
    AddProfile("alice");
    AddProfile("bob");
    BOOST_REQUIRE_EQUAL(LastActivity("alice"), T0);

    SetMockTime(T0 + 100);
    BOOST_CHECK(ManagerProcess(TxWith({reddidManager->CreateProfileUpdateOpReturn("alice", uint256S("01"))}), 7));
    BOOST_CHECK_EQUAL(LastActivity("alice"), T0 + 100);
    BOOST_CHECK_EQUAL(LastActivity("bob"), T0);

    SetMockTime(T0 + 200);
    BOOST_CHECK(ManagerProcess(TxWith({reddidManager->CreateConnectionOpReturn("bob", "alice", CONNECTION_FOLLOW)}), 8));
    BOOST_CHECK_EQUAL(LastActivity("bob"), T0 + 200);
    BOOST_CHECK_EQUAL(LastActivity("alice"), T0 + 100);
    BOOST_CHECK_EQUAL(GetStoredProfile("bob").lastActivityTime, T0 + 200);

    // The three placeholder opcodes are accepted and change nothing
    SetMockTime(T0 + 300);
    for (unsigned char opCode : {OP_REDDID_AUCTION_CREATE, OP_REDDID_AUCTION_BID, OP_REDDID_AUCTION_FINALIZE}) {
        BOOST_CHECK(ManagerProcess(TxWith({RecordScript(opCode, std::vector<unsigned char>{5, 'a', 'l', 'i', 'c', 'e'})}), 9));
    }
    BOOST_CHECK_EQUAL(LastActivity("alice"), T0 + 100);
}

BOOST_AUTO_TEST_CASE(coinbase_and_coinstake_are_skipped)
{
    AddProfile("alice");
    uint256 auctionId = NewNamespaceAuction("skipns");
    CScript profileRecord = CreateProfileUpdateOpReturn("alice", uint256());
    CScript bidRecord = NamespaceBidRecord(auctionId, nsMgr->CalculateMinPrice("skipns"));
    SetMockTime(T0 + 100);

    BOOST_CHECK(ManagerProcess(CoinbaseWith(profileRecord), 5));
    BOOST_CHECK(ManagerProcess(CoinstakeWith(profileRecord), 5));
    CMutableTransaction coinstakeBid = CoinstakeWith(bidRecord);
    coinstakeBid.vout.emplace_back(0, P2PKH(MakeKeyID()));
    BOOST_CHECK(ManagerProcess(coinstakeBid, 5));
    BOOST_CHECK_EQUAL(LastActivity("alice"), T0);
    BOOST_CHECK(nsMgr->GetAuctionBids(auctionId).empty());

    // The same records in an ordinary transaction are applied
    BOOST_CHECK(ManagerProcess(TxWith({profileRecord}), 5));
    BOOST_CHECK_EQUAL(LastActivity("alice"), T0 + 100);
    BOOST_CHECK(ManagerProcess(TxWith({bidRecord, P2PKH(MakeKeyID())}), 5));
    BOOST_CHECK_EQUAL(nsMgr->GetAuctionBids(auctionId).size(), 1U);
}

// ProcessReddIDTransaction is what block validation will call
BOOST_AUTO_TEST_CASE(process_reddid_transaction_hands_the_transaction_to_the_manager)
{
    AddProfile("alice");
    CScript record = CreateProfileUpdateOpReturn("alice", uint256());
    SetMockTime(T0 + 100);

    // Coinbase and coinstake are skipped before the manager is looked at
    BOOST_CHECK(ProcessReddIDTransaction(CTransaction(CoinbaseWith(record)), 5, nullptr));
    BOOST_CHECK(ProcessReddIDTransaction(CTransaction(CoinstakeWith(record)), 5, nullptr));
    BOOST_CHECK(ProcessReddIDTransaction(CTransaction(CoinbaseWith(record)), 5, reddidManager.get()));
    BOOST_CHECK(ProcessReddIDTransaction(CTransaction(CoinstakeWith(record)), 5, reddidManager.get()));
    BOOST_CHECK_EQUAL(LastActivity("alice"), T0);

    // Without a manager any other transaction is refused, with or without a record
    BOOST_CHECK(!ProcessReddIDTransaction(CTransaction(TxWith({record})), 5, nullptr));
    BOOST_CHECK(!ProcessReddIDTransaction(CTransaction(TxWith({P2PKH(MakeKeyID())})), 5, nullptr));
    BOOST_CHECK_EQUAL(LastActivity("alice"), T0);

    BOOST_CHECK(ProcessReddIDTransaction(CTransaction(TxWith({record})), 5, reddidManager.get()));
    BOOST_CHECK_EQUAL(LastActivity("alice"), T0 + 100);
    BOOST_CHECK(ProcessReddIDTransaction(CTransaction(TxWith({P2PKH(MakeKeyID())})), 5, reddidManager.get()));
}

// Current behaviour: ProcessTransaction returns true whatever the transaction
// holds and whatever became of its records. The caller cannot tell a record
// that was applied from one that was rejected or from no record at all.
BOOST_AUTO_TEST_CASE(process_transaction_returns_true_whatever_happens)
{
    uint256 auctionId = NewAuction("alice");
    uint256 nsAuction = NewNamespaceAuction("truens");

    // No record
    BOOST_CHECK(ManagerProcess(TxWith({})));
    BOOST_CHECK(ManagerProcess(TxWith({P2PKH(MakeKeyID())})));
    BOOST_CHECK(ManagerProcess(TxWith({CScript() << OP_RETURN})));
    BOOST_CHECK(ManagerProcess(TxWith({CScript() << OP_RETURN << std::vector<unsigned char>{'X', OP_AUCTION_BID, 1, 2, 3}})));
    // An opcode that no manager knows
    BOOST_CHECK(ManagerProcess(TxWith({RecordScript('Z', std::vector<unsigned char>{1, 2, 3})})));
    // Records without a body
    for (unsigned char opCode : {OP_NAMESPACE_AUCTION_CREATE, OP_NAMESPACE_AUCTION_BID, OP_AUCTION_CREATE, OP_AUCTION_BID,
                                 OP_BID_DEPOSIT, OP_AUCTION_SETTLE, OP_REDDID_PROFILE_UPDATE, OP_REDDID_CONNECTION}) {
        BOOST_CHECK(ManagerProcess(TxWith({RecordScript(opCode, std::vector<unsigned char>{})})));
        BOOST_CHECK(ManagerProcess(TxFrom(creator, RecordScript(opCode, std::vector<unsigned char>{}))));
    }
    // Records that the component manager rejects: no sender, a bid below the
    // reserve price, a cancellation by someone else, an unknown auction
    BOOST_CHECK(ManagerProcess(TxWith({CreateUserIDBidOpReturn(auctionId, 6000 * COIN)})));
    BOOST_CHECK(ManagerProcess(TxFrom(MakeKeyID(), CreateUserIDBidOpReturn(auctionId, 1 * COIN))));
    BOOST_CHECK(ManagerProcess(TxFrom(MakeKeyID(), RecordScript(OP_AUCTION_CANCEL, auctionId))));
    BOOST_CHECK(ManagerProcess(TxWith({RejectedAuctionRecord()})));
    BOOST_CHECK(ManagerProcess(TxWith({NamespaceBidRecord(nsAuction, 1 * COIN), P2PKH(MakeKeyID())})));
    BOOST_CHECK(ManagerProcess(TxWith({NamespaceBidRecord(uint256S("0bad"), 100000 * COIN), P2PKH(MakeKeyID())})));

    // No bid was placed and nothing was cancelled. (Both auctions are active
    // all the same: a rejected bid activates a pending auction, see
    // reddid_auction_tests and reddid_namespace_tests.)
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).currentBid, 0);
    BOOST_CHECK(GetAuction(aMgr, auctionId).state != AUCTION_CANCELED);
    BOOST_CHECK(nsMgr->GetAuctionBids(nsAuction).empty());
}

// SUSPECTED BUG (documented, not fixed): a transaction is handed to the
// component manager of its first record, and if that manager applies a
// record the rest of the transaction is not looked at. Records for the other
// managers further down are dropped without a trace.
BOOST_AUTO_TEST_CASE(second_record_is_dropped_when_the_first_is_applied)
{
    AddProfile("alice");
    AddProfile("bob");
    uint256 first = NewNamespaceAuction("firstns");
    uint256 second = NewNamespaceAuction("secondns");
    CKeyID bidder = MakeKeyID();
    SetMockTime(T0 + 100);

    // Profile record first: the namespace bid is dropped
    CMutableTransaction profileFirst = NamespaceBidTx(first, bidder);
    profileFirst.vout.insert(profileFirst.vout.begin(), CTxOut(0, CreateProfileUpdateOpReturn("alice", uint256())));
    BOOST_CHECK(ManagerProcess(profileFirst));
    BOOST_CHECK_EQUAL(LastActivity("alice"), T0 + 100);
    BOOST_CHECK(nsMgr->GetAuctionBids(first).empty());

    // Namespace bid first: the profile record is dropped
    CMutableTransaction bidFirst = NamespaceBidTx(second, bidder);
    bidFirst.vout.emplace_back(0, CreateProfileUpdateOpReturn("bob", uint256()));
    BOOST_CHECK(ManagerProcess(bidFirst));
    BOOST_CHECK_EQUAL(nsMgr->GetAuctionBids(second).size(), 1U);
    BOOST_CHECK_EQUAL(LastActivity("bob"), T0);

    // Two records for the same manager: it applies the first and stops as well
    CMutableTransaction twoBids = NamespaceBidTx(first, bidder);
    uint256 third = NewNamespaceAuction("thirdns");
    twoBids.vout.emplace_back(0, NamespaceBidRecord(third, nsMgr->CalculateMinPrice("thirdns")));
    BOOST_CHECK(ManagerProcess(twoBids));
    BOOST_CHECK_EQUAL(nsMgr->GetAuctionBids(first).size(), 1U);
    BOOST_CHECK(nsMgr->GetAuctionBids(third).empty());
}

// When the manager of the first record applies nothing, the transaction goes
// to the manager of the next record.
BOOST_AUTO_TEST_CASE(second_record_is_applied_when_the_first_is_rejected)
{
    AddProfile("alice");
    uint256 nsAuction = NewNamespaceAuction("nextns");
    CKeyID bidder = MakeKeyID();
    SetMockTime(T0 + 100);

    CMutableTransaction rejectedThenBid = NamespaceBidTx(nsAuction, bidder);
    rejectedThenBid.vout.insert(rejectedThenBid.vout.begin(), CTxOut(0, RejectedAuctionRecord()));
    BOOST_CHECK(ManagerProcess(rejectedThenBid));
    BOOST_CHECK_EQUAL(nsMgr->GetAuctionBids(nsAuction).size(), 1U);

    // An unknown opcode and a record that is no ReddID record are passed over in the same way
    CMutableTransaction unknownThenProfile = TxWith({RecordScript('Z', std::vector<unsigned char>{1, 2, 3}),
                                                     CScript() << OP_RETURN << std::vector<unsigned char>{'X', 'Y', 'Z'},
                                                     P2PKH(MakeKeyID()),
                                                     CreateProfileUpdateOpReturn("alice", uint256())});
    BOOST_CHECK(ManagerProcess(unknownThenProfile));
    BOOST_CHECK_EQUAL(LastActivity("alice"), T0 + 100);
}

// ===================================================================
// Category 3: UndoTransaction
// ===================================================================

BOOST_AUTO_TEST_CASE(undo_reaches_the_manager_of_the_record)
{
    // Namespace manager
    uint256 nsAuction = NewNamespaceAuction("undons");
    CMutableTransaction nsBid = NamespaceBidTx(nsAuction, MakeKeyID());
    BOOST_REQUIRE(ManagerProcess(nsBid, 5));
    BOOST_REQUIRE_EQUAL(nsMgr->GetAuctionBids(nsAuction).size(), 1U);
    BOOST_CHECK(ManagerUndo(nsBid, 5));
    BOOST_CHECK(nsMgr->GetAuctionBids(nsAuction).empty());
    BOOST_CHECK_EQUAL(NamespaceAuctionState(nsAuction), AUCTION_PENDING);

    // Auction manager
    uint256 auctionId = NewAuction("alice");
    CMutableTransaction legacyBid = TxFrom(MakeKeyID(), CreateUserIDBidOpReturn(auctionId, 6000 * COIN));
    DepositBid depositBid = MakeDepositBid(auctionId, MakeKeyID(), 8000 * COIN, 1200 * COIN);
    BOOST_REQUIRE(ManagerProcess(legacyBid, 5));
    BOOST_REQUIRE(ManagerProcess(depositBid.tx, 6));
    BOOST_REQUIRE(aMgr->IsDepositLocked(depositBid.utxo));
    BOOST_CHECK(ManagerUndo(depositBid.tx, 6));
    BOOST_CHECK(!aMgr->IsDepositLocked(depositBid.utxo));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).currentBid, 6000 * COIN);
    BOOST_CHECK(ManagerUndo(legacyBid, 5));
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_PENDING);

    // Profile manager
    AddProfile("bob");
    AddProfile("carol");
    Connect("bob", "carol", CONNECTION_FOLLOW);
    BOOST_CHECK(ManagerUndo(TxWith({CreateConnectionOpReturn("bob", "carol", CONNECTION_FOLLOW)}), 5));
    BOOST_CHECK(pMgr->GetConnections("bob").empty());
    BOOST_CHECK(pMgr->ProfileExists("bob"));
}

BOOST_AUTO_TEST_CASE(undo_transaction_returns_true_whatever_happens)
{
    uint256 auctionId = NewAuction("alice");
    uint256 bidId;
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, MakeKeyID(), 6000 * COIN, bidId));

    BOOST_CHECK(ManagerUndo(TxWith({})));
    BOOST_CHECK(ManagerUndo(TxWith({P2PKH(MakeKeyID())})));
    BOOST_CHECK(ManagerUndo(TxWith({CScript() << OP_RETURN})));
    BOOST_CHECK(ManagerUndo(TxWith({RecordScript('Z', std::vector<unsigned char>{1, 2, 3})})));
    for (unsigned char opCode : {OP_NAMESPACE_AUCTION_CREATE, OP_NAMESPACE_AUCTION_BID, OP_AUCTION_CREATE, OP_AUCTION_BID,
                                 OP_BID_DEPOSIT, OP_AUCTION_SETTLE, OP_REDDID_PROFILE_UPDATE, OP_REDDID_CONNECTION}) {
        BOOST_CHECK(ManagerUndo(TxWith({RecordScript(opCode, std::vector<unsigned char>{})})));
    }
    BOOST_CHECK(ManagerUndo(TxWith({RejectedAuctionRecord()})));
    BOOST_CHECK(ManagerUndo(TxWith({NamespaceBidRecord(uint256S("0bad"), 100000 * COIN)})));
    // Coinbase and coinstake are skipped by UndoBlock, not here
    BOOST_CHECK(ManagerUndo(CoinbaseWith(CScript() << OP_TRUE)));

    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).currentBid, 6000 * COIN);
    BOOST_CHECK_EQUAL(aMgr->GetAuctionBids(auctionId).size(), 1U);
}

// SUSPECTED BUG (documented, not fixed): undoing a transaction hands it to
// the component manager of its first record only, because every component
// manager reports an undo as done whether or not it had anything to undo.
// Processing goes on to the next record when the first is rejected (see
// second_record_is_applied_when_the_first_is_rejected), so what such a
// transaction did stays in place when its block is disconnected.
BOOST_AUTO_TEST_CASE(undo_stops_at_the_first_record_where_processing_went_on)
{
    uint256 nsAuction = NewNamespaceAuction("staysns");
    CMutableTransaction rejectedThenBid = NamespaceBidTx(nsAuction, MakeKeyID());
    rejectedThenBid.vout.insert(rejectedThenBid.vout.begin(), CTxOut(0, RejectedAuctionRecord()));
    BOOST_REQUIRE(ManagerProcess(rejectedThenBid, 5));
    BOOST_REQUIRE_EQUAL(nsMgr->GetAuctionBids(nsAuction).size(), 1U);

    BOOST_CHECK(ManagerUndo(rejectedThenBid, 5));

    BOOST_CHECK_EQUAL(nsMgr->GetAuctionBids(nsAuction).size(), 1U);
    BOOST_CHECK_EQUAL(NamespaceAuctionState(nsAuction), AUCTION_ACTIVE);

    // The namespace manager, given the transaction directly, undoes the bid
    BOOST_CHECK(nsMgr->UndoTransaction(CTransaction(rejectedThenBid), 5));
    BOOST_CHECK(nsMgr->GetAuctionBids(nsAuction).empty());
}

// ===================================================================
// Category 4: UndoBlock
// ===================================================================

BOOST_AUTO_TEST_CASE(undo_block_undoes_every_transaction_but_coinbase_and_coinstake)
{
    AddProfile("alice");
    AddProfile("bob");
    AddProfile("carol");
    AddProfile("dave");
    Connect("carol", "dave", CONNECTION_FOLLOW);
    uint256 nsAuction = NewNamespaceAuction("blockns");
    uint256 auctionId = NewAuction("erin");
    CMutableTransaction nsBid = NamespaceBidTx(nsAuction, MakeKeyID());
    CMutableTransaction auctionBid = TxFrom(MakeKeyID(), CreateUserIDBidOpReturn(auctionId, 6000 * COIN));
    CMutableTransaction connection = TxWith({CreateConnectionOpReturn("carol", "dave", CONNECTION_FOLLOW)});

    CBlock block;
    block.vtx.push_back(MakeTransactionRef(CoinbaseWith(CreateProfileUpdateOpReturn("alice", uint256()))));
    block.vtx.push_back(MakeTransactionRef(CoinstakeWith(CreateProfileUpdateOpReturn("bob", uint256()))));
    block.vtx.push_back(MakeTransactionRef(nsBid));
    block.vtx.push_back(MakeTransactionRef(auctionBid));
    block.vtx.push_back(MakeTransactionRef(connection));
    for (const CTransactionRef& tx : block.vtx) {
        BOOST_REQUIRE(reddidManager->ProcessTransaction(*tx, 9));
    }
    BOOST_REQUIRE_EQUAL(nsMgr->GetAuctionBids(nsAuction).size(), 1U);
    BOOST_REQUIRE_EQUAL(aMgr->GetAuctionBids(auctionId).size(), 1U);

    BOOST_CHECK(reddidManager->UndoBlock(block, 9));

    BOOST_CHECK(nsMgr->GetAuctionBids(nsAuction).empty());
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
    BOOST_CHECK(pMgr->GetConnections("carol").empty());
    // Undoing a profile record erases the profile (see reddid_profile_state_tests);
    // the two in the coinbase and the coinstake were not undone
    BOOST_CHECK(pMgr->ProfileExists("alice"));
    BOOST_CHECK(pMgr->ProfileExists("bob"));

    // An empty block and a block of coinbase and coinstake alone
    BOOST_CHECK(reddidManager->UndoBlock(CBlock(), 9));
    block.vtx.resize(2);
    BOOST_CHECK(reddidManager->UndoBlock(block, 9));
    BOOST_CHECK(pMgr->ProfileExists("alice"));
    BOOST_CHECK(pMgr->ProfileExists("bob"));
}

// SUSPECTED BUG (documented, not fixed): UndoBlock undoes the records of
// every transaction in the block, whether or not processing the block applied
// them, and the component managers undo by what a record names, not by what
// the transaction did. Disconnecting a block can therefore take away state
// that the block never created: here an auction that existed before the
// block, named by a create record that the block's processing rejected, and
// a profile whose record merely noted that it was active.
BOOST_AUTO_TEST_CASE(undo_block_takes_away_what_the_block_never_created)
{
    uint256 auctionId = NewAuction("alice");
    uint256 bidId;
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, MakeKeyID(), 6000 * COIN, bidId));
    ReddIDProfile profile = AddProfile("bob");

    CMutableTransaction rejectedCreate = TxFrom(MakeKeyID(), CreateUserIDAuctionOpReturn("alice", NS));
    CMutableTransaction activity = TxWith({CreateProfileUpdateOpReturn("bob", uint256())});
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(CoinbaseWith(CScript() << OP_TRUE)));
    block.vtx.push_back(MakeTransactionRef(rejectedCreate));
    block.vtx.push_back(MakeTransactionRef(activity));

    // Processing the block: the create record is rejected, the profile record changes nothing but a timestamp
    for (const CTransactionRef& tx : block.vtx) {
        BOOST_REQUIRE(reddidManager->ProcessTransaction(*tx, 9));
    }
    std::vector<uint256> auctionIds;
    BOOST_REQUIRE(db->ListAuctions(auctionIds));
    BOOST_REQUIRE_EQUAL(auctionIds.size(), 1U);
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).currentBid, 6000 * COIN);
    BOOST_REQUIRE(pMgr->CalculateProfileHash(GetProfile("bob")) == pMgr->CalculateProfileHash(profile));

    BOOST_CHECK(reddidManager->UndoBlock(block, 9));

    AuctionInfo info;
    BOOST_CHECK(!aMgr->GetAuctionInfo(auctionId, info));
    BOOST_CHECK(!db->ReadAuction(auctionId, info));
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
    BOOST_CHECK(!pMgr->ProfileExists("bob"));
    ReddIDProfile stored;
    BOOST_CHECK(!db->ReadProfile("bob", stored));
}

// Current behaviour: UndoBlock does not look at whether the manager is
// running. A block that is undone between Interrupt() and the destruction of
// the manager is undone like any other.
BOOST_AUTO_TEST_CASE(undo_block_works_on_an_interrupted_manager)
{
    uint256 nsAuction = NewNamespaceAuction("haltns");
    CMutableTransaction nsBid = NamespaceBidTx(nsAuction, MakeKeyID());
    BOOST_REQUIRE(ManagerProcess(nsBid, 5));
    BOOST_REQUIRE_EQUAL(nsMgr->GetAuctionBids(nsAuction).size(), 1U);
    BOOST_REQUIRE_EQUAL(NamespaceAuctionState(nsAuction), AUCTION_ACTIVE);

    CBlock block;
    block.vtx.push_back(MakeTransactionRef(CoinbaseWith(CScript() << OP_TRUE)));
    block.vtx.push_back(MakeTransactionRef(nsBid));

    reddidManager->Interrupt();
    BOOST_CHECK(reddidManager->UndoBlock(block, 5));

    BOOST_CHECK(nsMgr->GetAuctionBids(nsAuction).empty());
    BOOST_CHECK_EQUAL(NamespaceAuctionState(nsAuction), AUCTION_PENDING);
}

BOOST_AUTO_TEST_SUITE_END()
