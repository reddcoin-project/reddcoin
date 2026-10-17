// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <amount.h>
#include <chain.h>
#include <chainparams.h>
#include <clientversion.h>
#include <coins.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <crypto/common.h>
#include <id/auction.h>
#include <id/namespace.h>
#include <id/profile.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <id/reddid_p2p.h>
#include <id/transaction_handler.h>
#include <key.h>
#include <miner.h>
#include <pos/signer.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <streams.h>
#include <sync.h>
#include <test/reddid_auction_test_util.h>
#include <test/reddid_p2p_test_util.h>
#include <test/reddid_profile_test_util.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/system.h>
#include <util/time.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/staking.h>
#include <wallet/wallet.h>

#include <array>
#include <cassert>
#include <deque>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace reddid_test;

namespace {
/** 'R' + OP_NAMESPACE_AUCTION_BID + auction ID + amount */
CScript NamespaceBidRecord(const uint256& auctionId, CAmount amount)
{
    std::vector<unsigned char> body(auctionId.begin(), auctionId.end());
    body.resize(40);
    WriteLE64(body.data() + 32, amount);
    return RecordScript(OP_NAMESPACE_AUCTION_BID, body);
}
} // namespace

/**
 * A ReddIDManager attached to the chainstate of a real chain: 89 proof-of-work
 * blocks, then proof-of-stake blocks. Blocks are staked by the wallet of
 * TestChain100Setup; the transactions under test spend outputs of keys that
 * the wallet does not have, so that staking never competes for them.
 *
 * The keys are fixed and the clock is the mock clock of TestChain100Setup,
 * which every block moves on, so the chain is the same in every run.
 *
 * The manager is started without the scheduler: its periodic task does not
 * run here.
 */
struct ReddIDChainTestingSetup : public TestChain100Setup {
    static constexpr CAmount COIN_VALUE{100000 * COIN};
    static constexpr CAmount FEE{10 * COIN};
    const std::string NS{"chain"};

    std::unique_ptr<ReddIDManager> reddidManager;
    NamespaceManager* nsMgr{nullptr};
    AuctionManager* aMgr{nullptr};
    ProfileManager* pMgr{nullptr};
    ReddIDDB* db{nullptr};
    CChainState& chainstate;

    FillableSigningProvider keystore;
    CKeyID creator, alice, bob, carol, dave;
    /** The confirmed outputs of each key that no transaction of the test has spent yet. */
    std::map<CKeyID, std::deque<COutPoint>> coins;

    /** A bid with a deposit: the transaction that carries it and the output that it locks. */
    struct DepositBid {
        CKeyID bidder;
        CAmount amount;
        CAmount deposit;
        COutPoint utxo;
        CMutableTransaction tx;
    };

    ReddIDChainTestingSetup() : chainstate(m_node.chainman->ActiveChainstate())
    {
        gArgs.ForceSetArg("-reddid", "1");
        gArgs.ForceSetArg("-reddidbmemory", "1");

        reddidManager = std::make_unique<ReddIDManager>(m_node);
        bool initOk = reddidManager->Init();
        assert(initOk);
        // Start() must not hand the periodic task to the scheduler of the fixture.
        // The scheduler thread reads m_node.scheduler when it begins to run, so
        // wait until it services the queue before the pointer is taken away.
        SyncWithValidationInterfaceQueue();
        auto savedScheduler = std::move(m_node.scheduler);
        bool startOk = reddidManager->Start();
        m_node.scheduler = std::move(savedScheduler);
        assert(startOk);
        nsMgr = reddidManager->GetNamespaceManager();
        aMgr = reddidManager->GetAuctionManager();
        pMgr = reddidManager->GetProfileManager();
        db = reddidManager->GetReddIDDB();
        WITH_LOCK(cs_main, chainstate.SetReddIDManager(reddidManager.get()));

        keystore.AddKey(coinbaseKey);
        creator = AddKey(2);
        alice = AddKey(3);
        bob = AddKey(4);
        carol = AddKey(5);
        dave = AddKey(6);

        CreateNamespace(NS, creator);
        Fund({creator, alice, bob, carol});
    }

    ~ReddIDChainTestingSetup()
    {
        // The chainstate must not point to a manager that no longer exists
        WITH_LOCK(cs_main, chainstate.SetReddIDManager(nullptr));
        reddidManager->Stop();
        reddidManager.reset();
    }

    CKeyID AddKey(unsigned char n)
    {
        std::array<unsigned char, 32> bytes{};
        bytes[31] = n;
        CKey key;
        key.Set(bytes.begin(), bytes.end(), true);
        assert(key.IsValid());
        keystore.AddKey(key);
        return key.GetPubKey().GetID();
    }

    CScript StakeScript() const { return CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG; }

    int Height() const { return WITH_LOCK(cs_main, return chainstate.m_chain.Height()); }
    uint256 TipHash() const { return WITH_LOCK(cs_main, return chainstate.m_chain.Tip()->GetBlockHash()); }
    CBlockIndex* IndexAt(int height) const { return WITH_LOCK(cs_main, return chainstate.m_chain[height]); }
    bool HaveCoin(const COutPoint& outpoint) const { return WITH_LOCK(cs_main, return chainstate.CoinsTip().HaveCoin(outpoint)); }

    /** As ReddIDTestingSetup::CreateTestNamespace: a namespace that exists without an auction. */
    void CreateNamespace(const std::string& nsId, const CKeyID& owner)
    {
        NamespaceInfo nsInfo;
        nsInfo.id = nsId;
        nsInfo.owner = owner;
        nsInfo.allowNumbers = true;
        nsInfo.allowHyphens = true;
        nsInfo.allowUnderscores = true;
        nsInfo.minLength = 3;
        nsInfo.maxLength = 32;
        nsInfo.renewalPeriod = 365;
        nsInfo.gracePeriod = 30;
        nsInfo.namespaceRevenuePct = 10;
        nsInfo.burnPct = 70;
        nsInfo.nodePct = 15;
        nsInfo.devPct = 5;
        nsInfo.minAuctionDuration = 3;
        nsInfo.maxAuctionDuration = 7;
        nsInfo.minBidIncrement = 5.0;
        bool ok = nsMgr->CreateNamespace(nsInfo);
        assert(ok);
    }

    /** Let the wallet catch up with the chain, as TestChain100Setup::stakeBlocks does after each block. */
    void SyncWallet()
    {
        SyncWithValidationInterfaceQueue();
        m_wallet->BlockUntilSyncedToCurrentChain();
        {
            LOCK(m_wallet->cs_wallet);
            m_wallet->ReacceptWalletTransactions();
        }
        SyncWithValidationInterfaceQueue();
        m_wallet->BlockUntilSyncedToCurrentChain();
    }

    /**
     * Stake a block on the tip that holds the transactions of the mempool and
     * then the given ones, and require that it becomes the tip.
     */
    CBlock Stake(const std::vector<CMutableTransaction>& txns = {})
    {
        CBlock block = CreateAndProcessPoSBlock(txns, StakeScript(), m_wallet);
        assert(TipHash() == block.GetHash());
        m_coinbase_txns.push_back(block.vtx[1]);
        SyncWallet();
        SetMockTime(GetTime() + 60);
        return block;
    }

    /** As Stake(), for a block that is expected to be rejected. Returns whether the tip changed. */
    bool TryStake(const std::vector<CMutableTransaction>& txns)
    {
        const uint256 before = TipHash();
        try {
            CreateAndProcessPoSBlock(txns, StakeScript(), m_wallet);
        } catch (const std::runtime_error&) {
        }
        SyncWallet();
        SetMockTime(GetTime() + 60);
        return TipHash() != before;
    }

