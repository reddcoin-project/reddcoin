// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <id/reddid.h>
#include <id/namespace.h>
#include <id/reddid_db.h>
#include <id/transaction_handler.h>
#include <crypto/common.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
#include <test/reddid_test_fixture.h>
#include <test/util/setup_common.h>
#include <util/time.h>
#include <key.h>

#include <string>
#include <vector>

static const int64_t DAY = 24 * 60 * 60;
// A fixed clock for the cases that depend on expiration and auction end times
static const int64_t TEST_TIME = 1700000000;

// Helper: the record the namespace manager holds in memory, bypassing its lookup cache
static bool StoredNamespace(const NamespaceManager* nsMgr, const std::string& nsId, NamespaceInfo& result)
{
    for (const NamespaceInfo& ns : nsMgr->GetNamespaces()) {
        if (ns.id == nsId) {
            result = ns;
            return true;
        }
    }
    return false;
}

// Helper: Build an OP_RETURN script with ReddID header + payload
static CScript MakeNamespaceScript(unsigned char opCode, const std::vector<unsigned char>& payload)
{
    std::vector<unsigned char> data;
    data.push_back('R');
    data.push_back(opCode);
    data.insert(data.end(), payload.begin(), payload.end());
    CScript script;
    script << OP_RETURN << data;
    return script;
}

// Helper: Build a namespace bid record: auctionId(32) + amount(8)
static CScript MakeNamespaceBidScript(const uint256& auctionId, CAmount amount)
{
    std::vector<unsigned char> payload(auctionId.begin(), auctionId.end());
    payload.resize(40);
    WriteLE64(payload.data() + 32, amount);
    return MakeNamespaceScript(OP_NAMESPACE_AUCTION_BID, payload);
}

// Helper: Build a transaction carrying a ReddID record. When sender is given,
// the first output pays senderValue to its public key hash, which is where the
// namespace decoders take the acting key from.
static CTransaction MakeNamespaceTx(const CKeyID* sender, const CScript& record, CAmount senderValue = 0)
{
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    if (sender) {
        mtx.vout.emplace_back(senderValue, GetScriptForDestination(PKHash(*sender)));
    }
    mtx.vout.emplace_back(0, record);
    return CTransaction(mtx);
}

BOOST_FIXTURE_TEST_SUITE(reddid_namespace_tests, ReddIDTestingSetup)

// ===================================================================
// Category 1: Namespace creation
// ===================================================================

BOOST_AUTO_TEST_CASE(create_namespace_valid_config)
{
    CKeyID owner = MakeKeyID();
    CreateTestNamespace("testns", owner);
    NamespaceInfo result;
    BOOST_CHECK(reddidManager->GetNamespaceManager()->GetNamespaceInfo("testns", result));
    BOOST_CHECK_EQUAL(result.id, "testns");
    BOOST_CHECK(result.owner == owner);
}

BOOST_AUTO_TEST_CASE(create_duplicate_namespace_fails)
{
    CKeyID owner = MakeKeyID();
    CreateTestNamespace("dupens", owner);
    NamespaceInfo ns;
    ns.id = "dupens";
    ns.owner = MakeKeyID();
    ns.minLength = 3; ns.maxLength = 32; ns.renewalPeriod = 365; ns.gracePeriod = 30;
    ns.namespaceRevenuePct = 10; ns.burnPct = 70; ns.nodePct = 15; ns.devPct = 5;
    ns.minAuctionDuration = 3; ns.maxAuctionDuration = 7; ns.minBidIncrement = 5.0;
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->CreateNamespace(ns));
}

BOOST_AUTO_TEST_CASE(create_namespace_invalid_id)
{
    NamespaceInfo ns;
    ns.id = "BadName";
    ns.owner = MakeKeyID();
    ns.minLength = 3; ns.maxLength = 32; ns.renewalPeriod = 365; ns.gracePeriod = 30;
    ns.namespaceRevenuePct = 10; ns.burnPct = 70; ns.nodePct = 15; ns.devPct = 5;
    ns.minAuctionDuration = 3; ns.maxAuctionDuration = 7; ns.minBidIncrement = 5.0;
    BOOST_CHECK(!reddidManager->GetNamespaceManager()->CreateNamespace(ns));
}

// ===================================================================
// Category 2: Namespace auction lifecycle
// ===================================================================

BOOST_AUTO_TEST_CASE(create_namespace_auction_returns_id)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CAmount minPrice = nsMgr->CalculateMinPrice("premium");
    uint256 auctionId;
    BOOST_CHECK(nsMgr->CreateNamespaceAuction("premium", creator, minPrice, 3, AUCTION_STANDARD, auctionId));
    BOOST_CHECK(!auctionId.IsNull());

    // New auctions start in PENDING state; verify via GetAuctionInfo
    AuctionInfo aInfo;
    BOOST_CHECK(nsMgr->GetAuctionInfo(auctionId, aInfo));
    BOOST_CHECK_EQUAL(aInfo.state, AUCTION_PENDING);
}

BOOST_AUTO_TEST_CASE(create_auction_below_min_price_fails)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CAmount minPrice = nsMgr->CalculateMinPrice("lowball");
    uint256 auctionId;
    BOOST_CHECK(!nsMgr->CreateNamespaceAuction("lowball", creator, minPrice - 1, 3, AUCTION_STANDARD, auctionId));
}

BOOST_AUTO_TEST_CASE(bid_on_namespace_auction_stored)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("bidtest");
    uint256 auctionId;
    nsMgr->CreateNamespaceAuction("bidtest", creator, reserve, 3, AUCTION_STANDARD, auctionId);

    CKeyID bidder = MakeKeyID();
    uint256 bidId;
    BOOST_CHECK(nsMgr->BidOnNamespaceAuction(auctionId, bidder, reserve, bidId));
    BOOST_CHECK(!bidId.IsNull());

    std::vector<BidInfo> bids = nsMgr->GetAuctionBids(auctionId);
    BOOST_CHECK_EQUAL(bids.size(), 1u);
}

BOOST_AUTO_TEST_CASE(bid_below_reserve_fails)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("reserv");
    uint256 auctionId;
    nsMgr->CreateNamespaceAuction("reserv", creator, reserve, 3, AUCTION_STANDARD, auctionId);

    CKeyID bidder = MakeKeyID();
    uint256 bidId;
    BOOST_CHECK(!nsMgr->BidOnNamespaceAuction(auctionId, bidder, reserve - 1, bidId));
}

BOOST_AUTO_TEST_CASE(finalize_auction_creates_namespace)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("final");
    uint256 auctionId;
    nsMgr->CreateNamespaceAuction("final", creator, reserve, 3, AUCTION_STANDARD, auctionId);

    CKeyID winner = MakeKeyID();
    uint256 bidId;
    nsMgr->BidOnNamespaceAuction(auctionId, winner, reserve, bidId);

    // Advance past endTime
    SetMockTime(GetTime() + 3 * 86400 + 60);
    BOOST_CHECK(nsMgr->FinalizeNamespaceAuction(auctionId));

    NamespaceInfo info;
    BOOST_CHECK(nsMgr->GetNamespaceInfo("final", info));
    BOOST_CHECK(info.owner == winner);
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(finalized_auction_not_in_active_list)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("gone");
    uint256 auctionId;
    nsMgr->CreateNamespaceAuction("gone", creator, reserve, 3, AUCTION_STANDARD, auctionId);
    CKeyID bidder = MakeKeyID();
    uint256 bidId;
    nsMgr->BidOnNamespaceAuction(auctionId, bidder, reserve, bidId);
    SetMockTime(GetTime() + 3 * 86400 + 60);
    nsMgr->FinalizeNamespaceAuction(auctionId);

    std::vector<AuctionInfo> active = nsMgr->GetActiveAuctions();
    bool found = false;
    for (const auto& a : active) {
        if (a.auctionId == auctionId) { found = true; break; }
    }
    BOOST_CHECK(!found);
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(cancel_auction_no_bids_succeeds)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("cancel");
    uint256 auctionId;
    nsMgr->CreateNamespaceAuction("cancel", creator, reserve, 3, AUCTION_STANDARD, auctionId);
    BOOST_CHECK(nsMgr->CancelNamespaceAuction(auctionId, creator));
}

