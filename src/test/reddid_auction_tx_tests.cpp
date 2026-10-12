// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <amount.h>
#include <coins.h>
#include <id/auction.h>
#include <id/namespace.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <id/transaction_handler.h>
#include <key.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
#include <sync.h>
#include <test/reddid_auction_test_util.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

using namespace reddid_test;

/**
 * AuctionManager::ProcessTransaction, GetTransactionSender and
 * UndoTransaction, driven with hand-built transactions.
 *
 * Nothing here goes through block or mempool validation: the transactions
 * are passed to the auction manager directly, and the outputs they spend are
 * placed in the UTXO set by the test (see AuctionTestingSetup).
 */
BOOST_FIXTURE_TEST_SUITE(reddid_auction_tx_tests, AuctionTestingSetup)

// ===================================================================
// Category 1: The sender of a transaction
// ===================================================================

BOOST_AUTO_TEST_CASE(sender_is_owner_of_first_p2pkh_input)
{
    CKeyID keyA = MakeKeyID();
    CKeyID keyB = MakeKeyID();
    CKeyID sender;

    // No inputs at all
    BOOST_CHECK(!aMgr->GetTransactionSender(CTransaction(CMutableTransaction()), sender));

    // An input that is not in the UTXO set
    CMutableTransaction unknownInput;
    unknownInput.vin.emplace_back(COutPoint(InsecureRand256(), 0));
    BOOST_CHECK(!aMgr->GetTransactionSender(CTransaction(unknownInput), sender));
    BOOST_CHECK(sender.IsNull());

    // A pay-to-pubkey-hash output names its owner
    CMutableTransaction fromA;
    fromA.vin.emplace_back(FundKey(chainstate, keyA));
    BOOST_CHECK(aMgr->GetTransactionSender(CTransaction(fromA), sender));
    BOOST_CHECK(sender == keyA);

    // Script-hash and witness outputs do not
    COutPoint scriptHashCoin = AddCoin(chainstate, GetScriptForDestination(ScriptHash(P2PKH(keyA))));
    COutPoint witnessCoin = AddCoin(chainstate, GetScriptForDestination(WitnessV0KeyHash(keyA)));
    CMutableTransaction noKeyHash;
    noKeyHash.vin.emplace_back(scriptHashCoin);
    noKeyHash.vin.emplace_back(witnessCoin);
    sender.SetNull();
    BOOST_CHECK(!aMgr->GetTransactionSender(CTransaction(noKeyHash), sender));
    BOOST_CHECK(sender.IsNull());

    // Inputs without a usable output are skipped; the first usable one wins
    CMutableTransaction mixed;
    mixed.vin.emplace_back(COutPoint(InsecureRand256(), 3));
    mixed.vin.emplace_back(scriptHashCoin);
    mixed.vin.emplace_back(FundKey(chainstate, keyB));
    mixed.vin.emplace_back(FundKey(chainstate, keyA));
    BOOST_CHECK(aMgr->GetTransactionSender(CTransaction(mixed), sender));
    BOOST_CHECK(sender == keyB);

    // A spent output is no longer found
    {
        LOCK(cs_main);
        BOOST_REQUIRE(chainstate.CoinsTip().SpendCoin(fromA.vin[0].prevout));
    }
    sender.SetNull();
    BOOST_CHECK(!aMgr->GetTransactionSender(CTransaction(fromA), sender));

    // Without a chain state there is no sender
    aMgr->SetChainState(nullptr);
    BOOST_CHECK(!aMgr->GetTransactionSender(CTransaction(mixed), sender));
    aMgr->SetChainState(&chainstate);
    BOOST_CHECK(aMgr->GetTransactionSender(CTransaction(mixed), sender));
}

BOOST_AUTO_TEST_CASE(process_transaction_needs_a_sender_and_a_chain_state)
{
    uint256 auctionId = NewAuction("alice");
    CScript bidRecord = CreateUserIDBidOpReturn(auctionId, 6000 * COIN);

    // The input is not in the UTXO set, so the sender is unknown
    CMutableTransaction noSender = RecordTx(COutPoint(InsecureRand256(), 0), bidRecord);
    BOOST_CHECK(!Process(noSender));
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_PENDING);

    CMutableTransaction withSender = TxFrom(MakeKeyID(), bidRecord);

    aMgr->SetChainState(nullptr);
    BOOST_CHECK(!Process(withSender));
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
    aMgr->SetChainState(&chainstate);

    BOOST_CHECK(Process(withSender));
    BOOST_CHECK_EQUAL(aMgr->GetAuctionBids(auctionId).size(), 1U);
}

// Current behaviour: the result does not tell "not an auction transaction"
// from "auction transaction that was rejected"; both are false.
BOOST_AUTO_TEST_CASE(process_transaction_without_auction_record_returns_false)
{
    CKeyID sender = MakeKeyID();

    CMutableTransaction payment;
    payment.vin.emplace_back(FundKey(chainstate, sender));
    payment.vout.emplace_back(5 * COIN, P2PKH(MakeKeyID()));
    BOOST_CHECK(!Process(payment));

    // An OP_RETURN that is not a ReddID record
    BOOST_CHECK(!Process(TxFrom(sender, CScript() << OP_RETURN << std::vector<unsigned char>{'X', 'Y', 'Z'})));
    // A ReddID record with an operation the auction manager does not handle
    BOOST_CHECK(!Process(TxFrom(sender, RecordScript(OP_REDDID_PROFILE_UPDATE, uint256S("01")))));
    // A bare 'R' + opcode header
    BOOST_CHECK(!Process(TxFrom(sender, RecordScript(OP_AUCTION_CANCEL, std::vector<unsigned char>{}))));
}

// ===================================================================
// Category 2: OP_AUCTION_CREATE
// ===================================================================

// SUSPECTED BUG (documented, not fixed): the auction that an OP_AUCTION_CREATE
// record creates always has a reserve price of 1,000 RDD. The minimum for a
// user ID is 5,000 RDD unless the namespace has a pricing tier that lowers
// it, so in a namespace without such a tier the record cannot create an
// auction.
BOOST_AUTO_TEST_CASE(create_record_cannot_create_an_auction_at_the_default_minimum_price)
{
    CKeyID sender = MakeKeyID();
    BOOST_CHECK_EQUAL(reddidManager->CalculateMinPriceForUserID("alice", NS), 5000 * COIN);

    BOOST_CHECK(!Process(TxFrom(sender, CreateUserIDAuctionOpReturn("alice", NS))));

    std::vector<uint256> auctionIds;
    BOOST_REQUIRE(db->ListAuctions(auctionIds));
    BOOST_CHECK(auctionIds.empty());
    AuctionInfo info;
    BOOST_CHECK(!aMgr->GetAuctionInfo(ExpectedAuctionId("alice", NS, T0), info));
}