    /** Sign every input. The outputs spent are those of the UTXO set and of the given unconfirmed transactions. */
    void Sign(CMutableTransaction& mtx, const std::vector<CMutableTransaction>& parents = {})
    {
        std::map<COutPoint, Coin> inputCoins;
        {
            LOCK(cs_main);
            for (const CTxIn& txin : mtx.vin) {
                Coin coin;
                if (chainstate.CoinsTip().GetCoin(txin.prevout, coin)) inputCoins.emplace(txin.prevout, coin);
            }
        }
        for (const CMutableTransaction& parent : parents) {
            const uint256 hash = parent.GetHash();
            for (unsigned int n = 0; n < parent.vout.size(); n++) {
                inputCoins.emplace(COutPoint(hash, n), Coin(parent.vout[n], 1, false, false, 0));
            }
        }
        std::map<int, std::string> errors;
        bool ok = SignTransaction(mtx, &keystore, inputCoins, SIGHASH_ALL, errors);
        assert(ok);
    }

    CMutableTransaction Build(const std::vector<COutPoint>& inputs, const std::vector<CTxOut>& outputs,
                              const std::vector<CMutableTransaction>& parents = {})
    {
        CMutableTransaction mtx;
        // As the other tests that put transactions into proof-of-stake blocks
        mtx.nVersion = 2;
        mtx.nTime = 1;
        for (const COutPoint& input : inputs) mtx.vin.emplace_back(input);
        mtx.vout = outputs;
        Sign(mtx, parents);
        return mtx;
    }

    COutPoint TakeCoin(const CKeyID& key)
    {
        assert(!coins[key].empty());
        COutPoint coin = coins[key].front();
        coins[key].pop_front();
        return coin;
    }

    /**
     * Give each of the keys eight outputs of COIN_VALUE, from a coinbase of the
     * staking wallet. The transaction goes through the mempool, so that the
     * wallet knows that the coinbase is spent and does not stake with it.
     */
    void Fund(const std::vector<CKeyID>& keys)
    {
        CTransactionRef source;
        {
            LOCK(cs_main);
            for (size_t i = 0; i < 40 && !source; i++) {
                if (chainstate.CoinsTip().HaveCoin(COutPoint(m_coinbase_txns[i]->GetHash(), 0))) source = m_coinbase_txns[i];
            }
        }
        assert(source);
        std::vector<CTxOut> outputs;
        for (const CKeyID& key : keys) {
            for (int i = 0; i < 8; i++) outputs.emplace_back(COIN_VALUE, P2PKH(key));
        }
        const CAmount change = source->vout[0].nValue - FEE - COIN_VALUE * static_cast<CAmount>(outputs.size());
        assert(change > 0);
        outputs.emplace_back(change, StakeScript());
        CMutableTransaction funding = Build({COutPoint(source->GetHash(), 0)}, outputs);
        bool accepted = ToMempool(funding);
        assert(accepted);
        SyncWithValidationInterfaceQueue();
        Stake();
        assert(m_node.mempool->size() == 0);
        const uint256 hash = funding.GetHash();
        unsigned int n = 0;
        for (const CKeyID& key : keys) {
            for (int i = 0; i < 8; i++) {
                assert(HaveCoin(COutPoint(hash, n)));
                coins[key].emplace_back(hash, n++);
            }
        }
    }

    /** Submit to the mempool. The reject reason is left in m_reject_reason. */
    bool ToMempool(const CMutableTransaction& mtx)
    {
        LOCK(cs_main);
        const MempoolAcceptResult result = AcceptToMemoryPool(chainstate, *m_node.mempool, MakeTransactionRef(mtx), /* bypass_limits */ false);
        m_reject_reason = result.m_state.GetRejectReason();
        m_reject_result = result.m_state.GetResult();
        return result.m_result_type == MempoolAcceptResult::ResultType::VALID;
    }
    std::string m_reject_reason;
    TxValidationResult m_reject_result{TxValidationResult::TX_RESULT_UNSET};

    /** A transaction of the key that carries the record in output 0 and pays the rest back to the key. */
    CMutableTransaction RecordTxFrom(const CKeyID& sender, const CScript& record)
    {
        return Build({TakeCoin(sender)}, {CTxOut(0, record), CTxOut(COIN_VALUE - FEE, P2PKH(sender))});
    }

    /** A user ID auction in NS that starts now, created through the auction manager and not through a block. */
    uint256 NewAuction(const std::string& name, int64_t duration = 3 * DAY)
    {
        AuctionInfo auction = UserAuction(name, NS, creator, 5000 * COIN, GetTime(), duration);
        uint256 auctionId;
        bool created = aMgr->CreateAuction(auction, auctionId);
        assert(created);
        return auctionId;
    }

    /** Output 0 is the record, output 1 the deposit of 15% paying the bidder, output 2 the change. */
    DepositBid MakeDepositBid(const uint256& auctionId, const CKeyID& bidder, CAmount amount)
    {
        DepositBid bid;
        bid.bidder = bidder;
        bid.amount = amount;
        bid.deposit = amount * 15 / 100;
        bid.tx = Build({TakeCoin(bidder)}, {CTxOut(0, CreateBidDepositOpReturn(auctionId, amount, 1, bidder)),
                                            CTxOut(bid.deposit, P2PKH(bidder)),
                                            CTxOut(COIN_VALUE - FEE - bid.deposit, P2PKH(bidder))});
        bid.utxo = COutPoint(bid.tx.GetHash(), 1);
        return bid;
    }

    /** A transaction that spends the deposit and pays it, less the fee, to its owner, with an optional record. */
    CMutableTransaction SpendDeposit(const DepositBid& bid, const CScript& record = CScript(),
                                     const std::vector<CMutableTransaction>& parents = {})
    {
        std::vector<CTxOut> outputs{CTxOut(bid.deposit - FEE, P2PKH(bid.bidder))};
        if (!record.empty()) outputs.emplace_back(0, record);
        return Build({bid.utxo}, outputs, parents);
    }

    std::vector<BidInfo> Bids(const uint256& auctionId) const { return aMgr->GetAuctionBids(auctionId); }

    /** The bid of the bidder on the auction; there must be exactly one that was not replaced. */
    BidInfo BidOf(const uint256& auctionId, const CKeyID& bidder) const
    {
        BidInfo found;
        int matches = 0;
        for (const BidInfo& bid : Bids(auctionId)) {
            if (bid.bidder == bidder) {
                found = bid;
                matches++;
            }
        }
        assert(matches == 1);
        return found;
    }

    bool HasLock(const COutPoint& utxo) const
    {
        DepositLock lock;
        return aMgr->GetDepositLock(utxo, lock);
    }

    /**
     * Everything the managers and the database hold for the auctions, and the
     * given profiles, serialized. Two snapshots are equal if no record changed.
     */
    std::string Snapshot(const std::vector<std::string>& profiles = {}) const
    {
        CDataStream ss(SER_DISK, CLIENT_VERSION);
        std::vector<uint256> auctionIds;
        db->ListAuctions(auctionIds);
        ss << auctionIds;
        for (const uint256& auctionId : auctionIds) {
            AuctionInfo auction;
            if (db->ReadAuction(auctionId, auction)) ss << auction;
            if (aMgr->GetAuctionInfo(auctionId, auction)) ss << auction;
            if (nsMgr->GetAuctionInfo(auctionId, auction)) ss << auction;
            for (const BidInfo& bid : aMgr->GetAuctionBids(auctionId)) {
                ss << bid;
                BidInfo stored;
                if (db->ReadBid(bid.bidId, stored)) ss << stored;
            }
            for (const BidInfo& bid : nsMgr->GetAuctionBids(auctionId)) ss << bid;
            for (const DepositLock& lock : aMgr->GetDepositLocksForAuction(auctionId)) {
                ss << lock;
                DepositLock stored;
                if (db->ReadDepositLock(lock.utxo, stored)) ss << stored;
            }
        }
        for (const std::string& reddId : profiles) {
            ReddIDProfile profile;
            if (pMgr->GetProfile(reddId, profile)) ss << profile;
        }
        return ss.str();
    }

    /** Move the clock past the end of the auction. Later blocks carry the later time. */
    void EndAuction(const uint256& auctionId)
    {
        SetMockTime(GetAuction(aMgr, auctionId).endTime + 1);
    }