BOOST_AUTO_TEST_CASE(cancel_by_non_creator_fails)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CKeyID imposter = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("nocancel");
    uint256 auctionId;
    nsMgr->CreateNamespaceAuction("nocancel", creator, reserve, 3, AUCTION_STANDARD, auctionId);
    BOOST_CHECK(!nsMgr->CancelNamespaceAuction(auctionId, imposter));
}

// ===================================================================
// Category 3: ConfigHash
// ===================================================================

BOOST_AUTO_TEST_CASE(config_hash_deterministic)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    NamespaceInfo ns;
    ns.id = "hashtest";
    ns.owner = MakeKeyID();
    uint256 hash1 = nsMgr->CalculateConfigHash(ns);
    uint256 hash2 = nsMgr->CalculateConfigHash(ns);
    BOOST_CHECK(hash1 == hash2);
    BOOST_CHECK(!hash1.IsNull());
}

BOOST_AUTO_TEST_CASE(config_hash_differs_on_field_change)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    NamespaceInfo ns;
    ns.id = "hashcmp";
    ns.owner = MakeKeyID();
    uint256 hashOrig = nsMgr->CalculateConfigHash(ns);
    ns.minLength = 5;
    uint256 hashChanged = nsMgr->CalculateConfigHash(ns);
    BOOST_CHECK(hashOrig != hashChanged);
}

BOOST_AUTO_TEST_CASE(config_hash_ignores_confighash_and_lastupdated)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    NamespaceInfo ns;
    ns.id = "hashzero";
    ns.owner = MakeKeyID();
    uint256 hashBase = nsMgr->CalculateConfigHash(ns);
    ns.configHash = uint256S("abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
    ns.lastUpdated = 9999999;
    uint256 hashModified = nsMgr->CalculateConfigHash(ns);
    BOOST_CHECK(hashBase == hashModified);
}

// ===================================================================
// Category 4: Pricing and queries
// ===================================================================

BOOST_AUTO_TEST_CASE(min_price_one_char)
{
    BOOST_CHECK_EQUAL(reddidManager->GetNamespaceManager()->CalculateMinPrice("a"), 100000 * COIN);
}

BOOST_AUTO_TEST_CASE(min_price_two_chars)
{
    BOOST_CHECK_EQUAL(reddidManager->GetNamespaceManager()->CalculateMinPrice("ab"), 50000 * COIN);
}

BOOST_AUTO_TEST_CASE(min_price_three_chars)
{
    BOOST_CHECK_EQUAL(reddidManager->GetNamespaceManager()->CalculateMinPrice("abc"), 25000 * COIN);
}

BOOST_AUTO_TEST_CASE(min_price_four_plus_chars)
{
    BOOST_CHECK_EQUAL(reddidManager->GetNamespaceManager()->CalculateMinPrice("abcd"), 10000 * COIN);
    BOOST_CHECK_EQUAL(reddidManager->GetNamespaceManager()->CalculateMinPrice("abcdefgh"), 10000 * COIN);
}

BOOST_AUTO_TEST_CASE(is_namespace_available_before_and_after)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("avail"));
    CKeyID owner = MakeKeyID();
    CreateTestNamespace("avail", owner);
    BOOST_CHECK(!nsMgr->IsNamespaceAvailable("avail"));
}

// ===================================================================
// Category 5: Namespace transfer
// ===================================================================

BOOST_AUTO_TEST_CASE(transfer_namespace_changes_owner)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID originalOwner = MakeKeyID();
    CKeyID newOwner = MakeKeyID();
    CreateTestNamespace("xferns", originalOwner);
    BOOST_CHECK(nsMgr->IsOwner("xferns", originalOwner));
    BOOST_CHECK(nsMgr->TransferNamespace("xferns", originalOwner, newOwner));
    BOOST_CHECK(nsMgr->IsOwner("xferns", newOwner));
    BOOST_CHECK(!nsMgr->IsOwner("xferns", originalOwner));
}

BOOST_AUTO_TEST_CASE(new_owner_can_update_namespace)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID origOwner = MakeKeyID();
    CKeyID newOwner = MakeKeyID();
    CreateTestNamespace("updns", origOwner);
    nsMgr->TransferNamespace("updns", origOwner, newOwner);

    NamespaceInfo updatedInfo;
    updatedInfo.id = "updns";
    updatedInfo.owner = newOwner;
    updatedInfo.minLength = 5; updatedInfo.maxLength = 32;
    updatedInfo.renewalPeriod = 365; updatedInfo.gracePeriod = 30;
    updatedInfo.namespaceRevenuePct = 10; updatedInfo.burnPct = 70;
    updatedInfo.nodePct = 15; updatedInfo.devPct = 5;
    updatedInfo.minAuctionDuration = 3; updatedInfo.maxAuctionDuration = 7;
    updatedInfo.minBidIncrement = 5.0;
    BOOST_CHECK(nsMgr->UpdateNamespace(updatedInfo));
}

BOOST_AUTO_TEST_CASE(old_owner_cannot_update_namespace)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID origOwner = MakeKeyID();
    CKeyID newOwner = MakeKeyID();
    CreateTestNamespace("noupd", origOwner);
    nsMgr->TransferNamespace("noupd", origOwner, newOwner);

    NamespaceInfo staleInfo;
    staleInfo.id = "noupd";
    staleInfo.owner = origOwner;
    staleInfo.minLength = 7; staleInfo.maxLength = 32;
    staleInfo.renewalPeriod = 365; staleInfo.gracePeriod = 30;
    staleInfo.namespaceRevenuePct = 10; staleInfo.burnPct = 70;
    staleInfo.nodePct = 15; staleInfo.devPct = 5;
    staleInfo.minAuctionDuration = 3; staleInfo.maxAuctionDuration = 7;
    staleInfo.minBidIncrement = 5.0;
    BOOST_CHECK(!nsMgr->UpdateNamespace(staleInfo));
}

// ===================================================================
// Category 6: Namespace renewal
// ===================================================================