BOOST_AUTO_TEST_CASE(create_record_creates_an_auction_when_a_pricing_tier_allows_1000_rdd)
{
    CKeyID sender = MakeKeyID();
    BOOST_REQUIRE(reddidManager->GetNamespaceManager()->AddPricingTier(NS, 3, 1000 * COIN));

    CMutableTransaction createTx = TxFrom(sender, CreateUserIDAuctionOpReturn("alice", NS));
    // The height passed in is not used: the auction records the height of the chain tip
    BOOST_CHECK(Process(createTx, 77));

    uint256 auctionId = ExpectedAuctionId("alice", NS, T0);
    AuctionInfo info;
    BOOST_REQUIRE(aMgr->GetAuctionInfo(auctionId, info));
    BOOST_CHECK(info.auctionId == auctionId);
    BOOST_CHECK_EQUAL(info.name, "alice");
    BOOST_CHECK_EQUAL(info.namespaceId, NS);
    BOOST_CHECK(info.creator == sender);
    BOOST_CHECK_EQUAL(info.startTime, T0);
    BOOST_CHECK_EQUAL(info.endTime, T0 + 7 * DAY);
    BOOST_CHECK_EQUAL(info.reservePrice, 1000 * COIN);
    BOOST_CHECK_EQUAL(info.depositAmount, 100 * COIN);
    BOOST_CHECK_EQUAL(info.currentBid, 0);
    BOOST_CHECK_EQUAL(info.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(info.type, AUCTION_STANDARD);
    BOOST_CHECK(info.txid == CTransaction(createTx).GetHash());
    BOOST_CHECK_EQUAL(info.blockHeight, 0);

    AuctionInfo stored;
    BOOST_REQUIRE(db->ReadAuction(auctionId, stored));
    BOOST_CHECK(stored.txid == info.txid);
    BOOST_CHECK(stored.creator == sender);
}

// SUSPECTED BUG (documented, not fixed): the auction ID is the hash of the
// name, the namespace and the start time, and the start time of an auction
// created by a record is the clock of the node at the moment it processes the
// transaction. The same transaction therefore yields a different auction ID
// each second, on each node, and a second auction for a name that is already
// being auctioned.
BOOST_AUTO_TEST_CASE(create_record_auction_id_depends_on_the_clock)
{
    CKeyID sender = MakeKeyID();
    BOOST_REQUIRE(reddidManager->GetNamespaceManager()->AddPricingTier(NS, 3, 1000 * COIN));
    CMutableTransaction createTx = TxFrom(sender, CreateUserIDAuctionOpReturn("alice", NS));

    BOOST_CHECK(Process(createTx));
    // Processed again within the same second, the auction exists already
    BOOST_CHECK(!Process(createTx));

    SetMockTime(T0 + 1);
    BOOST_CHECK(Process(createTx));

    uint256 first = ExpectedAuctionId("alice", NS, T0);
    uint256 second = ExpectedAuctionId("alice", NS, T0 + 1);
    BOOST_CHECK(first != second);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, first).state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, second).state, AUCTION_PENDING);
    BOOST_CHECK(GetAuction(aMgr, first).txid == GetAuction(aMgr, second).txid);

    std::vector<uint256> auctionIds;
    BOOST_REQUIRE(db->ListAuctions(auctionIds));
    BOOST_CHECK_EQUAL(auctionIds.size(), 2U);
}

BOOST_AUTO_TEST_CASE(create_record_malformed)
{
    CKeyID sender = MakeKeyID();
    BOOST_REQUIRE(reddidManager->GetNamespaceManager()->AddPricingTier(NS, 3, 1000 * COIN));

    auto record = [](const std::string& name, const std::string& ns) {
        std::vector<unsigned char> body;
        body.push_back(static_cast<unsigned char>(name.size()));
        body.insert(body.end(), name.begin(), name.end());
        body.push_back(static_cast<unsigned char>(ns.size()));
        body.insert(body.end(), ns.begin(), ns.end());
        return RecordScript(OP_AUCTION_CREATE, body);
    };

    // Too short to hold a name and a namespace
    BOOST_CHECK(!Process(TxFrom(sender, RecordScript(OP_AUCTION_CREATE, std::vector<unsigned char>{1}))));
    BOOST_CHECK(!Process(TxFrom(sender, RecordScript(OP_AUCTION_CREATE, std::vector<unsigned char>{1, 'a'}))));
    // Empty name, name longer than MAX_REDDID_LENGTH, name length running past the end
    BOOST_CHECK(!Process(TxFrom(sender, record("", NS))));
    BOOST_CHECK(!Process(TxFrom(sender, record(std::string(MAX_REDDID_LENGTH + 1, 'a'), NS))));
    BOOST_CHECK(!Process(TxFrom(sender, RecordScript(OP_AUCTION_CREATE, std::vector<unsigned char>{9, 'a', 'b', 'c'}))));
    // Empty namespace, namespace longer than MAX_NAMESPACE_LENGTH, namespace length running past the end
    BOOST_CHECK(!Process(TxFrom(sender, record("alice", ""))));
    BOOST_CHECK(!Process(TxFrom(sender, record("alice", std::string(MAX_NAMESPACE_LENGTH + 1, 'n')))));
    BOOST_CHECK(!Process(TxFrom(sender, RecordScript(OP_AUCTION_CREATE, std::vector<unsigned char>{3, 'a', 'b', 'c', 9, 'n'}))));
    // Well formed, but the namespace does not exist or the name is not valid in it
    BOOST_CHECK(!Process(TxFrom(sender, record("alice", "nosuchns"))));
    BOOST_CHECK(!Process(TxFrom(sender, record("al", NS))));
    BOOST_CHECK(!Process(TxFrom(sender, record("Alice", NS))));

    std::vector<uint256> auctionIds;
    BOOST_REQUIRE(db->ListAuctions(auctionIds));
    BOOST_CHECK(auctionIds.empty());

    // The same shape with valid content is accepted
    BOOST_CHECK(Process(TxFrom(sender, record("alice", NS))));
}

// ===================================================================
// Category 3: OP_AUCTION_BID (bid without a deposit)
// ===================================================================

// SUSPECTED BUG (documented, not fixed): a bid placed with OP_AUCTION_BID
// needs no deposit. The transaction below pays nothing to anyone, yet its
// sender becomes the highest bidder, and the bid records the deposit amount of
// the auction although nothing was locked.
BOOST_AUTO_TEST_CASE(legacy_bid_record_needs_no_deposit)
{
    uint256 auctionId = NewAuction("alice");
    CKeyID bidder = MakeKeyID();

    BOOST_CHECK(Process(TxFrom(bidder, CreateUserIDBidOpReturn(auctionId, 6000 * COIN))));

    std::vector<BidInfo> bids = aMgr->GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK(bids[0].bidId == ExpectedBidId(auctionId, bidder, 6000 * COIN, T0));
    BOOST_CHECK(bids[0].bidder == bidder);
    BOOST_CHECK_EQUAL(bids[0].bidAmount, 6000 * COIN);
    BOOST_CHECK_EQUAL(bids[0].bidTime, T0);
    BOOST_CHECK(bids[0].depositUtxo.IsNull());
    BOOST_CHECK_EQUAL(bids[0].depositAmount, 500 * COIN);
    BOOST_CHECK(aMgr->GetDepositLocksForAuction(auctionId).empty());

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(info.currentBid, 6000 * COIN);
    BOOST_CHECK(info.currentBidder == bidder);
    BOOST_CHECK_EQUAL(info.totalDepositsLocked, 0);
}

// Current behaviour: a bid record without an amount takes the largest
// spendable output of the transaction, whoever it pays.
BOOST_AUTO_TEST_CASE(legacy_bid_record_without_amount_uses_largest_spendable_output)
{
    uint256 auctionId = NewAuction("alice");
    CKeyID bidder = MakeKeyID();

    // 'R' + opcode + auction ID, without the eight amount bytes
    CScript shortRecord = RecordScript(OP_AUCTION_BID, auctionId);

    // No spendable output: the bid amount is zero
    BOOST_CHECK(!Process(TxFrom(bidder, shortRecord)));
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());

    CMutableTransaction bidTx = TxFrom(bidder, shortRecord);
    bidTx.vout.emplace_back(100 * COIN, P2PKH(MakeKeyID()));
    bidTx.vout.emplace_back(7000 * COIN, P2PKH(MakeKeyID()));
    bidTx.vout.emplace_back(900 * COIN, P2PKH(MakeKeyID()));
    BOOST_CHECK(Process(bidTx));

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.currentBid, 7000 * COIN);
    BOOST_CHECK(info.currentBidder == bidder);
}