    /** What BeginSettlement left behind, for the settlement tests. */
    struct Settlement {
        uint256 auctionId;
        DepositBid loser;
        DepositBid winner;
        uint256 winningBidId;
        int finalizeHeight;
    };

    /**
     * An auction with two deposit bids, ended, whose finalize record is in the
     * block at finalizeHeight. The settlement window is two blocks.
     */
    Settlement StartSettlement(const std::string& name)
    {
        AuctionConfig config;
        bool haveConfig = aMgr->GetAuctionConfig("userid", config);
        assert(haveConfig);
        config.settlementWindowBlocks = 2;
        bool configSet = aMgr->SetAuctionConfig(config);
        assert(configSet);

        Settlement s;
        s.auctionId = NewAuction(name);
        s.loser = MakeDepositBid(s.auctionId, alice, 6000 * COIN);
        s.winner = MakeDepositBid(s.auctionId, bob, 7000 * COIN);
        Stake({s.loser.tx, s.winner.tx});
        assert(aMgr->IsDepositLocked(s.loser.utxo));
        assert(aMgr->IsDepositLocked(s.winner.utxo));
        s.winningBidId = BidOf(s.auctionId, bob).bidId;

        EndAuction(s.auctionId);
        Stake({RecordTxFrom(creator, RecordScript(OP_AUCTION_FINALIZE, s.auctionId))});
        s.finalizeHeight = Height();
        assert(GetAuction(aMgr, s.auctionId).state == AUCTION_SETTLEMENT);
        return s;
    }
};

/**
 * The hooks of the validation code: ReddID records are applied when a block
 * becomes the tip of the active chain (CChainState::ConnectTip), undone when
 * it stops being the tip (CChainState::DisconnectTip), and the mempool
 * refuses to spend a locked deposit (MemPoolAccept::PreChecks). Every case
 * runs on a real chain.
 */
BOOST_FIXTURE_TEST_SUITE(reddid_chain_tests, ReddIDChainTestingSetup)

// ===================================================================
// Category 1: Connecting blocks
// ===================================================================

BOOST_AUTO_TEST_CASE(fixture_is_attached_to_a_real_chain)
{
    BOOST_CHECK_EQUAL(Height(), 101);
    BOOST_CHECK(WITH_LOCK(cs_main, return chainstate.GetReddIDManager()) == reddidManager.get());
    BOOST_CHECK(reddidManager->GetChainState() == &chainstate);
    BOOST_CHECK(IndexAt(101)->IsProofOfStake());
    for (const CKeyID& key : {creator, alice, bob, carol}) {
        BOOST_CHECK_EQUAL(coins[key].size(), 8U);
    }
    BOOST_CHECK(nsMgr->IsOwner(NS, creator));
}

BOOST_AUTO_TEST_CASE(block_applies_its_records_once_at_its_height)
{
    uint256 auctionId = NewAuction("erin");
    uint256 nsAuctionId;
    const CAmount nsReserve = nsMgr->CalculateMinPrice("freshns");
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("freshns", creator, nsReserve, 3, AUCTION_STANDARD, nsAuctionId));
    BOOST_REQUIRE(pMgr->CreateProfile(TestProfile("alice", alice)));
    const int64_t profileCreated = GetTime();

    DepositBid bid = MakeDepositBid(auctionId, alice, 6000 * COIN);
    CMutableTransaction nsBid = RecordTxFrom(bob, NamespaceBidRecord(nsAuctionId, nsReserve));
    CMutableTransaction activity = RecordTxFrom(carol, CreateProfileUpdateOpReturn("alice", uint256()));

    // Nothing is applied before the block
    BOOST_CHECK(Bids(auctionId).empty());
    BOOST_CHECK(!HasLock(bid.utxo));

    const int height = Height() + 1;
    SetMockTime(GetTime() + 1000);
    const int64_t connectTime = GetTime();
    CBlock block;
    {
        LogCounter depositSeen("Processing transaction " + bid.tx.GetHash().ToString() + " for auction operations");
        LogCounter profileSeen("Received ReddID profile update transaction: " + activity.GetHash().ToString());
        block = Stake({bid.tx, nsBid, activity});
        BOOST_CHECK_EQUAL(depositSeen.count, 1);
        BOOST_CHECK_EQUAL(profileSeen.count, 1);
    }
    BOOST_REQUIRE_EQUAL(Height(), height);
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 5U);
    BOOST_CHECK(block.vtx[0]->IsCoinBase());
    BOOST_CHECK(block.vtx[1]->IsCoinStake());

    // The bid with its deposit, locked at the height of the block
    DepositLock lock = GetLock(aMgr, bid.utxo);
    BOOST_CHECK_EQUAL(lock.lockHeight, height);
    BOOST_CHECK_EQUAL(lock.releaseHeight, 0);
    BOOST_CHECK_EQUAL(lock.state, DEPOSIT_LOCKED_ACTIVE);
    BOOST_CHECK_EQUAL(lock.amount, 900 * COIN);
    BOOST_CHECK(lock.bidder == alice);
    BOOST_CHECK(lock.auctionId == auctionId);
    BOOST_REQUIRE_EQUAL(Bids(auctionId).size(), 1U);
    BidInfo stored = BidOf(auctionId, alice);
    BOOST_CHECK_EQUAL(stored.bidAmount, 6000 * COIN);
    BOOST_CHECK(stored.depositUtxo == bid.utxo);
    BOOST_CHECK_EQUAL(stored.depositLockHeight, height);
    BOOST_CHECK_EQUAL(stored.bidTime, connectTime);
    BOOST_CHECK(lock.bidId == stored.bidId);
    AuctionInfo auction = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(auction.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(auction.currentBid, 6000 * COIN);
    BOOST_CHECK(auction.currentBidder == alice);
    BOOST_CHECK_EQUAL(auction.totalDepositsLocked, 900 * COIN);
    // The locked output exists in the UTXO set
    BOOST_CHECK(HaveCoin(bid.utxo));

    // The bid on the namespace auction; the bidder is the first key that the transaction pays
    std::vector<BidInfo> nsBids = nsMgr->GetAuctionBids(nsAuctionId);
    BOOST_REQUIRE_EQUAL(nsBids.size(), 1U);
    BOOST_CHECK(nsBids[0].bidder == bob);
    BOOST_CHECK_EQUAL(nsBids[0].bidAmount, nsReserve);
    AuctionInfo nsAuction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(nsAuctionId, nsAuction));
    BOOST_CHECK_EQUAL(nsAuction.state, AUCTION_ACTIVE);

    // The profile was active at the time the block was connected
    ReddIDProfile profile;
    BOOST_REQUIRE(pMgr->GetProfile("alice", profile));
    BOOST_CHECK_EQUAL(profile.lastActivityTime, connectTime);
    BOOST_CHECK(profile.lastActivityTime != profileCreated);

    // Later blocks leave all of it alone
    const std::string before = Snapshot({"alice"});
    {
        LogCounter anything("Processing transaction " + bid.tx.GetHash().ToString());
        Stake();
        Stake();
        BOOST_CHECK_EQUAL(anything.count, 0);
    }
    BOOST_CHECK(Snapshot({"alice"}) == before);
    BOOST_CHECK_EQUAL(Bids(auctionId).size(), 1U);
}