BOOST_AUTO_TEST_CASE(renew_namespace_extends_expiration)
{
    SetMockTime(TEST_TIME);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID owner = MakeKeyID();
    CreateTestNamespace("renewns", owner);

    NamespaceInfo before;
    BOOST_REQUIRE(StoredNamespace(nsMgr, "renewns", before));
    BOOST_CHECK_EQUAL(before.expiration, TEST_TIME + 365 * DAY);

    SetMockTime(TEST_TIME + 10 * DAY);
    BOOST_CHECK(nsMgr->RenewNamespace("renewns", owner, 30));

    // Not yet expired: the days are added to the current expiration
    NamespaceInfo after;
    BOOST_REQUIRE(StoredNamespace(nsMgr, "renewns", after));
    BOOST_CHECK_EQUAL(after.expiration, TEST_TIME + 395 * DAY);
    BOOST_CHECK_EQUAL(after.lastUpdated, TEST_TIME + 10 * DAY);

    // The renewal reached the database as well
    NamespaceInfo stored;
    BOOST_REQUIRE(reddidManager->GetReddIDDB()->ReadNamespace("renewns", stored));
    BOOST_CHECK_EQUAL(stored.expiration, TEST_TIME + 395 * DAY);
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(renew_namespace_default_period)
{
    SetMockTime(TEST_TIME);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID owner = MakeKeyID();
    CreateTestNamespace("renewdef", owner);

    // Zero or negative days fall back to the namespace's renewal period
    BOOST_CHECK(nsMgr->RenewNamespace("renewdef", owner, 0));
    NamespaceInfo after;
    BOOST_REQUIRE(StoredNamespace(nsMgr, "renewdef", after));
    BOOST_CHECK_EQUAL(after.expiration, TEST_TIME + 2 * 365 * DAY);
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(renew_expired_namespace_counts_from_now)
{
    SetMockTime(TEST_TIME);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID owner = MakeKeyID();
    CreateTestNamespace("renewexp", owner);

    SetMockTime(TEST_TIME + 400 * DAY);
    BOOST_CHECK(nsMgr->RenewNamespace("renewexp", owner, 30));
    NamespaceInfo after;
    BOOST_REQUIRE(StoredNamespace(nsMgr, "renewexp", after));
    BOOST_CHECK_EQUAL(after.expiration, TEST_TIME + 430 * DAY);
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(renew_namespace_rejects_bad_requests)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID owner = MakeKeyID();
    CreateTestNamespace("renewbad", owner);

    BOOST_CHECK(!nsMgr->RenewNamespace("", owner, 30));
    BOOST_CHECK(!nsMgr->RenewNamespace("nosuchns", owner, 30));
    BOOST_CHECK(!nsMgr->RenewNamespace("renewbad", MakeKeyID(), 30));
}

// SUSPECTED BUG (documented, not fixed): RenewNamespace, TransferNamespace and
// UpdateNamespace write the in-memory map and the database but leave the
// lookup cache alone, and GetNamespaceInfo answers from that cache first. Once
// a namespace has been looked up, GetNamespaceInfo keeps returning the record
// as it was at that first lookup.
BOOST_AUTO_TEST_CASE(renew_namespace_leaves_stale_cache)
{
    SetMockTime(TEST_TIME);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID owner = MakeKeyID();
    CreateTestNamespace("stalens", owner);

    BOOST_CHECK(nsMgr->RenewNamespace("stalens", owner, 30));

    NamespaceInfo stored;
    BOOST_REQUIRE(StoredNamespace(nsMgr, "stalens", stored));
    BOOST_CHECK_EQUAL(stored.expiration, TEST_TIME + 395 * DAY);

    // GetNamespaceInfo still reports the expiration from before the renewal
    NamespaceInfo cached;
    BOOST_REQUIRE(nsMgr->GetNamespaceInfo("stalens", cached));
    BOOST_CHECK_EQUAL(cached.expiration, TEST_TIME + 365 * DAY);

    // RenewNamespace itself starts from the stale record, so a second renewal
    // overwrites the first instead of adding to it.
    BOOST_CHECK(nsMgr->RenewNamespace("stalens", owner, 30));
    BOOST_REQUIRE(StoredNamespace(nsMgr, "stalens", stored));
    BOOST_CHECK_EQUAL(stored.expiration, TEST_TIME + 395 * DAY);
    SetMockTime(0);
}

// SUSPECTED BUG (documented, not fixed): same stale cache as above. After a
// transfer GetNamespaceInfo still names the previous owner, so the new owner
// cannot renew, and a renewal by the previous owner is accepted and writes
// the stale record back, which undoes the transfer.
BOOST_AUTO_TEST_CASE(transfer_namespace_leaves_stale_cache)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID oldOwner = MakeKeyID();
    CKeyID newOwner = MakeKeyID();
    CreateTestNamespace("stalexfer", oldOwner);

    BOOST_CHECK(nsMgr->TransferNamespace("stalexfer", oldOwner, newOwner));
    BOOST_CHECK(nsMgr->IsOwner("stalexfer", newOwner));

    NamespaceInfo cached;
    BOOST_REQUIRE(nsMgr->GetNamespaceInfo("stalexfer", cached));
    BOOST_CHECK(cached.owner == oldOwner);

    BOOST_CHECK(!nsMgr->RenewNamespace("stalexfer", newOwner, 30));
    BOOST_CHECK(nsMgr->RenewNamespace("stalexfer", oldOwner, 30));
    BOOST_CHECK(nsMgr->IsOwner("stalexfer", oldOwner));
    BOOST_CHECK(!nsMgr->IsOwner("stalexfer", newOwner));
}

// SUSPECTED BUG (documented, not fixed): same stale cache as above, seen
// through UpdateNamespace.
BOOST_AUTO_TEST_CASE(update_namespace_leaves_stale_cache)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID owner = MakeKeyID();
    CreateTestNamespace("staleupd", owner);

    NamespaceInfo info;
    BOOST_REQUIRE(nsMgr->GetNamespaceInfo("staleupd", info));
    BOOST_CHECK_EQUAL(info.minLength, 3);

    NamespaceInfo updated = info;
    updated.minLength = 5;
    BOOST_CHECK(nsMgr->UpdateNamespace(updated));

    NamespaceInfo stored;
    BOOST_REQUIRE(StoredNamespace(nsMgr, "staleupd", stored));
    BOOST_CHECK_EQUAL(stored.minLength, 5);

    NamespaceInfo cached;
    BOOST_REQUIRE(nsMgr->GetNamespaceInfo("staleupd", cached));
    BOOST_CHECK_EQUAL(cached.minLength, 3);

    // ValidateUserID reads the namespace through GetNamespaceInfo
    BOOST_CHECK(reddidManager->ValidateUserID("abc", "staleupd"));
}

// ===================================================================
// Category 7: Pricing tiers and renewal fee
// ===================================================================

BOOST_AUTO_TEST_CASE(pricing_tier_add_update_remove)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    ReddIDDB* db = reddidManager->GetReddIDDB();
    CreateTestNamespace("tierns", MakeKeyID());
    BOOST_CHECK(nsMgr->GetPricingTiers("tierns").empty());

    BOOST_CHECK(nsMgr->AddPricingTier("tierns", 3, 7000 * COIN));
    BOOST_CHECK(nsMgr->AddPricingTier("tierns", 5, 6000 * COIN));
    std::vector<PricingTier> tiers = nsMgr->GetPricingTiers("tierns");
    BOOST_REQUIRE_EQUAL(tiers.size(), 2U);
    BOOST_CHECK_EQUAL(tiers[0].namespaceId, "tierns");
    BOOST_CHECK_EQUAL(tiers[0].minLength, 3);
    BOOST_CHECK_EQUAL(tiers[0].minPrice, 7000 * COIN);
    BOOST_CHECK_EQUAL(tiers[1].minLength, 5);
    BOOST_CHECK_EQUAL(tiers[1].minPrice, 6000 * COIN);

    // Adding a tier for an existing length replaces its price
    BOOST_CHECK(nsMgr->AddPricingTier("tierns", 3, 8000 * COIN));
    tiers = nsMgr->GetPricingTiers("tierns");
    BOOST_REQUIRE_EQUAL(tiers.size(), 2U);
    BOOST_CHECK_EQUAL(tiers[0].minPrice, 8000 * COIN);

    std::vector<PricingTier> stored;
    BOOST_CHECK(db->ReadPricingTiers("tierns", stored));
    BOOST_CHECK_EQUAL(stored.size(), 2U);

    BOOST_CHECK(nsMgr->RemovePricingTier("tierns", 3));
    tiers = nsMgr->GetPricingTiers("tierns");
    BOOST_REQUIRE_EQUAL(tiers.size(), 1U);
    BOOST_CHECK_EQUAL(tiers[0].minLength, 5);

    stored.clear();
    db->ReadPricingTiers("tierns", stored);
    BOOST_CHECK_EQUAL(stored.size(), 1U);

    // Removing it again fails: the tier is gone
    BOOST_CHECK(!nsMgr->RemovePricingTier("tierns", 3));

    BOOST_CHECK(nsMgr->RemovePricingTier("tierns", 5));
    BOOST_CHECK(nsMgr->GetPricingTiers("tierns").empty());
}