BOOST_AUTO_TEST_CASE(legacy_bid_record_rejected)
{
    uint256 auctionId = NewAuction("alice");
    CKeyID bidder = MakeKeyID();

    // Shorter than 'R' + opcode + auction ID
    BOOST_CHECK(!Process(TxFrom(bidder, RecordScript(OP_AUCTION_BID, std::vector<unsigned char>(31, 1)))));
    // Zero and negative amounts
    BOOST_CHECK(!Process(TxFrom(bidder, CreateUserIDBidOpReturn(auctionId, 0))));
    BOOST_CHECK(!Process(TxFrom(bidder, CreateUserIDBidOpReturn(auctionId, -1))));
    // Null and unknown auction IDs
    BOOST_CHECK(!Process(TxFrom(bidder, CreateUserIDBidOpReturn(uint256(), 6000 * COIN))));
    BOOST_CHECK(!Process(TxFrom(bidder, CreateUserIDBidOpReturn(uint256S("01"), 6000 * COIN))));
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_PENDING);

    // Below the reserve price, and from the creator of the auction
    BOOST_CHECK(!Process(TxFrom(bidder, CreateUserIDBidOpReturn(auctionId, 5000 * COIN - 1))));
    BOOST_CHECK(!Process(TxFrom(creator, CreateUserIDBidOpReturn(auctionId, 6000 * COIN))));
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());

    BOOST_CHECK(Process(TxFrom(bidder, CreateUserIDBidOpReturn(auctionId, 5000 * COIN))));
    // Less than 5% above the highest bid
    BOOST_CHECK(!Process(TxFrom(MakeKeyID(), CreateUserIDBidOpReturn(auctionId, 5250 * COIN - 1))));
    BOOST_CHECK(Process(TxFrom(MakeKeyID(), CreateUserIDBidOpReturn(auctionId, 5250 * COIN))));
    BOOST_CHECK_EQUAL(aMgr->GetAuctionBids(auctionId).size(), 2U);
}

// ===================================================================
// Category 4: OP_AUCTION_FINALIZE
// ===================================================================

BOOST_AUTO_TEST_CASE(finalize_record_without_deposits_finalizes_the_auction)
{
    uint256 auctionId = NewAuction("alice");
    CKeyID loser = MakeKeyID();
    CKeyID winner = MakeKeyID();
    BOOST_REQUIRE(Process(TxFrom(loser, CreateUserIDBidOpReturn(auctionId, 6000 * COIN))));
    BOOST_REQUIRE(Process(TxFrom(winner, CreateUserIDBidOpReturn(auctionId, 7000 * COIN))));

    CScript finalizeRecord = RecordScript(OP_AUCTION_FINALIZE, auctionId);

    // Before the end, even from the creator
    BOOST_CHECK(!Process(TxFrom(creator, finalizeRecord)));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_ACTIVE);

    SetMockTime(T0 + 7 * DAY - 1);
    BOOST_CHECK(!Process(TxFrom(creator, finalizeRecord)));

    SetMockTime(T0 + 7 * DAY);
    // Only the creator may finalize
    BOOST_CHECK(!Process(TxFrom(winner, finalizeRecord)));
    BOOST_CHECK(!Process(TxFrom(MakeKeyID(), finalizeRecord)));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_ACTIVE);
    // Malformed, null and unknown
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, std::vector<unsigned char>(31, 1)))));
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, uint256()))));
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, uint256S("01")))));

    BOOST_CHECK(Process(TxFrom(creator, finalizeRecord)));

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_FINALIZED);
    BOOST_CHECK_EQUAL(info.settlementDeadlineHeight, 0);
    BOOST_CHECK(info.finalizeTxid.IsNull());
    BOOST_CHECK(!aMgr->IsAuctionActive(auctionId));
    BOOST_CHECK(aMgr->IsAuctionEnded(auctionId));

    UserIDInfo userId;
    BOOST_REQUIRE(db->ReadUserID("alice", NS, userId));
    BOOST_CHECK(userId.owner == winner);
    BOOST_CHECK_EQUAL(userId.registrationTime, T0 + 7 * DAY);
    BOOST_CHECK_EQUAL(userId.expirationTime, T0 + 7 * DAY + 365 * DAY);

    BOOST_CHECK(GetBid(aMgr, ExpectedBidId(auctionId, winner, 7000 * COIN, T0)).isWinner);
    BidInfo losingBid = GetBid(aMgr, ExpectedBidId(auctionId, loser, 6000 * COIN, T0));
    BOOST_CHECK(!losingBid.isWinner);
    BOOST_CHECK(losingBid.refunded);

    // A finalized auction cannot be finalized again
    BOOST_CHECK(!Process(TxFrom(creator, finalizeRecord)));
}

BOOST_AUTO_TEST_CASE(finalize_record_with_deposits_begins_settlement)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid loser = PlaceDepositBid(auctionId, 6000 * COIN);
    DepositBid winner = PlaceDepositBid(auctionId, 8000 * COIN);

    EndAuction(auctionId);
    BOOST_CHECK(!Process(TxFrom(winner.bidder, RecordScript(OP_AUCTION_FINALIZE, auctionId))));

    CMutableTransaction finalizeTx = TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, auctionId));
    BOOST_CHECK(Process(finalizeTx));

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_SETTLEMENT);
    BOOST_CHECK(info.finalizeTxid == CTransaction(finalizeTx).GetHash());
    BOOST_CHECK_EQUAL(info.settlementDeadlineHeight, 4320);
    BOOST_CHECK(!aMgr->IsAuctionActive(auctionId));
    BOOST_CHECK(aMgr->IsAuctionEnded(auctionId));

    BOOST_CHECK(GetBid(aMgr, winner.bidId).isWinner);
    BOOST_CHECK(aMgr->IsDepositLocked(winner.utxo));
    BOOST_CHECK(!GetBid(aMgr, loser.bidId).isWinner);
    BOOST_CHECK_EQUAL(GetLock(aMgr, loser.utxo).state, DEPOSIT_RELEASED_LOST);

    // The name is not registered until the winner settles
    UserIDInfo userId;
    BOOST_CHECK(!db->ReadUserID("alice", NS, userId));
}

// ===================================================================
// Category 5: OP_AUCTION_CANCEL
// ===================================================================

BOOST_AUTO_TEST_CASE(cancel_record)
{
    uint256 pendingId = NewAuction("alice");
    CScript cancelRecord = RecordScript(OP_AUCTION_CANCEL, pendingId);

    // Malformed, null, unknown, and not from the creator
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_AUCTION_CANCEL, std::vector<unsigned char>(31, 1)))));
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_AUCTION_CANCEL, uint256()))));
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_AUCTION_CANCEL, uint256S("01")))));
    BOOST_CHECK(!Process(TxFrom(MakeKeyID(), cancelRecord)));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, pendingId).state, AUCTION_PENDING);

    BOOST_CHECK(Process(TxFrom(creator, cancelRecord)));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, pendingId).state, AUCTION_CANCELED);
    AuctionInfo stored;
    BOOST_REQUIRE(db->ReadAuction(pendingId, stored));
    BOOST_CHECK_EQUAL(stored.state, AUCTION_CANCELED);

    // An auction with a bid cannot be canceled
    uint256 biddenId = NewAuction("bob");
    BOOST_REQUIRE(Process(TxFrom(MakeKeyID(), CreateUserIDBidOpReturn(biddenId, 6000 * COIN))));
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_AUCTION_CANCEL, biddenId))));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, biddenId).state, AUCTION_ACTIVE);

    // An auction that a rejected bid has activated has no bid and can be canceled
    uint256 activatedId = NewAuction("carol");
    BOOST_CHECK(!Process(TxFrom(MakeKeyID(), CreateUserIDBidOpReturn(activatedId, 1 * COIN))));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, activatedId).state, AUCTION_ACTIVE);
    BOOST_CHECK(Process(TxFrom(creator, RecordScript(OP_AUCTION_CANCEL, activatedId))));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, activatedId).state, AUCTION_CANCELED);
    BOOST_CHECK(!aMgr->IsAuctionActive(activatedId));
}

// ===================================================================
// Category 6: OP_BID_DEPOSIT
// ===================================================================