// The auction manager takes the sender of a transaction from the output that
// its first input spends, which it looks up in the UTXO set. The records are
// applied before the block's changes to the UTXO set are flushed, so the
// output is still there.
BOOST_AUTO_TEST_CASE(sender_is_found_for_an_input_confirmed_in_an_earlier_block)
{
    uint256 auctionId = NewAuction("erin");
    // An auction made by a create record has a reserve price of 1000, which the namespace must allow
    BOOST_REQUIRE(nsMgr->AddPricingTier(NS, 3, 1000 * COIN));
    CMutableTransaction bid = RecordTxFrom(bob, CreateUserIDBidOpReturn(auctionId, 5000 * COIN));
    const COutPoint spent = bid.vin[0].prevout;
    CMutableTransaction create = RecordTxFrom(carol, CreateUserIDAuctionOpReturn("frank", NS));
    BOOST_REQUIRE(HaveCoin(spent));

    const int64_t connectTime = GetTime();
    Stake({bid, create});

    // The block spent the outputs
    BOOST_CHECK(!HaveCoin(spent));
    BOOST_CHECK(!HaveCoin(create.vin[0].prevout));

    std::vector<BidInfo> bids = Bids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK(bids[0].bidder == bob);
    BOOST_CHECK_EQUAL(bids[0].bidAmount, 5000 * COIN);

    // The auction that the create record made has the sender as its creator
    AuctionInfo created = GetAuction(aMgr, ExpectedAuctionId("frank", NS, connectTime));
    BOOST_CHECK(created.creator == carol);
    BOOST_CHECK(created.txid == create.GetHash());
    BOOST_CHECK_EQUAL(created.state, AUCTION_PENDING);
    // Current behaviour: the height recorded for the auction is the height of
    // the tip while the block is being connected, which is the block before
    BOOST_CHECK_EQUAL(created.blockHeight, Height() - 1);
}

// Current behaviour: the outputs that a block creates are not in the UTXO set
// yet when its records are applied. A transaction whose only input was
// created earlier in the same block has no sender, and its auction record is
// dropped. The block itself is valid.
BOOST_AUTO_TEST_CASE(record_is_dropped_when_its_input_was_created_in_the_same_block)
{
    uint256 auctionId = NewAuction("erin");
    // Bob pays dave, and dave bids with what he was paid, in one block
    CMutableTransaction payment = Build({TakeCoin(bob)}, {CTxOut(COIN_VALUE - FEE, P2PKH(dave))});
    CMutableTransaction bid = Build({COutPoint(payment.GetHash(), 0)},
                                    {CTxOut(0, CreateUserIDBidOpReturn(auctionId, 5000 * COIN)), CTxOut(COIN_VALUE - 2 * FEE, P2PKH(dave))},
                                    {payment});

    {
        ASSERT_DEBUG_LOG("Error: AuctionManager::ProcessTransaction: Failed to determine transaction sender");
        Stake({payment, bid});
    }
    BOOST_CHECK(HaveCoin(COutPoint(bid.GetHash(), 1)));
    BOOST_CHECK(Bids(auctionId).empty());
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_PENDING);

    // The same bid from the output of that transaction, one block later, is taken
    CMutableTransaction later = Build({COutPoint(bid.GetHash(), 1)},
                                      {CTxOut(0, CreateUserIDBidOpReturn(auctionId, 5000 * COIN)), CTxOut(COIN_VALUE - 3 * FEE, P2PKH(dave))});
    Stake({later});
    BOOST_REQUIRE_EQUAL(Bids(auctionId).size(), 1U);
    BOOST_CHECK(Bids(auctionId)[0].bidder == dave);
}

// SUSPECTED BUG (documented, not fixed): the sender is the owner of the first
// input that is found in the UTXO set, not of the first input. When the first
// input was created in the same block the record is attributed to the owner
// of the second.
BOOST_AUTO_TEST_CASE(record_is_attributed_to_the_second_input_when_the_first_was_created_in_the_same_block)
{
    uint256 auctionId = NewAuction("erin");
    CMutableTransaction payment = Build({TakeCoin(bob)}, {CTxOut(COIN_VALUE - FEE, P2PKH(dave))});
    CMutableTransaction bid = Build({COutPoint(payment.GetHash(), 0), TakeCoin(carol)},
                                    {CTxOut(0, CreateUserIDBidOpReturn(auctionId, 5000 * COIN)), CTxOut(COIN_VALUE, P2PKH(dave))},
                                    {payment});

    Stake({payment, bid});

    BOOST_REQUIRE_EQUAL(Bids(auctionId).size(), 1U);
    BOOST_CHECK(Bids(auctionId)[0].bidder == carol);
}

// A block whose last transaction has a bad signature fails validation after
// every transaction in it was looked at. None of its records is applied.
BOOST_AUTO_TEST_CASE(block_that_fails_validation_applies_nothing)
{
    uint256 auctionId = NewAuction("erin");
    uint256 nsAuctionId;
    const CAmount nsReserve = nsMgr->CalculateMinPrice("freshns");
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("freshns", creator, nsReserve, 3, AUCTION_STANDARD, nsAuctionId));
    BOOST_REQUIRE(pMgr->CreateProfile(TestProfile("alice", alice)));

    DepositBid bid = MakeDepositBid(auctionId, alice, 6000 * COIN);
    CMutableTransaction nsBid = RecordTxFrom(bob, NamespaceBidRecord(nsAuctionId, nsReserve));
    CMutableTransaction activity = RecordTxFrom(carol, CreateProfileUpdateOpReturn("alice", uint256()));
    // Signed, and then changed
    CMutableTransaction bad = RecordTxFrom(creator, CreateUserIDBidOpReturn(auctionId, 9000 * COIN));
    bad.vout[1].nValue -= 1;

    const std::string before = Snapshot({"alice"});
    const int height = Height();
    {
        LogCounter processed("Processing transaction " + bid.tx.GetHash().ToString());
        LogCounter profileSeen("Received ReddID profile update transaction");
        ASSERT_DEBUG_LOG("ConnectTip: ConnectBlock");
        BOOST_CHECK(!TryStake({bid.tx, nsBid, activity, bad}));
        BOOST_CHECK_EQUAL(processed.count, 0);
        BOOST_CHECK_EQUAL(profileSeen.count, 0);
    }
    BOOST_CHECK_EQUAL(Height(), height);
    BOOST_CHECK(Snapshot({"alice"}) == before);
    BOOST_CHECK(Bids(auctionId).empty());
    BOOST_CHECK(!HasLock(bid.utxo));
    BOOST_CHECK(nsMgr->GetAuctionBids(nsAuctionId).empty());
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_PENDING);

    // The same transactions without the bad one, in a block that is valid
    Stake({bid.tx, nsBid, activity});
    BOOST_CHECK_EQUAL(Height(), height + 1);
    BOOST_CHECK_EQUAL(GetLock(aMgr, bid.utxo).lockHeight, height + 1);
    BOOST_CHECK_EQUAL(Bids(auctionId).size(), 1U);
    BOOST_CHECK_EQUAL(nsMgr->GetAuctionBids(nsAuctionId).size(), 1U);
}

// The chain is taken back into the proof-of-work era, where the test can
// choose the script that the coinbase pays to.
BOOST_AUTO_TEST_CASE(record_in_a_coinbase_is_skipped)
{
    BOOST_REQUIRE(pMgr->CreateProfile(TestProfile("alice", alice)));
    BOOST_REQUIRE(pMgr->CreateProfile(TestProfile("bob", bob)));
    const int64_t created = GetTime();

    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, IndexAt(85)));
    BOOST_REQUIRE_EQUAL(Height(), 84);
    m_node.mempool->clear();

    // The coinbase pays to a record for alice; a transaction that spends the
    // coinbase of block 1 carries the same kind of record for bob
    const CScript aliceRecord = CreateProfileUpdateOpReturn("alice", uint256());
    CMutableTransaction spend = Build({COutPoint(m_coinbase_txns[0]->GetHash(), 0)},
                                      {CTxOut(0, CreateProfileUpdateOpReturn("bob", uint256())),
                                       CTxOut(m_coinbase_txns[0]->vout[0].nValue - FEE, StakeScript())});
    SetMockTime(GetTime() + 1000);
    CBlock block = CreateAndProcessBlock({spend}, aliceRecord);
    BOOST_REQUIRE_EQUAL(Height(), 85);
    BOOST_REQUIRE(TipHash() == block.GetHash());
    BOOST_REQUIRE(block.vtx[0]->IsCoinBase());
    BOOST_REQUIRE(block.vtx[0]->vout[0].scriptPubKey == aliceRecord);

    ReddIDProfile profile;
    BOOST_REQUIRE(pMgr->GetProfile("alice", profile));
    BOOST_CHECK_EQUAL(profile.lastActivityTime, created);
    BOOST_REQUIRE(pMgr->GetProfile("bob", profile));
    BOOST_CHECK(profile.lastActivityTime > created);
}