BOOST_AUTO_TEST_CASE(pricing_tier_rejects_bad_requests)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CreateTestNamespace("tierbad", MakeKeyID());

    BOOST_CHECK(!nsMgr->AddPricingTier("", 3, 7000 * COIN));
    BOOST_CHECK(!nsMgr->AddPricingTier("tierbad", 0, 7000 * COIN));
    BOOST_CHECK(!nsMgr->AddPricingTier("tierbad", -1, 7000 * COIN));
    BOOST_CHECK(!nsMgr->AddPricingTier("tierbad", 3, 0));
    BOOST_CHECK(!nsMgr->AddPricingTier("tierbad", 3, -1));
    BOOST_CHECK(!nsMgr->AddPricingTier("nosuchns", 3, 7000 * COIN));
    BOOST_CHECK(nsMgr->GetPricingTiers("tierbad").empty());

    BOOST_CHECK(!nsMgr->RemovePricingTier("", 3));
    BOOST_CHECK(!nsMgr->RemovePricingTier("tierbad", 0));
    BOOST_CHECK(!nsMgr->RemovePricingTier("nosuchns", 3));
    // The namespace exists but has no tiers
    BOOST_CHECK(!nsMgr->RemovePricingTier("tierbad", 3));
}

BOOST_AUTO_TEST_CASE(pricing_tiers_unknown_namespace_is_empty)
{
    BOOST_CHECK(reddidManager->GetNamespaceManager()->GetPricingTiers("nosuchns").empty());
}

BOOST_AUTO_TEST_CASE(finalized_auction_gets_default_pricing_tiers)
{
    SetMockTime(TEST_TIME);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CAmount reserve = nsMgr->CalculateMinPrice("deftiers");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("deftiers", MakeKeyID(), reserve, 3, AUCTION_STANDARD, auctionId));
    uint256 bidId;
    BOOST_REQUIRE(nsMgr->BidOnNamespaceAuction(auctionId, MakeKeyID(), reserve, bidId));
    SetMockTime(TEST_TIME + 3 * DAY + 60);
    BOOST_REQUIRE(nsMgr->FinalizeNamespaceAuction(auctionId));

    std::vector<PricingTier> tiers = nsMgr->GetPricingTiers("deftiers");
    BOOST_REQUIRE_EQUAL(tiers.size(), 4U);
    BOOST_CHECK_EQUAL(tiers[0].minLength, 1);
    BOOST_CHECK_EQUAL(tiers[0].minPrice, 100000 * COIN);
    BOOST_CHECK_EQUAL(tiers[1].minLength, 2);
    BOOST_CHECK_EQUAL(tiers[1].minPrice, 50000 * COIN);
    BOOST_CHECK_EQUAL(tiers[2].minLength, 3);
    BOOST_CHECK_EQUAL(tiers[2].minPrice, 25000 * COIN);
    BOOST_CHECK_EQUAL(tiers[3].minLength, 4);
    BOOST_CHECK_EQUAL(tiers[3].minPrice, 10000 * COIN);
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(renewal_fee_by_length)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    // A quarter of the minimum auction price; the namespace need not exist
    BOOST_CHECK_EQUAL(nsMgr->CalculateRenewalFee("a"), 25000 * COIN);
    BOOST_CHECK_EQUAL(nsMgr->CalculateRenewalFee("ab"), 12500 * COIN);
    BOOST_CHECK_EQUAL(nsMgr->CalculateRenewalFee("abc"), 6250 * COIN);
    // Four characters and more get a 10% discount. The fee with a discount is
    // computed in floating point: see REDDID_FLOAT_SLACK.
    BOOST_CHECK(IsFloatAmount(nsMgr->CalculateRenewalFee("abcd"), 2250 * COIN));
    BOOST_CHECK(IsFloatAmount(nsMgr->CalculateRenewalFee("abcdefgh"), 2250 * COIN));
}

BOOST_AUTO_TEST_CASE(renewal_fee_age_discount)
{
    SetMockTime(TEST_TIME);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CreateTestNamespace("oldns", MakeKeyID());
    CreateTestNamespace("old", MakeKeyID());
    BOOST_CHECK(IsFloatAmount(nsMgr->CalculateRenewalFee("oldns"), 2250 * COIN));
    BOOST_CHECK_EQUAL(nsMgr->CalculateRenewalFee("old"), 6250 * COIN);

    // The age is counted from lastUpdated: exactly 365 days is not yet "older"
    SetMockTime(TEST_TIME + 365 * DAY);
    BOOST_CHECK(IsFloatAmount(nsMgr->CalculateRenewalFee("oldns"), 2250 * COIN));

    // More than one year: a further 5%
    SetMockTime(TEST_TIME + 366 * DAY);
    BOOST_CHECK(IsFloatAmount(nsMgr->CalculateRenewalFee("oldns"), 2125 * COIN));
    BOOST_CHECK(IsFloatAmount(nsMgr->CalculateRenewalFee("old"), 11875 * COIN / 2));

    // More than two years: another 5%
    SetMockTime(TEST_TIME + 731 * DAY);
    BOOST_CHECK(IsFloatAmount(nsMgr->CalculateRenewalFee("oldns"), 2000 * COIN));
    BOOST_CHECK(IsFloatAmount(nsMgr->CalculateRenewalFee("old"), 5625 * COIN));
    SetMockTime(0);
}

// ===================================================================
// Category 8: Namespace auction transactions (ProcessTransaction)
//
// All four decoders take the acting key (creator, bidder, finaliser,
// canceller) from the first output of the transaction that pays to a
// public key hash. No input is looked at.
// ===================================================================

BOOST_AUTO_TEST_CASE(process_tx_ignores_other_transactions)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID key = MakeKeyID();

    // No OP_RETURN at all
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vout.emplace_back(COIN, GetScriptForDestination(PKHash(key)));
    BOOST_CHECK(!nsMgr->ProcessTransaction(CTransaction(mtx), 1));

    // An OP_RETURN that is not a ReddID record
    CScript other;
    other << OP_RETURN << std::vector<unsigned char>{'X', OP_NAMESPACE_AUCTION_CREATE, 1, 'a'};
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&key, other), 1));

    // A ReddID record of another subsystem
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&key, MakeNamespaceScript(OP_AUCTION_CREATE, {1, 'a', 1, 'b'})), 1));
}

// SUSPECTED BUG (documented, not fixed): ProcessNamespaceAuctionCreate passes
// a reserve price of 0 to CreateNamespaceAuction, which rejects any reserve
// below the minimum price for the name. A namespace auction can therefore
// never be created from a transaction, whatever the transaction pays.
BOOST_AUTO_TEST_CASE(process_create_tx_never_creates_an_auction)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CTransaction tx = MakeNamespaceTx(&creator, CreateNamespaceAuctionOpReturn("chainns"), 100000 * COIN);

    BOOST_CHECK(!nsMgr->ProcessTransaction(tx, 1));

    std::vector<uint256> auctionIds;
    BOOST_CHECK(reddidManager->GetReddIDDB()->ListAuctions(auctionIds));
    BOOST_CHECK(auctionIds.empty());
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("chainns"));
}