BOOST_AUTO_TEST_CASE(bid_deposit_record_places_bid_and_locks_deposit)
{
    uint256 auctionId = NewAuction("alice");
    CKeyID bidder = MakeKeyID();
    CKeyID funder = MakeKeyID();

    // Current behaviour: the sender of the transaction need not be the bidder
    // named in the record
    DepositBid bid = MakeDepositBid(auctionId, bidder, 10000 * COIN, 1500 * COIN);
    bid.tx.vin[0].prevout = FundKey(chainstate, funder);
    bid.utxo = COutPoint(bid.tx.GetHash(), 1);
    BOOST_CHECK(Process(bid.tx, 12));

    BidInfo info = GetBid(aMgr, bid.bidId);
    BOOST_CHECK(info.auctionId == auctionId);
    BOOST_CHECK(info.bidder == bidder);
    BOOST_CHECK_EQUAL(info.bidAmount, 10000 * COIN);
    BOOST_CHECK_EQUAL(info.bidTime, T0);
    BOOST_CHECK(info.depositUtxo == bid.utxo);
    BOOST_CHECK_EQUAL(info.depositAmount, 1500 * COIN);
    BOOST_CHECK_EQUAL(info.depositLockHeight, 12);
    BOOST_CHECK_EQUAL(info.depositState, DEPOSIT_LOCKED_ACTIVE);
    BOOST_CHECK(!info.isWinner);
    BOOST_CHECK(!info.refunded);

    BOOST_CHECK(aMgr->IsDepositLocked(bid.utxo));
    DepositLock lock = GetLock(aMgr, bid.utxo);
    BOOST_CHECK(lock.utxo == bid.utxo);
    BOOST_CHECK(lock.auctionId == auctionId);
    BOOST_CHECK(lock.bidId == bid.bidId);
    BOOST_CHECK(lock.bidder == bidder);
    BOOST_CHECK_EQUAL(lock.amount, 1500 * COIN);
    BOOST_CHECK_EQUAL(lock.lockHeight, 12);
    BOOST_CHECK_EQUAL(lock.releaseHeight, 0);
    BOOST_CHECK_EQUAL(lock.state, DEPOSIT_LOCKED_ACTIVE);

    AuctionInfo auction = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(auction.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(auction.currentBid, 10000 * COIN);
    BOOST_CHECK(auction.currentBidder == bidder);
    BOOST_CHECK_EQUAL(auction.totalDepositsLocked, 1500 * COIN);

    // All of it is in the database too
    DepositLock storedLock;
    BOOST_REQUIRE(db->ReadDepositLock(bid.utxo, storedLock));
    BOOST_CHECK(storedLock.bidId == bid.bidId);
    BidInfo storedBid;
    BOOST_REQUIRE(db->ReadBid(bid.bidId, storedBid));
    BOOST_CHECK(storedBid.depositUtxo == bid.utxo);
    AuctionInfo storedAuction;
    BOOST_REQUIRE(db->ReadAuction(auctionId, storedAuction));
    BOOST_CHECK_EQUAL(storedAuction.totalDepositsLocked, 1500 * COIN);

    // A second bidder adds to the total
    DepositBid second = PlaceDepositBid(auctionId, 12000 * COIN, 13);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).totalDepositsLocked, 1500 * COIN + 1800 * COIN);
    BOOST_CHECK_EQUAL(aMgr->GetDepositLocksForAuction(auctionId).size(), 2U);
    BOOST_CHECK_EQUAL(GetLock(aMgr, second.utxo).lockHeight, 13);
}

BOOST_AUTO_TEST_CASE(bid_deposit_record_rejected)
{
    uint256 auctionId = NewAuction("alice");
    CKeyID bidder = MakeKeyID();
    const CAmount bidAmount = 10000 * COIN;
    const CAmount minDeposit = 1500 * COIN;

    // One byte short of 'R' + opcode + auction ID + amount + vout + key hash
    {
        CMutableTransaction mtx = TxFrom(bidder, RecordScript(OP_BID_DEPOSIT, std::vector<unsigned char>(60, 1)));
        mtx.vout.emplace_back(minDeposit, P2PKH(bidder));
        BOOST_CHECK(!Process(mtx));
    }
    // Null and unknown auction IDs
    BOOST_CHECK(!Process(MakeDepositBid(uint256(), bidder, bidAmount, minDeposit).tx));
    BOOST_CHECK(!Process(MakeDepositBid(uint256S("01"), bidder, bidAmount, minDeposit).tx));
    // Zero and negative bid amounts
    BOOST_CHECK(!Process(MakeDepositBid(auctionId, bidder, 0, minDeposit).tx));
    BOOST_CHECK(!Process(MakeDepositBid(auctionId, bidder, -1, minDeposit).tx));
    // The record names an output that the transaction does not have
    {
        CMutableTransaction mtx = TxFrom(bidder, CreateBidDepositOpReturn(auctionId, bidAmount, 2, bidder));
        mtx.vout.emplace_back(minDeposit, P2PKH(bidder));
        BOOST_CHECK(!Process(mtx));
    }
    // One satoshi less than 15% of the bid
    BOOST_CHECK(!Process(MakeDepositBid(auctionId, bidder, bidAmount, minDeposit - 1).tx));
    // The deposit output pays another key
    {
        DepositBid bid = MakeDepositBid(auctionId, bidder, bidAmount, minDeposit);
        bid.tx.vout[1].scriptPubKey = P2PKH(MakeKeyID());
        BOOST_CHECK(!Process(bid.tx));
    }
    // The deposit output pays a script hash
    {
        DepositBid bid = MakeDepositBid(auctionId, bidder, bidAmount, minDeposit);
        bid.tx.vout[1].scriptPubKey = GetScriptForDestination(ScriptHash(P2PKH(bidder)));
        BOOST_CHECK(!Process(bid.tx));
    }
    // The deposit output has no destination
    {
        DepositBid bid = MakeDepositBid(auctionId, bidder, bidAmount, minDeposit);
        bid.tx.vout[1].scriptPubKey = CScript() << OP_TRUE;
        BOOST_CHECK(!Process(bid.tx));
    }

    // None of these got as far as placing the bid
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_PENDING);
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
    BOOST_CHECK(aMgr->GetDepositLocksForAuction(auctionId).empty());

    // A bid that the auction rejects creates no lock either
    BOOST_CHECK(!Process(MakeDepositBid(auctionId, creator, bidAmount, minDeposit).tx));
    BOOST_CHECK(!Process(MakeDepositBid(auctionId, bidder, 4000 * COIN, 600 * COIN).tx));
    BOOST_CHECK(aMgr->GetDepositLocksForAuction(auctionId).empty());

    // Exactly 15% is enough
    DepositBid accepted = MakeDepositBid(auctionId, bidder, bidAmount, minDeposit);
    BOOST_CHECK(Process(accepted.tx));
    BOOST_CHECK(aMgr->IsDepositLocked(accepted.utxo));

    // An auction that is neither pending nor active takes no deposit
    uint256 canceledId = NewAuction("bob");
    BOOST_REQUIRE(aMgr->CancelAuction(canceledId, creator));
    BOOST_CHECK(!Process(MakeDepositBid(canceledId, bidder, bidAmount, minDeposit).tx));
}

BOOST_AUTO_TEST_CASE(bid_deposit_percentage_comes_from_the_userid_config)
{
    uint256 auctionId = NewAuction("alice");

    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    BOOST_CHECK_EQUAL(config.depositPercentage, 15);
    config.depositPercentage = 50;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    CKeyID bidder = MakeKeyID();
    BOOST_CHECK(!Process(MakeDepositBid(auctionId, bidder, 10000 * COIN, 1500 * COIN).tx));
    BOOST_CHECK(!Process(MakeDepositBid(auctionId, bidder, 10000 * COIN, 5000 * COIN - 1).tx));
    BOOST_CHECK(Process(MakeDepositBid(auctionId, bidder, 10000 * COIN, 5000 * COIN).tx));
}

// SUSPECTED BUG (documented, not fixed): the minimum deposit is computed as
// (bidAmount * depositPercentage) / 100 on a bid amount that is only checked
// to be positive. The product overflows a signed 64-bit integer for bids above
// INT64_MAX / depositPercentage, which at 15% is about 6.1 billion RDD and
// well inside the money range. This test stops at the largest bid for which
// the product still fits, so that it does not execute the overflow.
BOOST_AUTO_TEST_CASE(bid_deposit_amount_has_no_upper_bound)
{
    uint256 auctionId = NewAuction("alice");
    const CAmount largest = std::numeric_limits<CAmount>::max() / 15;
    BOOST_CHECK(MoneyRange(largest + 1));

    DepositBid bid = MakeDepositBid(auctionId, MakeKeyID(), largest, largest * 15 / 100);
    BOOST_CHECK(Process(bid.tx));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).currentBid, largest);
    BOOST_CHECK(aMgr->IsDepositLocked(bid.utxo));
}