// The staking code builds the coinstake, so the test adds an output with a
// record to the coinstake of a block template and signs the transaction and
// the block again.
BOOST_AUTO_TEST_CASE(record_in_a_coinstake_is_skipped)
{
    BOOST_REQUIRE(pMgr->CreateProfile(TestProfile("alice", alice)));
    const int64_t created = GetTime();
    SetMockTime(GetTime() + 1000);

    std::unique_ptr<CBlockTemplate> blockTemplate;
    bool cancel = false;
    {
        LOCK(m_wallet->cs_wallet);
        auto stakingWallet = MakeStakingWallet(m_wallet);
        blockTemplate = BlockAssembler(chainstate, *m_node.mempool, Params()).CreateNewBlock(StakeScript(), stakingWallet.get(), &cancel);
    }
    BOOST_REQUIRE(blockTemplate);
    CBlock block = blockTemplate->block;
    BOOST_REQUIRE(block.IsProofOfStake());

    CMutableTransaction coinstake(*block.vtx[1]);
    coinstake.vout.emplace_back(0, CreateProfileUpdateOpReturn("alice", uint256()));
    for (CTxIn& txin : coinstake.vin) txin.scriptSig.clear();
    Sign(coinstake);
    BOOST_REQUIRE(CTransaction(coinstake).IsCoinStake());
    block.vtx[1] = MakeTransactionRef(coinstake);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    {
        LOCK(m_wallet->cs_wallet);
        BOOST_REQUIRE(SignBlock(block, *m_wallet));
    }

    const int height = Height();
    BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(Params(), std::make_shared<const CBlock>(block), true, nullptr));
    BOOST_REQUIRE_EQUAL(Height(), height + 1);
    BOOST_REQUIRE(TipHash() == block.GetHash());

    ReddIDProfile profile;
    BOOST_REQUIRE(pMgr->GetProfile("alice", profile));
    BOOST_CHECK_EQUAL(profile.lastActivityTime, created);
}

// Current behaviour: between Interrupt() and the moment the node takes the
// manager away from the chainstate, blocks are still handed to it, and it
// applies and undoes their records whether or not it is running.
BOOST_AUTO_TEST_CASE(interrupted_manager_still_applies_and_undoes_blocks)
{
    uint256 auctionId = NewAuction("erin");
    DepositBid bid = MakeDepositBid(auctionId, alice, 6000 * COIN);

    reddidManager->Interrupt();
    Stake({bid.tx});

    BOOST_CHECK(aMgr->IsDepositLocked(bid.utxo));
    BOOST_CHECK_EQUAL(Bids(auctionId).size(), 1U);

    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, IndexAt(Height())));
    BOOST_CHECK(!HasLock(bid.utxo));
    BOOST_CHECK(Bids(auctionId).empty());
    SyncWithValidationInterfaceQueue();
}

// ===================================================================
// Category 2: Disconnecting blocks
// ===================================================================

BOOST_AUTO_TEST_CASE(disconnecting_the_tip_undoes_its_records)
{
    uint256 auctionId = NewAuction("erin");
    uint256 nsAuctionId;
    const CAmount nsReserve = nsMgr->CalculateMinPrice("freshns");
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("freshns", creator, nsReserve, 3, AUCTION_STANDARD, nsAuctionId));
    const std::string empty = Snapshot();

    DepositBid bid = MakeDepositBid(auctionId, alice, 6000 * COIN);
    CMutableTransaction nsBid = RecordTxFrom(bob, NamespaceBidRecord(nsAuctionId, nsReserve));
    const int64_t connectTime = GetTime();
    Stake({bid.tx, nsBid});
    const int height = Height();
    CBlockIndex* tip = IndexAt(height);
    BOOST_REQUIRE(aMgr->IsDepositLocked(bid.utxo));
    BOOST_REQUIRE_EQUAL(nsMgr->GetAuctionBids(nsAuctionId).size(), 1U);
    const std::string applied = Snapshot();
    const uint256 firstBidId = BidOf(auctionId, alice).bidId;
    BOOST_CHECK(applied != empty);

    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, tip));
    BOOST_REQUIRE_EQUAL(Height(), height - 1);

    BOOST_CHECK(Bids(auctionId).empty());
    BOOST_CHECK(!HasLock(bid.utxo));
    DepositLock lock;
    BOOST_CHECK(!db->ReadDepositLock(bid.utxo, lock));
    AuctionInfo auction = GetAuction(aMgr, auctionId);
    BOOST_CHECK_EQUAL(auction.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(auction.currentBid, 0);
    BOOST_CHECK_EQUAL(auction.totalDepositsLocked, 0);
    BOOST_CHECK(nsMgr->GetAuctionBids(nsAuctionId).empty());
    BOOST_CHECK(Snapshot() == empty);

    // The block is connected again, with the clock where it was the first
    // time: the records are applied again, at the same height
    const int64_t later = GetTime();
    SetMockTime(connectTime);
    {
        LOCK(cs_main);
        chainstate.ResetBlockFailureFlags(tip);
    }
    BOOST_REQUIRE(chainstate.ActivateBestChain(state));
    BOOST_REQUIRE_EQUAL(Height(), height);
    BOOST_CHECK_EQUAL(GetLock(aMgr, bid.utxo).lockHeight, height);
    BOOST_CHECK_EQUAL(Bids(auctionId).size(), 1U);
    BOOST_CHECK_EQUAL(nsMgr->GetAuctionBids(nsAuctionId).size(), 1U);
    BOOST_CHECK(BidOf(auctionId, alice).bidId == firstBidId);
    BOOST_CHECK(Snapshot() == applied);

    // SUSPECTED BUG (documented, not fixed): the time of a bid, and with it
    // the bid ID, which a settle record has to name, is the time on the clock
    // of the node when it connects the block. Connecting the same block at
    // another time gives the same bid another ID.
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, tip));
    BOOST_CHECK(Snapshot() == empty);
    SetMockTime(later);
    {
        LOCK(cs_main);
        chainstate.ResetBlockFailureFlags(tip);
    }
    BOOST_REQUIRE(chainstate.ActivateBestChain(state));
    BOOST_REQUIRE_EQUAL(Height(), height);
    BOOST_CHECK(BidOf(auctionId, alice).bidId != firstBidId);
    BOOST_CHECK(Snapshot() != applied);
    SyncWithValidationInterfaceQueue();
}