BOOST_AUTO_TEST_CASE(process_create_tx_rejects_bad_records)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();

    // Invalid namespace ID
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&creator, MakeNamespaceScript(OP_NAMESPACE_AUCTION_CREATE, {3, 'B', 'a', 'd'})), 1));
    // Length byte runs past the end of the record
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&creator, MakeNamespaceScript(OP_NAMESPACE_AUCTION_CREATE, {9, 'a', 'b'})), 1));
    // Namespace already taken
    CreateTestNamespace("takenns", MakeKeyID());
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&creator, CreateNamespaceAuctionOpReturn("takenns")), 1));
    // No output to take the creator from
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(nullptr, CreateNamespaceAuctionOpReturn("nocreator")), 1));
}

// The bidder is whoever the first pay-to-public-key-hash output pays, and the
// bid amount is the number in the record. SUSPECTED BUG (documented, not
// fixed): nothing ties either to the inputs or to the value of the
// transaction; the bid below is placed by a transaction that pays nothing.
BOOST_AUTO_TEST_CASE(process_bid_tx_places_bid)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CKeyID bidder = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("txbid");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("txbid", creator, reserve, 3, AUCTION_STANDARD, auctionId));

    CTransaction tx = MakeNamespaceTx(&bidder, MakeNamespaceBidScript(auctionId, reserve), 0);
    BOOST_CHECK(nsMgr->ProcessTransaction(tx, 1));

    std::vector<BidInfo> bids = nsMgr->GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK(bids[0].bidder == bidder);
    BOOST_CHECK_EQUAL(bids[0].bidAmount, reserve);
    BOOST_CHECK(bids[0].auctionId == auctionId);

    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(auction.currentBid, reserve);
    BOOST_CHECK(auction.currentBidder == bidder);
}

// Current behaviour: a record with fewer than 8 amount bytes takes the largest
// spendable output
// of the transaction as the bid amount.
BOOST_AUTO_TEST_CASE(process_bid_tx_short_record_uses_largest_output)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID bidder = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("txshort");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("txshort", MakeKeyID(), reserve, 3, AUCTION_STANDARD, auctionId));

    std::vector<unsigned char> payload(auctionId.begin(), auctionId.end());
    payload.push_back(0);
    payload.push_back(0);
    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vout.emplace_back(COIN, GetScriptForDestination(PKHash(bidder)));
    mtx.vout.emplace_back(reserve + 5, GetScriptForDestination(PKHash(MakeKeyID())));
    mtx.vout.emplace_back(0, MakeNamespaceScript(OP_NAMESPACE_AUCTION_BID, payload));
    BOOST_CHECK(nsMgr->ProcessTransaction(CTransaction(mtx), 1));

    std::vector<BidInfo> bids = nsMgr->GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK_EQUAL(bids[0].bidAmount, reserve + 5);
    BOOST_CHECK(bids[0].bidder == bidder);
}

BOOST_AUTO_TEST_CASE(process_bid_tx_rejects_bad_records)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID bidder = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("txbadbid");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("txbadbid", MakeKeyID(), reserve, 3, AUCTION_STANDARD, auctionId));

    // Truncated auction ID
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&bidder, MakeNamespaceScript(OP_NAMESPACE_AUCTION_BID, std::vector<unsigned char>(20, 1))), 1));
    // Unknown auction
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&bidder, MakeNamespaceBidScript(uint256S("01"), reserve)), 1));
    // Zero amount
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&bidder, MakeNamespaceBidScript(auctionId, 0)), 1));
    // No output to take the bidder from
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(nullptr, MakeNamespaceBidScript(auctionId, reserve)), 1));

    BOOST_CHECK(nsMgr->GetAuctionBids(auctionId).empty());
    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.state, AUCTION_PENDING);
}

// SUSPECTED BUG (documented, not fixed): BidOnNamespaceAuction moves a
// pending auction to active before it checks the bid, so a rejected bid still
// activates the auction.
BOOST_AUTO_TEST_CASE(rejected_bid_activates_pending_auction)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID bidder = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("txlowbid");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("txlowbid", MakeKeyID(), reserve, 3, AUCTION_STANDARD, auctionId));

    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&bidder, MakeNamespaceBidScript(auctionId, reserve - 1)), 1));

    BOOST_CHECK(nsMgr->GetAuctionBids(auctionId).empty());
    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(auction.currentBid, 0);
}

BOOST_AUTO_TEST_CASE(process_finalize_tx_creates_namespace)
{
    SetMockTime(TEST_TIME);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CKeyID bidder = MakeKeyID();
    CKeyID stranger = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("txfinal");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("txfinal", creator, reserve, 3, AUCTION_STANDARD, auctionId));
    BOOST_REQUIRE(nsMgr->ProcessTransaction(MakeNamespaceTx(&bidder, MakeNamespaceBidScript(auctionId, reserve)), 1));

    CScript finalize = MakeNamespaceScript(OP_NAMESPACE_AUCTION_FINALIZE, std::vector<unsigned char>(auctionId.begin(), auctionId.end()));

    // Not before the auction has ended
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&creator, finalize), 2));
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&bidder, finalize), 2));

    SetMockTime(TEST_TIME + 3 * DAY + 60);

    // Only the creator or the highest bidder
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&stranger, finalize), 3));
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(nullptr, finalize), 3));
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("txfinal"));

    BOOST_CHECK(nsMgr->ProcessTransaction(MakeNamespaceTx(&bidder, finalize), 3));

    NamespaceInfo info;
    BOOST_REQUIRE(nsMgr->GetNamespaceInfo("txfinal", info));
    BOOST_CHECK(info.owner == bidder);
    BOOST_CHECK_EQUAL(info.expiration, TEST_TIME + 3 * DAY + 60 + 365 * DAY);

    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.state, AUCTION_FINALIZED);
    std::vector<BidInfo> bids = nsMgr->GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK(bids[0].isWinner);

    // A finalized auction cannot be finalized again
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&creator, finalize), 4));
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(process_finalize_tx_rejects_bad_records)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID key = MakeKeyID();
    // Truncated auction ID
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&key, MakeNamespaceScript(OP_NAMESPACE_AUCTION_FINALIZE, std::vector<unsigned char>(20, 1))), 1));
    // Unknown auction
    uint256 unknown = uint256S("02");
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&key, MakeNamespaceScript(OP_NAMESPACE_AUCTION_FINALIZE, std::vector<unsigned char>(unknown.begin(), unknown.end()))), 1));
}

// SUSPECTED BUG (documented, not fixed): an auction only becomes active when
// a bid arrives, and both the finalize decoder and FinalizeNamespaceAuction
// refuse an auction that is still pending. An auction that never received a
// bid can therefore not be finalized, not even by its creator after it has
// ended; the decoder's "early finalization by creator" branch cannot succeed.
BOOST_AUTO_TEST_CASE(auction_without_bids_cannot_be_finalized)
{
    SetMockTime(TEST_TIME);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("txnobids");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("txnobids", creator, reserve, 3, AUCTION_STANDARD, auctionId));
    CScript finalize = MakeNamespaceScript(OP_NAMESPACE_AUCTION_FINALIZE, std::vector<unsigned char>(auctionId.begin(), auctionId.end()));

    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&creator, finalize), 1));
    SetMockTime(TEST_TIME + 3 * DAY + 60);
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&creator, finalize), 2));
    BOOST_CHECK(!nsMgr->FinalizeNamespaceAuction(auctionId));

    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.state, AUCTION_PENDING);
    SetMockTime(0);
}