// ===================================================================
// Category 7: OP_AUCTION_SETTLE
// ===================================================================

BOOST_AUTO_TEST_CASE(settle_record_completes_the_settlement)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid loser = PlaceDepositBid(auctionId, 6000 * COIN);
    DepositBid winner = PlaceDepositBid(auctionId, 8000 * COIN);
    CScript settleRecord = CreateAuctionSettleOpReturn(auctionId, winner.bidId);
    CKeyID anyone = MakeKeyID();

    auto settleTx = [&](const CScript& record, const COutPoint& deposit) {
        CMutableTransaction mtx = TxFrom(anyone, record);
        mtx.vin.emplace_back(deposit);
        return mtx;
    };

    // The auction is still running
    BOOST_CHECK(!Process(settleTx(settleRecord, winner.utxo)));

    EndAuction(auctionId);
    BOOST_REQUIRE(Process(TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, auctionId))));
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_SETTLEMENT);

    // One byte short of 'R' + opcode + auction ID + bid ID
    BOOST_CHECK(!Process(settleTx(RecordScript(OP_AUCTION_SETTLE, std::vector<unsigned char>(63, 1)), winner.utxo)));
    // Null and unknown IDs
    BOOST_CHECK(!Process(settleTx(CreateAuctionSettleOpReturn(uint256(), winner.bidId), winner.utxo)));
    BOOST_CHECK(!Process(settleTx(CreateAuctionSettleOpReturn(auctionId, uint256()), winner.utxo)));
    BOOST_CHECK(!Process(settleTx(CreateAuctionSettleOpReturn(uint256S("01"), winner.bidId), winner.utxo)));
    // A bid that did not win
    BOOST_CHECK(!Process(settleTx(CreateAuctionSettleOpReturn(auctionId, loser.bidId), loser.utxo)));
    // The winning bid, but the transaction does not spend its deposit
    BOOST_CHECK(!Process(TxFrom(anyone, settleRecord)));
    BOOST_CHECK(!Process(settleTx(settleRecord, loser.utxo)));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_SETTLEMENT);
    BOOST_CHECK(aMgr->IsDepositLocked(winner.utxo));

    // The sender is not the winner; spending the deposit is what counts
    CMutableTransaction goodTx = settleTx(settleRecord, winner.utxo);
    BOOST_CHECK(Process(goodTx));

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_COMPLETED);
    BOOST_CHECK(info.settleTxid == CTransaction(goodTx).GetHash());
    BOOST_CHECK(aMgr->IsAuctionEnded(auctionId));

    UserIDInfo userId;
    BOOST_REQUIRE(db->ReadUserID("alice", NS, userId));
    BOOST_CHECK(userId.owner == winner.bidder);
    BOOST_CHECK_EQUAL(userId.expirationTime, GetTime() + 365 * DAY);

    BOOST_CHECK_EQUAL(GetLock(aMgr, winner.utxo).state, DEPOSIT_CONSUMED_WON);
    BOOST_CHECK(!aMgr->IsDepositLocked(winner.utxo));
    BOOST_CHECK_EQUAL(GetBid(aMgr, winner.bidId).depositState, DEPOSIT_CONSUMED_WON);

    // Settled once
    BOOST_CHECK(!Process(settleTx(settleRecord, winner.utxo)));
}

// ===================================================================
// Category 8: OP_AUCTION_DEFAULT and OP_DEPOSIT_FORFEIT
// ===================================================================

BOOST_AUTO_TEST_CASE(default_record_rejected_before_the_deadline)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid winner = PlaceDepositBid(auctionId, 8000 * COIN);
    CScript defaultRecord = RecordScript(OP_AUCTION_DEFAULT, auctionId, winner.bidId);

    // Not in settlement yet
    BOOST_CHECK(!Process(TxFrom(MakeKeyID(), defaultRecord)));

    EndAuction(auctionId);
    BOOST_REQUIRE(Process(TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, auctionId))));
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).settlementDeadlineHeight, 4320);

    // The chain is at height 0, long before the deadline
    BOOST_CHECK(!Process(TxFrom(MakeKeyID(), defaultRecord)));
    BOOST_CHECK(!Process(TxFrom(creator, defaultRecord)));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_SETTLEMENT);
    BOOST_CHECK(aMgr->IsDepositLocked(winner.utxo));
}

// Current behaviour: at tip height 0 a settlement window of 0 blocks gives a
// deadline of 0, which the checks treat as "no deadline", so the default can
// be declared at once. At a height h > 0 the deadline is h and the default
// needs height h + 1. Anyone can declare it: the sender of an
// OP_AUCTION_DEFAULT record is not checked.
BOOST_AUTO_TEST_CASE(default_record_forfeits_deposit_and_creates_re_auction)
{
    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    config.settlementWindowBlocks = 0;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    uint256 auctionId = NewAuction("alice");
    DepositBid winner = PlaceDepositBid(auctionId, 8000 * COIN);
    EndAuction(auctionId);
    BOOST_REQUIRE(Process(TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, auctionId))));
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).settlementDeadlineHeight, 0);

    // One byte short, null IDs, unknown auction, a bid that is not the winner
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_AUCTION_DEFAULT, std::vector<unsigned char>(63, 1)))));
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_AUCTION_DEFAULT, uint256(), winner.bidId))));
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_AUCTION_DEFAULT, auctionId, uint256()))));
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_AUCTION_DEFAULT, uint256S("01"), winner.bidId))));
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_AUCTION_DEFAULT, auctionId, uint256S("02")))));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_SETTLEMENT);

    BOOST_CHECK(Process(TxFrom(MakeKeyID(), RecordScript(OP_AUCTION_DEFAULT, auctionId, winner.bidId))));

    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_DEFAULTED);
    BOOST_CHECK_EQUAL(GetLock(aMgr, winner.utxo).state, DEPOSIT_FORFEITED);
    BOOST_CHECK_EQUAL(GetBid(aMgr, winner.bidId).depositState, DEPOSIT_FORFEITED);

    // The name goes back to auction, in the name of the original creator
    uint256 reAuctionId = ExpectedAuctionId("alice", NS, GetTime());
    AuctionInfo reAuction;
    BOOST_REQUIRE(aMgr->GetAuctionInfo(reAuctionId, reAuction));
    BOOST_CHECK_EQUAL(reAuction.name, "alice");
    BOOST_CHECK_EQUAL(reAuction.namespaceId, NS);
    BOOST_CHECK(reAuction.creator == creator);
    BOOST_CHECK_EQUAL(reAuction.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(reAuction.reservePrice, 5000 * COIN);
    BOOST_CHECK_EQUAL(reAuction.startTime, GetTime());
    BOOST_CHECK_EQUAL(reAuction.endTime, GetTime() + 7 * DAY);
    BOOST_CHECK_EQUAL(reAuction.reAuctionCount, 1);
    BOOST_CHECK(reAuction.previousAuctionId == auctionId);
    BOOST_CHECK_EQUAL(reAuction.currentBid, 0);

    // Defaulted once
    BOOST_CHECK(!Process(TxFrom(MakeKeyID(), RecordScript(OP_AUCTION_DEFAULT, auctionId, winner.bidId))));
}

