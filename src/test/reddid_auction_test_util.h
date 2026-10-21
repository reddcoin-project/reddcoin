// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_REDDID_AUCTION_TEST_UTIL_H
#define BITCOIN_TEST_REDDID_AUCTION_TEST_UTIL_H

#include <amount.h>
#include <chain.h>
#include <coins.h>
#include <hash.h>
#include <id/auction.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <id/transaction_handler.h>
#include <key.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
#include <sync.h>
#include <test/reddid_test_fixture.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/time.h>
#include <validation.h>
#include <version.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/** Helpers shared by the unit tests that drive AuctionManager. */
namespace reddid_test {

/** A fixed point in time for the tests, so that nothing depends on the wall clock. */
static const int64_t T0 = 1700000000;
static const int64_t DAY = 24 * 60 * 60;
static const int64_t HOUR = 60 * 60;

/** Sets the mock time for the lifetime of the object and clears it again afterwards. */
struct MockTimeGuard {
    explicit MockTimeGuard(int64_t time) { SetMockTime(time); }
    ~MockTimeGuard() { SetMockTime(0); }
};

/**
 * Replaces the tip of the active chain with a made-up one at the given height
 * for the lifetime of the object. AuctionManager reads nothing but the height
 * of the tip, and the test chain consists of the genesis block alone.
 *
 * The made-up block indexes have no block hash: anything that calls
 * Tip()->GetBlockHash() or flushes the chainstate while one of these objects
 * is alive will crash. Tests that make a winner default "at once" with a
 * settlement window of 0 rely on the tip being at height 0, where the
 * resulting deadline of 0 means "no deadline".
 */
class FakeChainHeight {
    CChain& m_chain;
    CBlockIndex* m_original;
    std::vector<std::unique_ptr<CBlockIndex>> m_blocks;

 public:
    FakeChainHeight(CChainState& chainstate, int height) : m_chain(chainstate.m_chain) {
        LOCK(cs_main);
        m_original = m_chain.Tip();
        CBlockIndex* prev = m_original;
        for (int h = prev->nHeight + 1; h <= height; h++) {
            m_blocks.push_back(std::make_unique<CBlockIndex>());
            m_blocks.back()->pprev = prev;
            m_blocks.back()->nHeight = h;
            prev = m_blocks.back().get();
        }
        m_chain.SetTip(prev);
    }
    ~FakeChainHeight() {
        LOCK(cs_main);
        m_chain.SetTip(m_original);
    }
};

inline CScript P2PKH(const CKeyID& key) { return GetScriptForDestination(PKHash(key)); }

/** Adds an unspent output paying to the script to the UTXO set and returns its outpoint. */
inline COutPoint AddCoin(CChainState& chainstate, const CScript& scriptPubKey, CAmount value = 10 * COIN) {
    COutPoint outpoint(InsecureRand256(), 0);
    LOCK(cs_main);
    chainstate.CoinsTip().AddCoin(outpoint, Coin(CTxOut(value, scriptPubKey), 1, false, false, 0), false);
    return outpoint;
}

/** Adds an unspent output owned by the key, so that the key can be the sender of a transaction. */
inline COutPoint FundKey(CChainState& chainstate, const CKeyID& key) {
    return AddCoin(chainstate, P2PKH(key));
}

/** An OP_RETURN script with the payload 'R' + opCode + body. */
inline CScript RecordScript(unsigned char opCode, const std::vector<unsigned char>& body) {
    std::vector<unsigned char> data{'R', opCode};
    data.insert(data.end(), body.begin(), body.end());
    return CScript() << OP_RETURN << data;
}

/** An OP_RETURN script with the payload 'R' + opCode + id. */
inline CScript RecordScript(unsigned char opCode, const uint256& id) {
    return RecordScript(opCode, std::vector<unsigned char>(id.begin(), id.end()));
}

/** An OP_RETURN script with the payload 'R' + opCode + id1 + id2. */
inline CScript RecordScript(unsigned char opCode, const uint256& id1, const uint256& id2) {
    std::vector<unsigned char> body(id1.begin(), id1.end());
    body.insert(body.end(), id2.begin(), id2.end());
    return RecordScript(opCode, body);
}

/** A transaction that spends the given outpoint and carries the given script in a zero-value output. */
inline CMutableTransaction RecordTx(const COutPoint& input, const CScript& record) {
    CMutableTransaction mtx;
    mtx.vin.emplace_back(input);
    mtx.vout.emplace_back(0, record);
    return mtx;
}

/** A user ID auction that has not been handed to the auction manager yet. */
inline AuctionInfo UserAuction(const std::string& name, const std::string& namespaceId, const CKeyID& creator,
                               CAmount reserve, int64_t startTime, int64_t duration = 7 * DAY) {
    AuctionInfo auction;
    auction.name = name;
    auction.namespaceId = namespaceId;
    auction.creator = creator;
    auction.reservePrice = reserve;
    auction.startTime = startTime;
    auction.endTime = startTime + duration;
    auction.type = AUCTION_STANDARD;
    return auction;
}

/** The auction ID, computed independently of AuctionManager. */
inline uint256 ExpectedAuctionId(const std::string& name, const std::string& namespaceId, int64_t startTime) {
    CHashWriter ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << name << namespaceId << startTime;
    return ss.GetHash();
}

/** The bid ID, computed independently of AuctionManager. */
inline uint256 ExpectedBidId(const uint256& auctionId, const CKeyID& bidder, CAmount bidAmount, int64_t bidTime) {
    CHashWriter ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << auctionId << bidder << bidAmount << bidTime;
    return ss.GetHash();
}

inline BidInfo GetBid(AuctionManager* manager, const uint256& bidId) {
    BidInfo bid;
    bool found = manager->GetBidInfo(bidId, bid);
    assert(found);
    return bid;
}

inline AuctionInfo GetAuction(AuctionManager* manager, const uint256& auctionId) {
    AuctionInfo auction;
    bool found = manager->GetAuctionInfo(auctionId, auction);
    assert(found);
    return auction;
}

inline DepositLock GetLock(AuctionManager* manager, const COutPoint& utxo) {
    DepositLock lock;
    bool found = manager->GetDepositLock(utxo, lock);
    assert(found);
    return lock;
}

} // namespace reddid_test