// Two branches from one block. The old branch is disconnected and the new one
// connected inside one call, with cs_main held throughout: the records of the
// old branch are undone, last block first, before those of the new branch are
// applied, first block first.
BOOST_AUTO_TEST_CASE(reorg_leaves_the_state_of_the_new_branch)
{
    uint256 auctionX = NewAuction("erin");
    uint256 auctionY = NewAuction("frank");
    const int fork = Height();

    // Branch A: alice bids 6000 on X and bob bids 5000 on Y, then bob bids 7000 on X
    DepositBid aliceBid = MakeDepositBid(auctionX, alice, 6000 * COIN);
    CMutableTransaction shared = RecordTxFrom(bob, CreateUserIDBidOpReturn(auctionY, 5000 * COIN));
    DepositBid bobBid = MakeDepositBid(auctionX, bob, 7000 * COIN);
    Stake({aliceBid.tx, shared});
    Stake({bobBid.tx});
    CBlockIndex* a1 = IndexAt(fork + 1);
    CBlockIndex* a2 = IndexAt(fork + 2);
    BOOST_REQUIRE_EQUAL(Bids(auctionX).size(), 2U);
    const std::string stateA = Snapshot();

    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, a1));
    BOOST_REQUIRE_EQUAL(Height(), fork);
    m_node.mempool->clear();
    SyncWallet();
    BOOST_CHECK(Bids(auctionX).empty());
    BOOST_CHECK(Bids(auctionY).empty());

    // Branch B: carol bids 6500 on X and bob bids 5000 on Y with the same
    // transaction as in branch A, then an empty block
    DepositBid carolBid = MakeDepositBid(auctionX, carol, 6500 * COIN);
    Stake({carolBid.tx, shared});
    Stake();
    CBlockIndex* b1 = IndexAt(fork + 1);
    BOOST_REQUIRE(b1 != a1);
    BOOST_REQUIRE_EQUAL(Height(), fork + 2);
    BOOST_REQUIRE_EQUAL(Bids(auctionX).size(), 1U);
    BOOST_CHECK(Bids(auctionX)[0].bidder == carol);
    BOOST_CHECK(aMgr->IsDepositLocked(carolBid.utxo));
    BOOST_CHECK(!HasLock(aliceBid.utxo));
    BOOST_REQUIRE_EQUAL(Bids(auctionY).size(), 1U);

    // Branch A is valid again, and preferred over branch B, which has the same work
    {
        LOCK(cs_main);
        chainstate.ResetBlockFailureFlags(a1);
    }
    BOOST_REQUIRE_EQUAL(Height(), fork + 2);
    BOOST_REQUIRE(IndexAt(fork + 1) == b1);
    BOOST_REQUIRE(chainstate.PreciousBlock(state, a2));
    BOOST_REQUIRE(IndexAt(fork + 1) == a1);
    BOOST_REQUIRE(IndexAt(fork + 2) == a2);
    SyncWithValidationInterfaceQueue();

    // Exactly the state of branch A. Had carol's bid of 6500 still been there
    // when A was applied, alice's 6000 would have been refused; had the second
    // block of A been applied before the first, alice's 6000 would have been
    // refused after bob's 7000.
    std::vector<BidInfo> bids = Bids(auctionX);
    BOOST_REQUIRE_EQUAL(bids.size(), 2U);
    BOOST_CHECK(bids[0].bidder == alice);
    BOOST_CHECK_EQUAL(bids[0].bidAmount, 6000 * COIN);
    BOOST_CHECK(bids[1].bidder == bob);
    BOOST_CHECK_EQUAL(bids[1].bidAmount, 7000 * COIN);
    AuctionInfo auction = GetAuction(aMgr, auctionX);
    BOOST_CHECK_EQUAL(auction.currentBid, 7000 * COIN);
    BOOST_CHECK(auction.currentBidder == bob);
    BOOST_CHECK_EQUAL(auction.totalDepositsLocked, 900 * COIN + 1050 * COIN);
    BOOST_CHECK_EQUAL(GetLock(aMgr, aliceBid.utxo).lockHeight, fork + 1);
    BOOST_CHECK_EQUAL(GetLock(aMgr, bobBid.utxo).lockHeight, fork + 2);
    BOOST_CHECK(!HasLock(carolBid.utxo));
    DepositLock lock;
    BOOST_CHECK(!db->ReadDepositLock(carolBid.utxo, lock));

    // The record that is in both branches is there, once
    std::vector<BidInfo> bidsY = Bids(auctionY);
    BOOST_REQUIRE_EQUAL(bidsY.size(), 1U);
    BOOST_CHECK(bidsY[0].bidder == bob);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionY).currentBid, 5000 * COIN);

    // The clock moved on between the first and the second time that branch A
    // was connected, so the bid times and bid IDs differ (see
    // disconnecting_the_tip_undoes_its_records); nothing else does
    BOOST_CHECK(Snapshot() != stateA);
}

// ===================================================================
// Category 3: Verifying the database
// ===================================================================

// CVerifyDB disconnects the last blocks in memory at check level 3 and
// connects them again at level 4, as the verifychain RPC does on a running
// node. It calls DisconnectBlock and ConnectBlock directly, which do not
// touch ReddID state.
BOOST_AUTO_TEST_CASE(verifying_the_database_changes_no_reddid_state)
{
    uint256 auctionId = NewAuction("erin");
    BOOST_REQUIRE(pMgr->CreateProfile(TestProfile("alice", alice)));
    DepositBid aliceBid = MakeDepositBid(auctionId, alice, 6000 * COIN);
    CMutableTransaction activity = RecordTxFrom(carol, CreateProfileUpdateOpReturn("alice", uint256()));
    Stake({aliceBid.tx, activity});
    DepositBid bobBid = MakeDepositBid(auctionId, bob, 7000 * COIN);
    Stake({bobBid.tx});
    Stake();
    BOOST_REQUIRE_EQUAL(Bids(auctionId).size(), 2U);
    const std::string before = Snapshot({"alice"});
    const uint256 tip = TipHash();
    // The clock moves, so that a record applied again would leave another time behind
    SetMockTime(GetTime() + 1000);

    for (int level : {3, 4}) {
        LogCounter processed("Processing transaction ");
        LogCounter profileSeen("Received ReddID profile update transaction");
        std::string summary;
        DebugLogHelper done("No coin database inconsistencies in last 6 blocks", [&summary](const std::string* line) {
            if (line) summary = *line;
            return true;
        });
        {
            LOCK(cs_main);
            BOOST_CHECK(CVerifyDB().VerifyDB(chainstate, Params(), chainstate.CoinsTip(), level, 6));
        }
        // The blocks were disconnected: their transactions were counted
        BOOST_CHECK_MESSAGE(summary.find("(0 transactions)") == std::string::npos, summary);
        BOOST_CHECK_EQUAL(processed.count, 0);
        BOOST_CHECK_EQUAL(profileSeen.count, 0);
        BOOST_CHECK(Snapshot({"alice"}) == before);
        BOOST_CHECK(TipHash() == tip);
    }
    BOOST_CHECK(aMgr->IsDepositLocked(aliceBid.utxo));
    BOOST_CHECK(aMgr->IsDepositLocked(bobBid.utxo));
    BOOST_CHECK_EQUAL(Bids(auctionId).size(), 2U);
}

// ===================================================================
// Category 4: The mempool
// ===================================================================

BOOST_AUTO_TEST_CASE(mempool_rejects_a_spend_of_a_locked_deposit)
{
    uint256 auctionId = NewAuction("erin");
    DepositBid bid = MakeDepositBid(auctionId, alice, 6000 * COIN);
    Stake({bid.tx});
    BOOST_REQUIRE(aMgr->IsDepositLocked(bid.utxo));

    CMutableTransaction spend = SpendDeposit(bid);
    BOOST_CHECK(!ToMempool(spend));
    BOOST_CHECK_EQUAL(m_reject_reason, "reddid-deposit-locked");
    BOOST_CHECK(m_reject_result == TxValidationResult::TX_MEMPOOL_POLICY);
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);

    // The change of the same transaction is not locked
    CMutableTransaction change = Build({COutPoint(bid.tx.GetHash(), 2)}, {CTxOut(COIN_VALUE - 2 * FEE - bid.deposit, P2PKH(alice))});
    BOOST_CHECK(ToMempool(change));

    // A transaction that spends the deposit next to another input is rejected as well
    CMutableTransaction mixed = Build({TakeCoin(alice), bid.utxo}, {CTxOut(COIN_VALUE, P2PKH(alice))});
    BOOST_CHECK(!ToMempool(mixed));
    BOOST_CHECK_EQUAL(m_reject_reason, "reddid-deposit-locked");

    // Without a manager attached to the chainstate there is no check
    WITH_LOCK(cs_main, chainstate.SetReddIDManager(nullptr));
    BOOST_CHECK(ToMempool(spend));
    WITH_LOCK(cs_main, chainstate.SetReddIDManager(reddidManager.get()));
}