// SUSPECTED BUG (documented, not fixed): an OP_DEPOSIT_FORFEIT record removes
// the lock on a forfeited deposit whoever sends it and whatever the
// transaction spends or pays. The transaction below does not touch the
// deposit and pays nothing to a community fund; after it the deposit is no
// longer locked, so its owner can spend it like any other output.
BOOST_AUTO_TEST_CASE(forfeit_record_removes_the_lock)
{
    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    config.settlementWindowBlocks = 0;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    uint256 auctionId = NewAuction("alice");
    DepositBid loser = PlaceDepositBid(auctionId, 6000 * COIN);
    DepositBid winner = PlaceDepositBid(auctionId, 8000 * COIN);
    CScript forfeitRecord = RecordScript(OP_DEPOSIT_FORFEIT, auctionId, winner.bidId);

    // Nothing is forfeited while the auction runs or is in settlement
    BOOST_CHECK(!Process(TxFrom(MakeKeyID(), forfeitRecord)));
    EndAuction(auctionId);
    BOOST_REQUIRE(Process(TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, auctionId))));
    BOOST_CHECK(!Process(TxFrom(MakeKeyID(), forfeitRecord)));
    BOOST_REQUIRE(Process(TxFrom(MakeKeyID(), RecordScript(OP_AUCTION_DEFAULT, auctionId, winner.bidId))));
    BOOST_REQUIRE_EQUAL(GetLock(aMgr, winner.utxo).state, DEPOSIT_FORFEITED);

    // One byte short, null IDs, unknown auction, a bid that is not forfeited
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_DEPOSIT_FORFEIT, std::vector<unsigned char>(63, 1)))));
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_DEPOSIT_FORFEIT, uint256(), winner.bidId))));
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_DEPOSIT_FORFEIT, auctionId, uint256()))));
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_DEPOSIT_FORFEIT, uint256S("01"), winner.bidId))));
    BOOST_CHECK(!Process(TxFrom(creator, RecordScript(OP_DEPOSIT_FORFEIT, auctionId, loser.bidId))));
    DepositLock lock;
    BOOST_CHECK(aMgr->GetDepositLock(winner.utxo, lock));

    BOOST_CHECK(Process(TxFrom(MakeKeyID(), forfeitRecord)));

    BOOST_CHECK(!aMgr->GetDepositLock(winner.utxo, lock));
    BOOST_CHECK(!db->ExistsDepositLock(winner.utxo));
    BOOST_CHECK_EQUAL(GetBid(aMgr, winner.bidId).depositState, DEPOSIT_FORFEITED);

    CMutableTransaction spend;
    spend.vin.emplace_back(winner.utxo);
    spend.vout.emplace_back(winner.deposit, P2PKH(winner.bidder));
    BOOST_CHECK(aMgr->CheckDepositLocks(CTransaction(spend)));
}

// ===================================================================
// Category 9: Transactions with more than one record
// ===================================================================

// Current behaviour: the outputs are tried in order. A record that the
// auction manager rejects through one of its operations (CreateAuction,
// PlaceBid, ...) is skipped and the next output is tried; a record that is
// rejected while it is being decoded ends processing with false. An accepted
// record ends processing with true, so at most one record takes effect.
BOOST_AUTO_TEST_CASE(transaction_with_several_records)
{
    uint256 firstId = NewAuction("alice");
    uint256 secondId = NewAuction("bob");
    uint256 thirdId = NewAuction("carol");

    // The create record fails softly (reserve below the minimum); the cancel record is applied
    CMutableTransaction softFailure = TxFrom(creator, CreateUserIDAuctionOpReturn("dave", NS));
    softFailure.vout.emplace_back(0, RecordScript(OP_AUCTION_CANCEL, firstId));
    BOOST_CHECK(Process(softFailure));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, firstId).state, AUCTION_CANCELED);

    // The bid record with a null auction ID ends processing; the cancel record is not reached
    CMutableTransaction hardFailure = TxFrom(creator, CreateUserIDBidOpReturn(uint256(), 6000 * COIN));
    hardFailure.vout.emplace_back(0, RecordScript(OP_AUCTION_CANCEL, secondId));
    BOOST_CHECK(!Process(hardFailure));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, secondId).state, AUCTION_PENDING);

    // Two valid records: only the first is applied
    CMutableTransaction twoCancels = TxFrom(creator, RecordScript(OP_AUCTION_CANCEL, secondId));
    twoCancels.vout.emplace_back(0, RecordScript(OP_AUCTION_CANCEL, thirdId));
    BOOST_CHECK(Process(twoCancels));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, secondId).state, AUCTION_CANCELED);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, thirdId).state, AUCTION_PENDING);

    // Records of other subsystems and plain outputs are passed over
    CMutableTransaction mixed = TxFrom(creator, RecordScript(OP_REDDID_PROFILE_UPDATE, uint256S("01")));
    mixed.vout.emplace_back(1 * COIN, P2PKH(MakeKeyID()));
    mixed.vout.emplace_back(0, RecordScript(OP_AUCTION_CANCEL, thirdId));
    BOOST_CHECK(Process(mixed));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, thirdId).state, AUCTION_CANCELED);
}

// ===================================================================
// Category 10: UndoTransaction
// ===================================================================

BOOST_AUTO_TEST_CASE(undo_of_a_transaction_without_auction_effect_returns_true)
{
    uint256 auctionId = NewAuction("alice");

    CMutableTransaction payment;
    payment.vin.emplace_back(COutPoint(InsecureRand256(), 0));
    payment.vout.emplace_back(5 * COIN, P2PKH(MakeKeyID()));
    BOOST_CHECK(Undo(payment));

    // Records that name an auction which does not exist
    for (unsigned char opCode : {OP_AUCTION_FINALIZE, OP_AUCTION_CANCEL}) {
        BOOST_CHECK(Undo(RecordTx(COutPoint(InsecureRand256(), 0), RecordScript(opCode, uint256S("01")))));
    }
    BOOST_CHECK(Undo(RecordTx(COutPoint(InsecureRand256(), 0), CreateUserIDBidOpReturn(uint256S("01"), 6000 * COIN))));
    BOOST_CHECK(Undo(RecordTx(COutPoint(InsecureRand256(), 0), CreateAuctionSettleOpReturn(uint256S("01"), uint256S("02")))));
    BOOST_CHECK(Undo(RecordTx(COutPoint(InsecureRand256(), 0), RecordScript(OP_AUCTION_DEFAULT, uint256S("01"), uint256S("02")))));
    BOOST_CHECK(Undo(RecordTx(COutPoint(InsecureRand256(), 0), RecordScript(OP_DEPOSIT_FORFEIT, uint256S("01"), uint256S("02")))));
    BOOST_CHECK(Undo(RecordTx(COutPoint(InsecureRand256(), 0), CreateUserIDAuctionOpReturn("nobody", NS))));

    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_PENDING);
    std::vector<uint256> auctionIds;
    BOOST_REQUIRE(db->ListAuctions(auctionIds));
    BOOST_CHECK_EQUAL(auctionIds.size(), 1U);
}

// SUSPECTED BUG (documented, not fixed): undoing an OP_AUCTION_CREATE record
// erases an auction for the name and namespace in the record, not the auction
// that the transaction created. The auction below was not created by a
// transaction at all. With two auctions for the same name it is the one with
// the lower auction ID that goes.
BOOST_AUTO_TEST_CASE(undo_create_erases_an_auction_for_the_name)
{
    uint256 auctionId = NewAuction("alice");
    uint256 bidId;
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, MakeKeyID(), 6000 * COIN, bidId));
    BOOST_REQUIRE(GetAuction(aMgr, auctionId).txid.IsNull());

    CMutableTransaction unrelated = RecordTx(COutPoint(InsecureRand256(), 0), CreateUserIDAuctionOpReturn("alice", NS));
    BOOST_CHECK(Undo(unrelated));

    AuctionInfo info;
    BOOST_CHECK(!aMgr->GetAuctionInfo(auctionId, info));
    BOOST_CHECK(!db->ReadAuction(auctionId, info));
    BidInfo bid;
    BOOST_CHECK(!aMgr->GetBidInfo(bidId, bid));
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
    BOOST_CHECK(aMgr->GetActiveAuctions().empty());

    // Two auctions for one name
    uint256 first = NewAuction("bob");
    SetMockTime(T0 + 1);
    uint256 second = NewAuction("bob");
    BOOST_CHECK(Undo(RecordTx(COutPoint(InsecureRand256(), 0), CreateUserIDAuctionOpReturn("bob", NS))));
    BOOST_CHECK(!aMgr->GetAuctionInfo(std::min(first, second), info));
    BOOST_CHECK(aMgr->GetAuctionInfo(std::max(first, second), info));
}