/**
 * ReddIDTestingSetup with the clock stopped at T0, a namespace to hold user
 * ID auctions in, and shorthands for building and processing auction
 * transactions.
 *
 * The chain consists of the genesis block alone. AuctionManager takes the
 * sender of a transaction from the UTXO set, so TxFrom() puts an output owned
 * by the sender there and spends it; a transaction built in any other way has
 * no sender.
 */
struct AuctionTestingSetup : public ReddIDTestingSetup {
    reddid_test::MockTimeGuard m_mock_time{reddid_test::T0};
    const std::string NS{"aucns"};
    AuctionManager* aMgr;
    ReddIDDB* db;
    CChainState& chainstate;
    CKeyID creator;

    /** A bid placed with a deposit: the transaction that carried it and what it created. */
    struct DepositBid {
        CKeyID bidder;
        CAmount amount;
        CAmount deposit;
        uint256 bidId;
        COutPoint utxo;
        CMutableTransaction tx;
    };

    AuctionTestingSetup()
        : aMgr(reddidManager->GetAuctionManager()),
          db(reddidManager->GetReddIDDB()),
          chainstate(m_node.chainman->ActiveChainstate()),
          creator(MakeKeyID())
    {
        CreateTestNamespace(NS, creator);
    }

    /** Create a user ID auction in NS that starts now. */
    uint256 NewAuction(const std::string& name, CAmount reserve = 5000 * COIN, int64_t duration = 7 * reddid_test::DAY)
    {
        AuctionInfo auction = reddid_test::UserAuction(name, NS, creator, reserve, GetTime(), duration);
        uint256 auctionId;
        bool created = aMgr->CreateAuction(auction, auctionId);
        assert(created);
        return auctionId;
    }

    /** A transaction carrying the record whose sender is the given key. */
    CMutableTransaction TxFrom(const CKeyID& sender, const CScript& record)
    {
        return reddid_test::RecordTx(reddid_test::FundKey(chainstate, sender), record);
    }

    bool Process(const CMutableTransaction& mtx, int height = 1)
    {
        return aMgr->ProcessTransaction(CTransaction(mtx), height);
    }

    bool Undo(const CMutableTransaction& mtx, int height = 1)
    {
        return aMgr->UndoTransaction(CTransaction(mtx), height);
    }

    /**
     * A transaction that bids with a deposit: output 0 is the record, output 1
     * the deposit paying the bidder. The sender is the bidder.
     */
    DepositBid MakeDepositBid(const uint256& auctionId, const CKeyID& bidder, CAmount amount, CAmount deposit)
    {
        DepositBid bid;
        bid.bidder = bidder;
        bid.amount = amount;
        bid.deposit = deposit;
        bid.tx = TxFrom(bidder, CreateBidDepositOpReturn(auctionId, amount, 1, bidder));
        bid.tx.vout.emplace_back(deposit, reddid_test::P2PKH(bidder));
        bid.bidId = reddid_test::ExpectedBidId(auctionId, bidder, amount, GetTime());
        bid.utxo = COutPoint(bid.tx.GetHash(), 1);
        return bid;
    }

    /** Place a bid with the minimum deposit of 15% and require that it is accepted. */
    DepositBid PlaceDepositBid(const uint256& auctionId, const CKeyID& bidder, CAmount amount, int height = 1)
    {
        DepositBid bid = MakeDepositBid(auctionId, bidder, amount, amount * 15 / 100);
        bool accepted = Process(bid.tx, height);
        assert(accepted);
        return bid;
    }

    DepositBid PlaceDepositBid(const uint256& auctionId, CAmount amount, int height = 1)
    {
        return PlaceDepositBid(auctionId, MakeKeyID(), amount, height);
    }

    /** Move the clock to the end of the auction. */
    void EndAuction(const uint256& auctionId)
    {
        SetMockTime(reddid_test::GetAuction(aMgr, auctionId).endTime);
    }
};

#endif // BITCOIN_TEST_REDDID_AUCTION_TEST_UTIL_H