// Nothing is applied from the mempool: the deposit of a bid that is not
// confirmed is not locked, and there is no bid.
//
// Current behaviour: the deposit can therefore be spent before the bid
// confirms. Both transactions are mined in one block, and the block leaves an
// active lock on an output that it has spent.
BOOST_AUTO_TEST_CASE(deposit_is_locked_at_confirmation_and_not_in_the_mempool)
{
    uint256 auctionId = NewAuction("erin");
    DepositBid bid = MakeDepositBid(auctionId, alice, 6000 * COIN);

    BOOST_REQUIRE(ToMempool(bid.tx));
    BOOST_CHECK(!HasLock(bid.utxo));
    BOOST_CHECK(Bids(auctionId).empty());
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_PENDING);

    CMutableTransaction spend = SpendDeposit(bid, CScript(), {bid.tx});
    BOOST_CHECK(ToMempool(spend));
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 2U);

    CBlock block = Stake();
    BOOST_REQUIRE_EQUAL(block.vtx.size(), 4U);
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);

    BOOST_CHECK(aMgr->IsDepositLocked(bid.utxo));
    BOOST_CHECK_EQUAL(GetLock(aMgr, bid.utxo).lockHeight, Height());
    BOOST_CHECK(!HaveCoin(bid.utxo));
    BOOST_CHECK_EQUAL(Bids(auctionId).size(), 1U);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).totalDepositsLocked, 900 * COIN);
}

BOOST_AUTO_TEST_CASE(mempool_accepts_a_spend_of_a_released_deposit)
{
    Settlement s = StartSettlement("erin");

    // The deposit of the bidder who lost was released when the settlement began
    DepositLock released = GetLock(aMgr, s.loser.utxo);
    BOOST_CHECK_EQUAL(released.state, DEPOSIT_RELEASED_LOST);
    // Current behaviour: the release height is that of the block before the finalize record
    BOOST_CHECK_EQUAL(released.releaseHeight, s.finalizeHeight - 1);
    BOOST_CHECK(ToMempool(SpendDeposit(s.loser)));

    // The deposit of the winner stays locked
    BOOST_CHECK(aMgr->IsDepositLocked(s.winner.utxo));
    BOOST_CHECK(!ToMempool(SpendDeposit(s.winner)));
    BOOST_CHECK_EQUAL(m_reject_reason, "reddid-deposit-locked");

    // until the winner spends it with a settle record for the auction
    CMutableTransaction settle = SpendDeposit(s.winner, CreateAuctionSettleOpReturn(s.auctionId, s.winningBidId));
    BOOST_CHECK(ToMempool(settle));
    BOOST_CHECK(aMgr->IsDepositLocked(s.winner.utxo));

    Stake();
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, s.auctionId).state, AUCTION_COMPLETED);
    BOOST_CHECK_EQUAL(GetLock(aMgr, s.winner.utxo).state, DEPOSIT_CONSUMED_WON);
    UserIDInfo userId;
    BOOST_REQUIRE(db->ReadUserID("erin", NS, userId));
    BOOST_CHECK(userId.owner == bob);
}

// SUSPECTED BUG (documented, not fixed): for an active lock the mempool check
// asks only whether the transaction carries a settle record that names the
// auction of the lock. It does not ask whether the auction is being settled,
// whether the deposit is that of the winner, which bid the record names or
// where the money goes. Any bidder can take a deposit back at any time by
// adding such a record. The record is then rejected when the block is
// connected, and the lock stays active on an output that is spent.
BOOST_AUTO_TEST_CASE(mempool_accepts_a_spend_of_a_locked_deposit_with_any_settle_record)
{
    uint256 auctionId = NewAuction("erin");
    uint256 otherAuction = NewAuction("frank");
    DepositBid bid = MakeDepositBid(auctionId, alice, 6000 * COIN);
    Stake({bid.tx});
    BOOST_REQUIRE(aMgr->IsDepositLocked(bid.utxo));
    BOOST_REQUIRE_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_ACTIVE);

    // A settle record for another auction does not help
    BOOST_CHECK(!ToMempool(SpendDeposit(bid, CreateAuctionSettleOpReturn(otherAuction, uint256S("01")))));
    BOOST_CHECK_EQUAL(m_reject_reason, "reddid-deposit-locked");

    // One for the auction of the lock does, with a bid ID that does not exist,
    // while the auction is still running
    CMutableTransaction spend = SpendDeposit(bid, CreateAuctionSettleOpReturn(auctionId, uint256S("01")));
    BOOST_CHECK(ToMempool(spend));

    Stake();
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);
    BOOST_CHECK(!HaveCoin(bid.utxo));
    BOOST_CHECK(aMgr->IsDepositLocked(bid.utxo));
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).currentBid, 6000 * COIN);
    BOOST_CHECK_EQUAL(Bids(auctionId).size(), 1U);
}

// Current behaviour: the lock is a rule of the mempool only. A block may
// spend a locked deposit; the lock record is not changed by that and names an
// output that no longer exists, and the bid still stands.
BOOST_AUTO_TEST_CASE(block_may_spend_a_locked_deposit)
{
    uint256 auctionId = NewAuction("erin");
    DepositBid bid = MakeDepositBid(auctionId, alice, 6000 * COIN);
    Stake({bid.tx});
    BOOST_REQUIRE(aMgr->IsDepositLocked(bid.utxo));
    const std::string before = Snapshot();

    CMutableTransaction spend = SpendDeposit(bid);
    BOOST_REQUIRE(!ToMempool(spend));
    const int height = Height();
    Stake({spend});

    BOOST_CHECK_EQUAL(Height(), height + 1);
    BOOST_CHECK(!HaveCoin(bid.utxo));
    BOOST_CHECK(HaveCoin(COutPoint(spend.GetHash(), 0)));
    DepositLock lock = GetLock(aMgr, bid.utxo);
    BOOST_CHECK_EQUAL(lock.state, DEPOSIT_LOCKED_ACTIVE);
    BOOST_CHECK_EQUAL(lock.lockHeight, height);
    BOOST_CHECK_EQUAL(lock.releaseHeight, 0);
    BOOST_CHECK(Snapshot() == before);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, auctionId).currentBid, 6000 * COIN);
    BOOST_CHECK(GetAuction(aMgr, auctionId).currentBidder == alice);
}

// ===================================================================
// Category 5: The settlement deadline
// ===================================================================

// Current behaviour: while a block is being connected the tip of the chain is
// still the block before it, and that is the height the auction manager
// reads. For a finalize record in block H and a settlement window of W blocks
// the deadline is H - 1 + W. A settle record is accepted in the blocks up to
// H + W and a default record from block H + W + 1.
BOOST_AUTO_TEST_CASE(settle_record_is_accepted_up_to_the_block_after_the_deadline)
{
    Settlement s = StartSettlement("erin");
    const int H = s.finalizeHeight;
    AuctionInfo auction = GetAuction(aMgr, s.auctionId);
    BOOST_CHECK_EQUAL(auction.settlementDeadlineHeight, H + 1);
    BOOST_CHECK(BidOf(s.auctionId, bob).isWinner);

    Stake();
    BOOST_REQUIRE_EQUAL(Height(), H + 1);

    // Block H + 2: a default record is too early, the settle record is in time
    CMutableTransaction tooEarly = RecordTxFrom(creator, RecordScript(OP_AUCTION_DEFAULT, s.auctionId, s.winningBidId));
    CMutableTransaction settle = SpendDeposit(s.winner, CreateAuctionSettleOpReturn(s.auctionId, s.winningBidId));
    {
        ASSERT_DEBUG_LOG("Error: AuctionManager::ProcessDefault: Not past settlement deadline yet");
        Stake({tooEarly, settle});
    }
    BOOST_REQUIRE_EQUAL(Height(), H + 2);

    auction = GetAuction(aMgr, s.auctionId);
    BOOST_CHECK_EQUAL(auction.state, AUCTION_COMPLETED);
    BOOST_CHECK(auction.settleTxid == settle.GetHash());
    DepositLock lock = GetLock(aMgr, s.winner.utxo);
    BOOST_CHECK_EQUAL(lock.state, DEPOSIT_CONSUMED_WON);
    BOOST_CHECK_EQUAL(lock.releaseHeight, H + 1);
    UserIDInfo userId;
    BOOST_REQUIRE(db->ReadUserID("erin", NS, userId));
    BOOST_CHECK(userId.owner == bob);
}