BOOST_AUTO_TEST_CASE(undo_legacy_bid_restores_previous_highest_bid)
{
    uint256 auctionId = NewAuction("alice");
    CKeyID bidderA = MakeKeyID();
    CKeyID bidderB = MakeKeyID();
    CMutableTransaction txA = TxFrom(bidderA, CreateUserIDBidOpReturn(auctionId, 5000 * COIN));
    CMutableTransaction txB = TxFrom(bidderB, CreateUserIDBidOpReturn(auctionId, 6000 * COIN));
    BOOST_REQUIRE(Process(txA));
    BOOST_REQUIRE(Process(txB));
    uint256 bidA = ExpectedBidId(auctionId, bidderA, 5000 * COIN, T0);
    uint256 bidB = ExpectedBidId(auctionId, bidderB, 6000 * COIN, T0);

    BOOST_CHECK(Undo(txB));
    BidInfo bid;
    BOOST_CHECK(!aMgr->GetBidInfo(bidB, bid));
    BOOST_CHECK(!db->ReadBid(bidB, bid));
    BOOST_CHECK_EQUAL(aMgr->GetAuctionBids(auctionId).size(), 1U);
    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(info.currentBid, 5000 * COIN);
    BOOST_CHECK(info.currentBidder == bidderA);

    BOOST_CHECK(Undo(txA));
    BOOST_CHECK(!aMgr->GetBidInfo(bidA, bid));
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
    info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(info.currentBid, 0);
    BOOST_CHECK(info.currentBidder.IsNull());
    BOOST_CHECK(!aMgr->IsAuctionActive(auctionId));
    BOOST_CHECK(aMgr->GetActiveAuctions().empty());

    AuctionInfo stored;
    BOOST_REQUIRE(db->ReadAuction(auctionId, stored));
    BOOST_CHECK_EQUAL(stored.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(stored.currentBid, 0);
}

// SUSPECTED BUG (documented, not fixed): raising one's own bid marks the
// earlier bid as refunded, and undoing the raise does not clear that mark.
// The auction is left pending and without a highest bid while it still holds
// the first bid.
BOOST_AUTO_TEST_CASE(undo_of_a_raised_bid_does_not_restore_the_bid_it_replaced)
{
    uint256 auctionId = NewAuction("alice");
    CKeyID bidder = MakeKeyID();
    CMutableTransaction first = TxFrom(bidder, CreateUserIDBidOpReturn(auctionId, 5000 * COIN));
    CMutableTransaction raise = TxFrom(bidder, CreateUserIDBidOpReturn(auctionId, 6000 * COIN));
    BOOST_REQUIRE(Process(first));
    BOOST_REQUIRE(Process(raise));

    BOOST_CHECK(Undo(raise));

    std::vector<BidInfo> bids = aMgr->GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK_EQUAL(bids[0].bidAmount, 5000 * COIN);
    BOOST_CHECK(bids[0].refunded);
    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(info.currentBid, 0);
    BOOST_CHECK(info.currentBidder.IsNull());
}

// SUSPECTED BUG (documented, not fixed): a bid record without the amount
// bytes is accepted (the amount is taken from the outputs), but undoing it
// requires the amount bytes and does nothing without them.
BOOST_AUTO_TEST_CASE(undo_of_a_bid_record_without_amount_does_nothing)
{
    uint256 auctionId = NewAuction("alice");
    CKeyID bidder = MakeKeyID();
    CMutableTransaction bidTx = TxFrom(bidder, RecordScript(OP_AUCTION_BID, auctionId));
    bidTx.vout.emplace_back(7000 * COIN, P2PKH(MakeKeyID()));
    BOOST_REQUIRE(Process(bidTx));

    BOOST_CHECK(Undo(bidTx));

    BOOST_CHECK_EQUAL(aMgr->GetAuctionBids(auctionId).size(), 1U);
    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(info.currentBid, 7000 * COIN);
    BOOST_CHECK(info.currentBidder == bidder);
}

// SUSPECTED BUG (documented, not fixed): undoing a finalization returns the
// auction to the active state but leaves the user ID that the finalization
// registered in the database.
BOOST_AUTO_TEST_CASE(undo_finalize_reactivates_the_auction_but_keeps_the_user_id)
{
    uint256 auctionId = NewAuction("alice");
    CKeyID loser = MakeKeyID();
    CKeyID winner = MakeKeyID();
    uint256 losingBid, winningBid;
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, loser, 5000 * COIN, losingBid));
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, winner, 6000 * COIN, winningBid));
    EndAuction(auctionId);
    CMutableTransaction finalizeTx = TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, auctionId));
    BOOST_REQUIRE(Process(finalizeTx));
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_FINALIZED);
    BOOST_REQUIRE(GetBid(aMgr, winningBid).isWinner);
    BOOST_REQUIRE(GetBid(aMgr, losingBid).refunded);

    BOOST_CHECK(Undo(finalizeTx));

    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_ACTIVE);
    BOOST_CHECK(aMgr->IsAuctionActive(auctionId));
    BOOST_CHECK_EQUAL(aMgr->GetActiveAuctions().size(), 1U);
    BOOST_CHECK(!GetBid(aMgr, winningBid).isWinner);
    BOOST_CHECK(!GetBid(aMgr, losingBid).refunded);
    BidInfo stored;
    BOOST_REQUIRE(db->ReadBid(winningBid, stored));
    BOOST_CHECK(!stored.isWinner);
    BOOST_REQUIRE(db->ReadBid(losingBid, stored));
    BOOST_CHECK(!stored.refunded);

    UserIDInfo userId;
    BOOST_CHECK(db->ReadUserID("alice", NS, userId));
    BOOST_CHECK(userId.owner == winner);
}

BOOST_AUTO_TEST_CASE(undo_finalize_of_a_settlement_relocks_released_deposits)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid loser = PlaceDepositBid(auctionId, 6000 * COIN);
    DepositBid winner = PlaceDepositBid(auctionId, 8000 * COIN);
    EndAuction(auctionId);
    CMutableTransaction finalizeTx = TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, auctionId));
    BOOST_REQUIRE(Process(finalizeTx));
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_SETTLEMENT);
    BOOST_REQUIRE(!aMgr->IsDepositLocked(loser.utxo));

    BOOST_CHECK(Undo(finalizeTx));

    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(info.settlementDeadlineHeight, 0);
    BOOST_CHECK(info.finalizeTxid.IsNull());
    BOOST_CHECK(aMgr->IsAuctionActive(auctionId));

    BOOST_CHECK(!GetBid(aMgr, winner.bidId).isWinner);
    BOOST_CHECK(aMgr->IsDepositLocked(winner.utxo));

    BidInfo losingBid = GetBid(aMgr, loser.bidId);
    BOOST_CHECK(!losingBid.refunded);
    BOOST_CHECK_EQUAL(losingBid.depositState, DEPOSIT_LOCKED_ACTIVE);
    BOOST_CHECK(aMgr->IsDepositLocked(loser.utxo));
    BOOST_CHECK_EQUAL(GetLock(aMgr, loser.utxo).releaseHeight, 0);
    DepositLock storedLock;
    BOOST_REQUIRE(db->ReadDepositLock(loser.utxo, storedLock));
    BOOST_CHECK_EQUAL(storedLock.state, DEPOSIT_LOCKED_ACTIVE);

    // The settlement can begin again
    BOOST_CHECK(Process(finalizeTx));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_SETTLEMENT);
}

BOOST_AUTO_TEST_CASE(undo_cancel_restores_pending)
{
    uint256 pendingId = NewAuction("alice");
    CMutableTransaction cancelPending = TxFrom(creator, RecordScript(OP_AUCTION_CANCEL, pendingId));
    BOOST_REQUIRE(Process(cancelPending));
    BOOST_CHECK(Undo(cancelPending));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, pendingId).state, AUCTION_PENDING);
    BOOST_CHECK(!aMgr->IsAuctionActive(pendingId));
    AuctionInfo stored;
    BOOST_REQUIRE(db->ReadAuction(pendingId, stored));
    BOOST_CHECK_EQUAL(stored.state, AUCTION_PENDING);
}