// The canceller is taken from the first pay-to-public-key-hash output.
// SUSPECTED BUG (documented, not fixed): a transaction that merely pays the
// creator's address is accepted as the creator's cancellation, whoever funded
// and signed it. The same holds for the finalize and bid decoders.
BOOST_AUTO_TEST_CASE(process_cancel_tx_cancels_pending_auction)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CKeyID stranger = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("txcancel");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("txcancel", creator, reserve, 3, AUCTION_STANDARD, auctionId));
    CScript cancel = MakeNamespaceScript(OP_NAMESPACE_AUCTION_CANCEL, std::vector<unsigned char>(auctionId.begin(), auctionId.end()));

    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&stranger, cancel), 1));
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(nullptr, cancel), 1));

    BOOST_CHECK(nsMgr->ProcessTransaction(MakeNamespaceTx(&creator, cancel), 1));
    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.state, AUCTION_CANCELED);

    // A canceled auction cannot be canceled again, and takes no bids
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&creator, cancel), 2));
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&stranger, MakeNamespaceBidScript(auctionId, reserve)), 2));
}

BOOST_AUTO_TEST_CASE(process_cancel_tx_rejected_once_bid_placed)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CKeyID bidder = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("txkeep");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("txkeep", creator, reserve, 3, AUCTION_STANDARD, auctionId));
    BOOST_REQUIRE(nsMgr->ProcessTransaction(MakeNamespaceTx(&bidder, MakeNamespaceBidScript(auctionId, reserve)), 1));

    CScript cancel = MakeNamespaceScript(OP_NAMESPACE_AUCTION_CANCEL, std::vector<unsigned char>(auctionId.begin(), auctionId.end()));
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&creator, cancel), 2));
    BOOST_CHECK(!nsMgr->CancelNamespaceAuction(auctionId, creator));

    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.state, AUCTION_ACTIVE);

    // Truncated and unknown auction IDs
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&creator, MakeNamespaceScript(OP_NAMESPACE_AUCTION_CANCEL, std::vector<unsigned char>(20, 1))), 2));
    uint256 unknown = uint256S("03");
    BOOST_CHECK(!nsMgr->ProcessTransaction(MakeNamespaceTx(&creator, MakeNamespaceScript(OP_NAMESPACE_AUCTION_CANCEL, std::vector<unsigned char>(unknown.begin(), unknown.end()))), 2));
}

// SUSPECTED BUG (documented, not fixed): the auction ID is a hash of the
// namespace ID and the start time only. A second auction for the same name in
// the same second gets the same ID and silently replaces the first, creator
// included.
BOOST_AUTO_TEST_CASE(second_auction_in_same_second_replaces_first)
{
    SetMockTime(TEST_TIME);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID first = MakeKeyID();
    CKeyID second = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("txdup");
    uint256 firstId, secondId;
    BOOST_CHECK(nsMgr->CreateNamespaceAuction("txdup", first, reserve, 3, AUCTION_STANDARD, firstId));
    BOOST_CHECK(nsMgr->CreateNamespaceAuction("txdup", second, reserve * 2, 7, AUCTION_STANDARD, secondId));
    BOOST_CHECK(firstId == secondId);

    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(firstId, auction));
    BOOST_CHECK(auction.creator == second);
    BOOST_CHECK_EQUAL(auction.reservePrice, reserve * 2);

    // One second later the same name gets a second, concurrent auction
    SetMockTime(TEST_TIME + 1);
    uint256 thirdId;
    BOOST_CHECK(nsMgr->CreateNamespaceAuction("txdup", first, reserve, 3, AUCTION_STANDARD, thirdId));
    BOOST_CHECK(thirdId != firstId);
    SetMockTime(0);
}

// ===================================================================
// Category 9: UndoTransaction
//
// UndoTransaction returns true whether or not it found anything to undo.
// ===================================================================

BOOST_AUTO_TEST_CASE(undo_ignores_other_transactions)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("undonone");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("undonone", creator, reserve, 3, AUCTION_STANDARD, auctionId));

    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vout.emplace_back(COIN, GetScriptForDestination(PKHash(creator)));
    BOOST_CHECK(nsMgr->UndoTransaction(CTransaction(mtx), 1));
    BOOST_CHECK(nsMgr->UndoTransaction(MakeNamespaceTx(&creator, CreateNamespaceAuctionOpReturn("otherns")), 1));
    BOOST_CHECK(nsMgr->UndoTransaction(MakeNamespaceTx(&creator, MakeNamespaceBidScript(uint256S("04"), reserve)), 1));

    AuctionInfo auction;
    BOOST_CHECK(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.state, AUCTION_PENDING);
}

// Current behaviour: undoing a create record erases an auction for the named
// namespace. The
// auction is found by namespace ID alone: the transaction that is undone need
// not be the one that created it (and, see
// process_create_tx_never_creates_an_auction, never is), and with several
// auctions for one name the one erased is the first in auction ID order.
BOOST_AUTO_TEST_CASE(undo_create_erases_auction_for_namespace)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    ReddIDDB* db = reddidManager->GetReddIDDB();
    CKeyID creator = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("undocreate");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("undocreate", creator, reserve, 3, AUCTION_STANDARD, auctionId));
    BOOST_CHECK(db->ExistsAuction(auctionId));

    CKeyID stranger = MakeKeyID();
    BOOST_CHECK(nsMgr->UndoTransaction(MakeNamespaceTx(&stranger, CreateNamespaceAuctionOpReturn("undocreate")), 1));

    AuctionInfo auction;
    BOOST_CHECK(!nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK(!db->ExistsAuction(auctionId));
}

BOOST_AUTO_TEST_CASE(undo_bid_restores_previous_highest_bid)
{
    SetMockTime(TEST_TIME);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    ReddIDDB* db = reddidManager->GetReddIDDB();
    CKeyID bidder1 = MakeKeyID();
    CKeyID bidder2 = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("undobid");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("undobid", MakeKeyID(), reserve, 3, AUCTION_STANDARD, auctionId));

    CTransaction bid1 = MakeNamespaceTx(&bidder1, MakeNamespaceBidScript(auctionId, reserve));
    CTransaction bid2 = MakeNamespaceTx(&bidder2, MakeNamespaceBidScript(auctionId, reserve * 2));
    BOOST_REQUIRE(nsMgr->ProcessTransaction(bid1, 1));
    BOOST_REQUIRE(nsMgr->ProcessTransaction(bid2, 2));
    std::vector<BidInfo> bids = nsMgr->GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 2U);
    uint256 bidId2 = bids[1].bidId;

    BOOST_CHECK(nsMgr->UndoTransaction(bid2, 2));
    bids = nsMgr->GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK(bids[0].bidder == bidder1);
    BidInfo erased;
    BOOST_CHECK(!db->ReadBid(bidId2, erased));

    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(auction.currentBid, reserve);
    BOOST_CHECK(auction.currentBidder == bidder1);

    // Undoing the only remaining bid returns the auction to pending
    BOOST_CHECK(nsMgr->UndoTransaction(bid1, 1));
    BOOST_CHECK(nsMgr->GetAuctionBids(auctionId).empty());
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(auction.currentBid, 0);
    BOOST_CHECK(auction.currentBidder.IsNull());

    AuctionInfo stored;
    BOOST_REQUIRE(db->ReadAuction(auctionId, stored));
    BOOST_CHECK_EQUAL(stored.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(stored.currentBid, 0);
    SetMockTime(0);
}