BOOST_AUTO_TEST_CASE(default_record_is_accepted_two_blocks_after_the_deadline)
{
    Settlement s = StartSettlement("erin");
    const int H = s.finalizeHeight;
    BOOST_CHECK_EQUAL(GetAuction(aMgr, s.auctionId).settlementDeadlineHeight, H + 1);

    Stake();
    Stake();
    BOOST_REQUIRE_EQUAL(Height(), H + 2);
    BOOST_CHECK_EQUAL(GetAuction(aMgr, s.auctionId).state, AUCTION_SETTLEMENT);

    // Block H + 3: the settle record is too late, the default record is accepted
    CMutableTransaction tooLate = SpendDeposit(s.winner, CreateAuctionSettleOpReturn(s.auctionId, s.winningBidId));
    CMutableTransaction defaulted = RecordTxFrom(creator, RecordScript(OP_AUCTION_DEFAULT, s.auctionId, s.winningBidId));
    const int64_t connectTime = GetTime();
    {
        ASSERT_DEBUG_LOG("Error: AuctionManager::CompleteSettlement: Past settlement deadline");
        Stake({tooLate, defaulted});
    }
    BOOST_REQUIRE_EQUAL(Height(), H + 3);

    AuctionInfo auction = GetAuction(aMgr, s.auctionId);
    BOOST_CHECK_EQUAL(auction.state, AUCTION_DEFAULTED);
    DepositLock lock = GetLock(aMgr, s.winner.utxo);
    BOOST_CHECK_EQUAL(lock.state, DEPOSIT_FORFEITED);
    BOOST_CHECK_EQUAL(lock.releaseHeight, H + 2);
    // The deposit that was forfeited was spent by the settle record that came too late
    BOOST_CHECK(!HaveCoin(s.winner.utxo));
    UserIDInfo userId;
    BOOST_CHECK(!db->ReadUserID("erin", NS, userId));

    // The name is auctioned again
    AuctionInfo again = GetAuction(aMgr, ExpectedAuctionId("erin", NS, connectTime));
    BOOST_CHECK(again.previousAuctionId == s.auctionId);
    BOOST_CHECK_EQUAL(again.reAuctionCount, 1);
    BOOST_CHECK_EQUAL(again.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(again.blockHeight, H + 2);
}

// SUSPECTED BUG (documented, not fixed): no community fund address is
// configured by default, and without one the mempool accepts any spend of a
// forfeited deposit: the bidder who defaulted takes the deposit back. With an
// address configured the check asks for a forfeit record and nothing else; it
// does not look at where the money goes.
BOOST_AUTO_TEST_CASE(mempool_accepts_a_spend_of_a_forfeited_deposit)
{
    Settlement s = StartSettlement("erin");
    Stake();
    Stake();
    Stake({RecordTxFrom(creator, RecordScript(OP_AUCTION_DEFAULT, s.auctionId, s.winningBidId))});
    BOOST_REQUIRE_EQUAL(GetLock(aMgr, s.winner.utxo).state, DEPOSIT_FORFEITED);
    BOOST_REQUIRE(HaveCoin(s.winner.utxo));

    AuctionConfig config;
    BOOST_REQUIRE(aMgr->GetAuctionConfig("userid", config));
    BOOST_CHECK(config.communityFundAddress.empty());

    // With an address configured: a plain spend is rejected, one with a
    // forfeit record that pays the bidder himself is accepted
    config.communityFundAddress = "fund";
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));
    CMutableTransaction plain = SpendDeposit(s.winner);
    BOOST_CHECK(!ToMempool(plain));
    BOOST_CHECK_EQUAL(m_reject_reason, "reddid-deposit-locked");

    // Without an address the plain spend is accepted
    config.communityFundAddress.clear();
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));
    BOOST_CHECK(ToMempool(plain));
    m_node.mempool->clear();

    config.communityFundAddress = "fund";
    BOOST_REQUIRE(aMgr->SetAuctionConfig(config));
    CMutableTransaction withRecord = SpendDeposit(s.winner, RecordScript(OP_DEPOSIT_FORFEIT, s.auctionId, s.winningBidId));
    BOOST_CHECK(ToMempool(withRecord));
}

// A block is applied with cs_main and the lock of the mempool held, and the
// auction manager announces a bid that it places. The announcement takes the
// lock of the peer list and the send lock of each peer under those two.
BOOST_AUTO_TEST_CASE(applying_a_block_announces_to_subscribed_peers)
{
    ReddIDP2PManager* p2p = reddidManager->GetP2PManager();
    BOOST_REQUIRE(p2p != nullptr);
    BOOST_CHECK(aMgr->GetP2P() == p2p);
    ConnmanTestMsg& connman = static_cast<ConnmanTestMsg&>(*m_node.connman);
    uint256 auctionId = NewAuction("erin");

    // The connection manager of the fixture does not advertise NODE_REDDID,
    // as that of a node without -reddid: nothing is relayed
    {
        LogCounter refused("Not relaying ReddID messages - service not enabled");
        DepositBid bid = MakeDepositBid(auctionId, alice, 6000 * COIN);
        Stake({bid.tx});
        BOOST_CHECK(aMgr->IsDepositLocked(bid.utxo));
        BOOST_CHECK_EQUAL(refused.count, 1);
    }

    // It advertises the service and has no peers: the bid goes to no one
    SetLocalServices(connman, m_node.peerman.get(), REDDID_PEER_SERVICES);
    {
        LogCounter relayed("Relayed uidaucbid message to 0 nodes");
        DepositBid bid = MakeDepositBid(auctionId, bob, 7000 * COIN);
        Stake({bid.tx});
        BOOST_CHECK(aMgr->IsDepositLocked(bid.utxo));
        BOOST_CHECK_EQUAL(relayed.count, 1);
    }

    // With a subscribed peer and one that is not subscribed
    CNode* subscribed = NewTestPeer(0);
    CNode* stranger = NewTestPeer(1);
    connman.AddTestNode(*subscribed);
    connman.AddTestNode(*stranger);
    p2p->SubscribeNode(subscribed->GetId());
    {
        LogCounter relayed("Relayed uidaucbid message to 1 nodes");
        DepositBid bid = MakeDepositBid(auctionId, carol, 8000 * COIN);
        Stake({bid.tx});
        BOOST_CHECK(aMgr->IsDepositLocked(bid.utxo));
        BOOST_CHECK_EQUAL(relayed.count, 1);

        const std::vector<SentMessage> sent = SentMessages(*subscribed);
        BOOST_REQUIRE_EQUAL(sent.size(), 1U);
        BOOST_CHECK_EQUAL(sent[0].command, "uidaucbid");
        const CAuctionBid msg = Decode<CAuctionBid>(sent[0]);
        BOOST_CHECK(msg.auctionId == auctionId);
        BOOST_CHECK(msg.bidder == carol);
        BOOST_CHECK_EQUAL(msg.bidAmount, 8000 * COIN);
        BOOST_CHECK(msg.bidId == BidOf(auctionId, carol).bidId);
        BOOST_CHECK(SentMessages(*stranger).empty());
    }

    // Disconnecting the block announces nothing: a peer that applied the bid
    // on the strength of the message is not told that it is gone
    ClearSent(*subscribed);
    const int height = Height();
    BlockValidationState state;
    BOOST_REQUIRE(chainstate.InvalidateBlock(state, IndexAt(height)));
    BOOST_REQUIRE_EQUAL(Height(), height - 1);
    BOOST_CHECK_EQUAL(Bids(auctionId).size(), 2U);
    BOOST_CHECK(SentMessages(*subscribed).empty());

    connman.ClearTestNodes();
}

BOOST_AUTO_TEST_SUITE_END()