// SUSPECTED BUG (documented, not fixed): UndoTransaction does not know
// whether the transaction it undoes had any effect. The cancel record below
// was rejected (its sender is not the creator, and the auction is finalized),
// yet undoing it puts the finalized auction back into the active state.
BOOST_AUTO_TEST_CASE(undo_of_a_rejected_cancel_reactivates_a_finalized_auction)
{
    uint256 auctionId = NewAuction("alice");
    uint256 bidId;
    BOOST_REQUIRE(aMgr->PlaceBid(auctionId, MakeKeyID(), 6000 * COIN, bidId));
    EndAuction(auctionId);
    BOOST_REQUIRE(aMgr->FinalizeAuction(auctionId));
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_FINALIZED);

    CMutableTransaction rejectedCancel = TxFrom(MakeKeyID(), RecordScript(OP_AUCTION_CANCEL, auctionId));
    BOOST_REQUIRE(!Process(rejectedCancel));
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_FINALIZED);

    BOOST_CHECK(Undo(rejectedCancel));

    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_ACTIVE);
    BOOST_CHECK(aMgr->IsAuctionActive(auctionId));
    BOOST_CHECK_EQUAL(aMgr->GetActiveAuctions().size(), 1U);
}

BOOST_AUTO_TEST_CASE(undo_bid_deposit_removes_bid_and_lock)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid first = PlaceDepositBid(auctionId, 6000 * COIN);
    DepositBid second = PlaceDepositBid(auctionId, 8000 * COIN);
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).totalDepositsLocked, 900 * COIN + 1200 * COIN);

    BOOST_CHECK(Undo(second.tx));

    DepositLock lock;
    BOOST_CHECK(!aMgr->GetDepositLock(second.utxo, lock));
    BOOST_CHECK(!db->ExistsDepositLock(second.utxo));
    BidInfo bid;
    BOOST_CHECK(!aMgr->GetBidInfo(second.bidId, bid));
    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(info.currentBid, 6000 * COIN);
    BOOST_CHECK(info.currentBidder == first.bidder);
    BOOST_CHECK_EQUAL(info.totalDepositsLocked, 900 * COIN);
    BOOST_CHECK(aMgr->IsDepositLocked(first.utxo));

    BOOST_CHECK(Undo(first.tx));

    BOOST_CHECK(!aMgr->GetDepositLock(first.utxo, lock));
    BOOST_CHECK(aMgr->GetAuctionBids(auctionId).empty());
    BOOST_CHECK(aMgr->GetDepositLocksForAuction(auctionId).empty());
    info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(info.currentBid, 0);
    BOOST_CHECK(info.currentBidder.IsNull());
    BOOST_CHECK_EQUAL(info.totalDepositsLocked, 0);
    BOOST_CHECK(!aMgr->IsAuctionActive(auctionId));

    // The same bid can be placed again
    BOOST_CHECK(Process(first.tx));
    BOOST_CHECK(aMgr->IsDepositLocked(first.utxo));
}

BOOST_AUTO_TEST_CASE(undo_settle_unregisters_the_user_id_and_relocks_the_deposit)
{
    uint256 auctionId = NewAuction("alice");
    DepositBid winner = PlaceDepositBid(auctionId, 8000 * COIN);
    EndAuction(auctionId);
    BOOST_REQUIRE(Process(TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, auctionId))));
    CMutableTransaction settleTx = TxFrom(winner.bidder, CreateAuctionSettleOpReturn(auctionId, winner.bidId));
    settleTx.vin.emplace_back(winner.utxo);
    BOOST_REQUIRE(Process(settleTx));
    UserIDInfo userId;
    BOOST_REQUIRE(db->ReadUserID("alice", NS, userId));

    BOOST_CHECK(Undo(settleTx));

    BOOST_CHECK(!db->ReadUserID("alice", NS, userId));
    AuctionInfo info = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(info.state, AUCTION_SETTLEMENT);
    BOOST_CHECK(info.settleTxid.IsNull());
    BOOST_CHECK_EQUAL(info.settlementDeadlineHeight, 4320);
    BOOST_CHECK(aMgr->IsDepositLocked(winner.utxo));
    BOOST_CHECK_EQUAL(GetLock(aMgr, winner.utxo).releaseHeight, 0);
    BidInfo bid = GetBid(aMgr, winner.bidId);
    BOOST_CHECK_EQUAL(bid.depositState, DEPOSIT_LOCKED_ACTIVE);
    BOOST_CHECK(bid.isWinner);

    // The settlement can be completed again
    BOOST_CHECK(Process(settleTx));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_COMPLETED);
    BOOST_CHECK(db->ReadUserID("alice", NS, userId));
}

BOOST_AUTO_TEST_CASE(undo_default_erases_the_re_auction_and_relocks_the_deposit)
{
    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    config.settlementWindowBlocks = 0;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    uint256 auctionId = NewAuction("alice");
    DepositBid winner = PlaceDepositBid(auctionId, 8000 * COIN);
    EndAuction(auctionId);
    BOOST_REQUIRE(Process(TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, auctionId))));
    CMutableTransaction defaultTx = TxFrom(MakeKeyID(), RecordScript(OP_AUCTION_DEFAULT, auctionId, winner.bidId));
    BOOST_REQUIRE(Process(defaultTx));
    uint256 reAuctionId = ExpectedAuctionId("alice", NS, GetTime());
    AuctionInfo info;
    BOOST_REQUIRE(aMgr->GetAuctionInfo(reAuctionId, info));

    BOOST_CHECK(Undo(defaultTx));

    BOOST_CHECK(!aMgr->GetAuctionInfo(reAuctionId, info));
    BOOST_CHECK(!db->ReadAuction(reAuctionId, info));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_SETTLEMENT);
    BOOST_CHECK(aMgr->IsDepositLocked(winner.utxo));
    BOOST_CHECK_EQUAL(GetLock(aMgr, winner.utxo).releaseHeight, 0);
    BOOST_CHECK_EQUAL(GetBid(aMgr, winner.bidId).depositState, DEPOSIT_LOCKED_ACTIVE);
    BOOST_CHECK(GetBid(aMgr, winner.bidId).isWinner);

    std::vector<uint256> auctionIds;
    BOOST_REQUIRE(db->ListAuctions(auctionIds));
    BOOST_CHECK_EQUAL(auctionIds.size(), 1U);
}

BOOST_AUTO_TEST_CASE(undo_forfeit_recreates_the_forfeited_lock)
{
    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    config.settlementWindowBlocks = 0;
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));

    uint256 auctionId = NewAuction("alice");
    DepositBid winner = PlaceDepositBid(auctionId, 8000 * COIN, 15);
    EndAuction(auctionId);
    BOOST_REQUIRE(Process(TxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, auctionId))));
    BOOST_REQUIRE(Process(TxFrom(MakeKeyID(), RecordScript(OP_AUCTION_DEFAULT, auctionId, winner.bidId))));
    CMutableTransaction forfeitTx = TxFrom(MakeKeyID(), RecordScript(OP_DEPOSIT_FORFEIT, auctionId, winner.bidId));
    BOOST_REQUIRE(Process(forfeitTx));
    DepositLock lock;
    BOOST_REQUIRE(!aMgr->GetDepositLock(winner.utxo, lock));

    BOOST_CHECK(Undo(forfeitTx));

    BOOST_REQUIRE(aMgr->GetDepositLock(winner.utxo, lock));
    BOOST_CHECK(lock.utxo == winner.utxo);
    BOOST_CHECK(lock.auctionId == auctionId);
    BOOST_CHECK(lock.bidId == winner.bidId);
    BOOST_CHECK(lock.bidder == winner.bidder);
    BOOST_CHECK_EQUAL(lock.amount, winner.deposit);
    BOOST_CHECK_EQUAL(lock.lockHeight, 15);
    BOOST_CHECK_EQUAL(lock.state, DEPOSIT_FORFEITED);
    BOOST_CHECK(db->ExistsDepositLock(winner.utxo));
    BOOST_CHECK(!aMgr->IsDepositLocked(winner.utxo));
}

BOOST_AUTO_TEST_SUITE_END()