// SUSPECTED BUG (documented, not fixed): a bid in the last 24 hours moves the
// end of the auction to 12 hours from the bid. Undoing that bid does not move
// the end back.
BOOST_AUTO_TEST_CASE(undo_bid_keeps_extended_end_time)
{
    SetMockTime(TEST_TIME);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID bidder = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("undosnipe");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("undosnipe", MakeKeyID(), reserve, 3, AUCTION_STANDARD, auctionId));

    SetMockTime(TEST_TIME + 3 * DAY - 60);
    CTransaction bid = MakeNamespaceTx(&bidder, MakeNamespaceBidScript(auctionId, reserve));
    BOOST_REQUIRE(nsMgr->ProcessTransaction(bid, 1));
    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.endTime, TEST_TIME + 3 * DAY - 60 + 12 * 60 * 60);

    BOOST_CHECK(nsMgr->UndoTransaction(bid, 1));
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.state, AUCTION_PENDING);
    BOOST_CHECK_EQUAL(auction.endTime, TEST_TIME + 3 * DAY - 60 + 12 * 60 * 60);
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(undo_finalize_removes_namespace_and_reopens_auction)
{
    SetMockTime(TEST_TIME);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    ReddIDDB* db = reddidManager->GetReddIDDB();
    CKeyID creator = MakeKeyID();
    CKeyID winner = MakeKeyID();
    CKeyID loser = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("undofinal");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("undofinal", creator, reserve, 3, AUCTION_STANDARD, auctionId));
    BOOST_REQUIRE(nsMgr->ProcessTransaction(MakeNamespaceTx(&loser, MakeNamespaceBidScript(auctionId, reserve)), 1));
    BOOST_REQUIRE(nsMgr->ProcessTransaction(MakeNamespaceTx(&winner, MakeNamespaceBidScript(auctionId, reserve * 2)), 2));

    SetMockTime(TEST_TIME + 3 * DAY + 60);
    CTransaction finalize = MakeNamespaceTx(&creator, MakeNamespaceScript(OP_NAMESPACE_AUCTION_FINALIZE, std::vector<unsigned char>(auctionId.begin(), auctionId.end())));
    BOOST_REQUIRE(nsMgr->ProcessTransaction(finalize, 3));
    BOOST_CHECK(nsMgr->IsOwner("undofinal", winner));
    std::vector<BidInfo> bids = nsMgr->GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 2U);
    BOOST_CHECK(bids[0].refunded && !bids[0].isWinner);
    BOOST_CHECK(bids[1].isWinner && !bids[1].refunded);

    BOOST_CHECK(nsMgr->UndoTransaction(finalize, 3));

    NamespaceInfo info;
    BOOST_CHECK(!nsMgr->GetNamespaceInfo("undofinal", info));
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("undofinal"));
    BOOST_CHECK(!db->ExistsNamespace("undofinal"));

    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(auction.currentBid, reserve * 2);
    bids = nsMgr->GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 2U);
    for (const BidInfo& bid : bids) {
        BOOST_CHECK(!bid.isWinner);
        BOOST_CHECK(!bid.refunded);
        BidInfo stored;
        BOOST_REQUIRE(db->ReadBid(bid.bidId, stored));
        BOOST_CHECK(!stored.isWinner);
        BOOST_CHECK(!stored.refunded);
    }

    // The auction can be finalized again
    BOOST_CHECK(nsMgr->ProcessTransaction(finalize, 3));
    BOOST_CHECK(nsMgr->IsOwner("undofinal", winner));
    SetMockTime(0);
}

// SUSPECTED BUG (documented, not fixed): undoing a finalization erases the
// namespace and drops its pricing tiers from memory, but leaves the four
// default tiers that the finalization wrote in the database, where
// GetPricingTiers finds them for a namespace that no longer exists.
BOOST_AUTO_TEST_CASE(undo_finalize_leaves_pricing_tiers_in_database)
{
    SetMockTime(TEST_TIME);
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CKeyID winner = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("undotiers");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("undotiers", creator, reserve, 3, AUCTION_STANDARD, auctionId));
    BOOST_REQUIRE(nsMgr->ProcessTransaction(MakeNamespaceTx(&winner, MakeNamespaceBidScript(auctionId, reserve)), 1));
    SetMockTime(TEST_TIME + 3 * DAY + 60);
    CTransaction finalize = MakeNamespaceTx(&creator, MakeNamespaceScript(OP_NAMESPACE_AUCTION_FINALIZE, std::vector<unsigned char>(auctionId.begin(), auctionId.end())));
    BOOST_REQUIRE(nsMgr->ProcessTransaction(finalize, 2));
    BOOST_CHECK_EQUAL(nsMgr->GetPricingTiers("undotiers").size(), 4U);

    BOOST_CHECK(nsMgr->UndoTransaction(finalize, 2));
    BOOST_CHECK(nsMgr->IsNamespaceAvailable("undotiers"));
    BOOST_CHECK_EQUAL(nsMgr->GetPricingTiers("undotiers").size(), 4U);
    SetMockTime(0);
}

BOOST_AUTO_TEST_CASE(undo_cancel_reopens_auction)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    ReddIDDB* db = reddidManager->GetReddIDDB();
    CKeyID creator = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("undocancel");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("undocancel", creator, reserve, 3, AUCTION_STANDARD, auctionId));
    CTransaction cancel = MakeNamespaceTx(&creator, MakeNamespaceScript(OP_NAMESPACE_AUCTION_CANCEL, std::vector<unsigned char>(auctionId.begin(), auctionId.end())));
    BOOST_REQUIRE(nsMgr->ProcessTransaction(cancel, 1));

    BOOST_CHECK(nsMgr->UndoTransaction(cancel, 1));
    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.state, AUCTION_PENDING);
    AuctionInfo stored;
    BOOST_REQUIRE(db->ReadAuction(auctionId, stored));
    BOOST_CHECK_EQUAL(stored.state, AUCTION_PENDING);

    // The reopened auction takes bids again
    CKeyID bidder = MakeKeyID();
    BOOST_CHECK(nsMgr->ProcessTransaction(MakeNamespaceTx(&bidder, MakeNamespaceBidScript(auctionId, reserve)), 2));
}

BOOST_AUTO_TEST_CASE(undo_truncated_records_change_nothing)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID creator = MakeKeyID();
    CKeyID bidder = MakeKeyID();
    CAmount reserve = nsMgr->CalculateMinPrice("undoshort");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("undoshort", creator, reserve, 3, AUCTION_STANDARD, auctionId));
    BOOST_REQUIRE(nsMgr->ProcessTransaction(MakeNamespaceTx(&bidder, MakeNamespaceBidScript(auctionId, reserve)), 1));

    // A bid record without the full 8 amount bytes is not undone
    std::vector<unsigned char> payload(auctionId.begin(), auctionId.end());
    payload.push_back(0);
    payload.push_back(0);
    BOOST_CHECK(nsMgr->UndoTransaction(MakeNamespaceTx(&bidder, MakeNamespaceScript(OP_NAMESPACE_AUCTION_BID, payload)), 1));
    BOOST_CHECK(nsMgr->UndoTransaction(MakeNamespaceTx(&creator, MakeNamespaceScript(OP_NAMESPACE_AUCTION_FINALIZE, std::vector<unsigned char>(20, 1))), 1));
    BOOST_CHECK(nsMgr->UndoTransaction(MakeNamespaceTx(&creator, MakeNamespaceScript(OP_NAMESPACE_AUCTION_CANCEL, std::vector<unsigned char>(20, 1))), 1));
    BOOST_CHECK(nsMgr->UndoTransaction(MakeNamespaceTx(&creator, MakeNamespaceScript(OP_NAMESPACE_AUCTION_CREATE, {9, 'a', 'b'})), 1));

    BOOST_CHECK_EQUAL(nsMgr->GetAuctionBids(auctionId).size(), 1U);
    AuctionInfo auction;
    BOOST_REQUIRE(nsMgr->GetAuctionInfo(auctionId, auction));
    BOOST_CHECK_EQUAL(auction.state, AUCTION_ACTIVE);
    BOOST_CHECK_EQUAL(auction.currentBid, reserve);
}

// ===================================================================
// Category 10: Loading from the database
// ===================================================================

BOOST_AUTO_TEST_CASE(load_restores_namespaces_tiers_auctions_and_bids)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CKeyID owner = MakeKeyID();
    CKeyID bidder = MakeKeyID();
    CreateTestNamespace("loadns", owner);
    BOOST_REQUIRE(nsMgr->AddPricingTier("loadns", 3, 7000 * COIN));
    CAmount reserve = nsMgr->CalculateMinPrice("loadauc");
    uint256 auctionId;
    BOOST_REQUIRE(nsMgr->CreateNamespaceAuction("loadauc", owner, reserve, 3, AUCTION_STANDARD, auctionId));
    uint256 bidId;
    BOOST_REQUIRE(nsMgr->BidOnNamespaceAuction(auctionId, bidder, reserve, bidId));

    // A second namespace manager on the same database sees all of it
    NamespaceManager fresh(*reddidManager);
    BOOST_CHECK(fresh.GetNamespaces().empty());
    BOOST_REQUIRE(fresh.Init(reddidManager.get()));

    std::vector<NamespaceInfo> loaded = fresh.GetNamespaces();
    BOOST_REQUIRE_EQUAL(loaded.size(), 1U);
    BOOST_CHECK_EQUAL(loaded[0].id, "loadns");
    BOOST_CHECK(loaded[0].owner == owner);
    BOOST_CHECK(fresh.IsOwner("loadns", owner));

    std::vector<PricingTier> tiers = fresh.GetPricingTiers("loadns");
    BOOST_REQUIRE_EQUAL(tiers.size(), 1U);
    BOOST_CHECK_EQUAL(tiers[0].minPrice, 7000 * COIN);

    std::vector<AuctionInfo> active = fresh.GetActiveAuctions();
    BOOST_REQUIRE_EQUAL(active.size(), 1U);
    BOOST_CHECK(active[0].auctionId == auctionId);
    BOOST_CHECK_EQUAL(active[0].namespaceId, "loadauc");
    BOOST_CHECK_EQUAL(active[0].currentBid, reserve);

    std::vector<BidInfo> bids = fresh.GetAuctionBids(auctionId);
    BOOST_REQUIRE_EQUAL(bids.size(), 1U);
    BOOST_CHECK(bids[0].bidId == bidId);
    BOOST_CHECK(bids[0].bidder == bidder);
}

// The minimum price of a user ID is the price of a pricing tier of its
// namespace, or REDDID_MIN_FEE when the namespace has no tier that applies.
BOOST_AUTO_TEST_CASE(min_price_for_user_id_uses_pricing_tiers)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();

    // Unknown namespace, and a namespace without tiers
    BOOST_CHECK_EQUAL(nsMgr->CalculateMinPriceForUserID("alice", "nosuchns"), REDDID_MIN_FEE);
    CreateTestNamespace("priced", MakeKeyID());
    BOOST_CHECK_EQUAL(nsMgr->CalculateMinPriceForUserID("alice", "priced"), REDDID_MIN_FEE);
    BOOST_CHECK_EQUAL(REDDID_MIN_FEE, 5000 * COIN);

    BOOST_REQUIRE(nsMgr->AddPricingTier("priced", 3, 9000 * COIN));
    BOOST_REQUIRE(nsMgr->AddPricingTier("priced", 5, 4000 * COIN));
    BOOST_REQUIRE(nsMgr->AddPricingTier("priced", 8, 1000 * COIN));

    // Shorter than every tier: the default applies
    BOOST_CHECK_EQUAL(nsMgr->CalculateMinPriceForUserID("ab", "priced"), REDDID_MIN_FEE);
    BOOST_CHECK_EQUAL(nsMgr->CalculateMinPriceForUserID("abc", "priced"), 9000 * COIN);
    BOOST_CHECK_EQUAL(nsMgr->CalculateMinPriceForUserID("abcd", "priced"), 9000 * COIN);
    BOOST_CHECK_EQUAL(nsMgr->CalculateMinPriceForUserID("abcde", "priced"), 4000 * COIN);
    BOOST_CHECK_EQUAL(nsMgr->CalculateMinPriceForUserID("abcdefg", "priced"), 4000 * COIN);
    BOOST_CHECK_EQUAL(nsMgr->CalculateMinPriceForUserID("abcdefgh", "priced"), 1000 * COIN);
    BOOST_CHECK_EQUAL(nsMgr->CalculateMinPriceForUserID(std::string(32, 'a'), "priced"), 1000 * COIN);

    // The tiers of one namespace do not apply to another
    CreateTestNamespace("plain", MakeKeyID());
    BOOST_CHECK_EQUAL(nsMgr->CalculateMinPriceForUserID("abcdefgh", "plain"), REDDID_MIN_FEE);
}

// SUSPECTED BUG (documented, not fixed): the lookup takes the last tier, in
// the order in which the tiers are held, whose minimum length the name
// reaches. That is the tier for the longest matching length only if the tiers
// were added in ascending order of length. Added in another order, a shorter
// tier that comes later overrides a longer one.
BOOST_AUTO_TEST_CASE(min_price_for_user_id_depends_on_tier_order)
{
    NamespaceManager* nsMgr = reddidManager->GetNamespaceManager();
    CreateTestNamespace("unordered", MakeKeyID());
    BOOST_REQUIRE(nsMgr->AddPricingTier("unordered", 8, 1000 * COIN));
    BOOST_REQUIRE(nsMgr->AddPricingTier("unordered", 3, 9000 * COIN));

    // Eight characters reach both tiers; the tier for three characters was added last
    BOOST_CHECK_EQUAL(nsMgr->CalculateMinPriceForUserID("abcdefgh", "unordered"), 9000 * COIN);
    BOOST_CHECK_EQUAL(nsMgr->CalculateMinPriceForUserID("abc", "unordered"), 9000 * COIN);
}

// SUSPECTED BUG (documented, not fixed): every auction shares one key prefix
// in the database, and Load takes all of them. An auction for a user ID, which
// belongs to another subsystem, is loaded as a namespace auction (and is
// written back by Save when the namespace manager stops).
BOOST_AUTO_TEST_CASE(load_takes_every_auction_in_the_database)
{
    AuctionInfo userIdAuction;
    userIdAuction.auctionId = uint256S("05");
    userIdAuction.name = "alice";
    userIdAuction.namespaceId = "redd";
    userIdAuction.creator = MakeKeyID();
    userIdAuction.state = AUCTION_ACTIVE;
    BOOST_REQUIRE(reddidManager->GetReddIDDB()->WriteAuction(userIdAuction.auctionId, userIdAuction));

    NamespaceManager fresh(*reddidManager);
    BOOST_REQUIRE(fresh.Init(reddidManager.get()));
    std::vector<AuctionInfo> active = fresh.GetActiveAuctions();
    BOOST_REQUIRE_EQUAL(active.size(), 1U);
    BOOST_CHECK_EQUAL(active[0].name, "alice");
}

BOOST_AUTO_TEST_SUITE_END()
