// Copyright (c) 2015-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <pow.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <validation.h>

#include <test/util/setup_common.h>

#include <string>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

/* Test calculation of next difficulty target with no constraints applying */
BOOST_AUTO_TEST_CASE(get_next_work)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    int64_t nLastRetargetTime = 1261130161; // Block #30240
    CBlockIndex pindexLast;
    pindexLast.nHeight = 32255;
    pindexLast.nTime = 1262152739;  // Block #32255
    pindexLast.nBits = 0x1d00ffff;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1d00d86aU);
}

/* Test the constraint on the upper bound for next work */
BOOST_AUTO_TEST_CASE(get_next_work_pow_limit)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    int64_t nLastRetargetTime = 1231006505; // Block #0
    CBlockIndex pindexLast;
    pindexLast.nHeight = 2015;
    pindexLast.nTime = 1233061996;  // Block #2015
    pindexLast.nBits = 0x1d00ffff;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1d00ffffU);
}

/* Test the constraint on the lower bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_lower_limit_actual)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    int64_t nLastRetargetTime = 1279008237; // Block #66528
    CBlockIndex pindexLast;
    pindexLast.nHeight = 68543;
    pindexLast.nTime = 1279297671;  // Block #68543
    pindexLast.nBits = 0x1c05a3f4;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1c0168fdU);
}

/* Test the constraint on the upper bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_upper_limit_actual)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    int64_t nLastRetargetTime = 1263163443; // NOTE: Not an actual block time
    CBlockIndex pindexLast;
    pindexLast.nHeight = 46367;
    pindexLast.nTime = 1269211443;  // Block #46367
    pindexLast.nBits = 0x1c387f6f;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1d00e1fdU);
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_negative_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    nBits = UintToArith256(consensus.powLimit).GetCompact(true);
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_overflow_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits = ~0x00800000;
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_too_easy_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 nBits_arith = UintToArith256(consensus.powLimit);
    nBits_arith *= 2;
    nBits = nBits_arith.GetCompact();
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_biger_hash_than_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith = UintToArith256(consensus.powLimit);
    nBits = hash_arith.GetCompact();
    hash_arith *= 2; // hash > nBits
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_zero_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith{0};
    nBits = hash_arith.GetCompact();
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    std::vector<CBlockIndex> blocks(10000);
    for (int i = 0; i < 10000; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1269211443 + i * chainParams->GetConsensus().nPowTargetSpacing;
        blocks[i].nBits = 0x207fffff; /* target 0x7fffff000... */
        blocks[i].nChainWork = i ? blocks[i - 1].nChainWork + GetBlockProof(blocks[i - 1]) : arith_uint256(0);
    }

    for (int j = 0; j < 1000; j++) {
        CBlockIndex *p1 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p2 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p3 = &blocks[InsecureRandRange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }
}

void sanity_check_chainparams(const ArgsManager& args, std::string chainName)
{
    const auto chainParams = CreateChainParams(args, chainName);
    const auto consensus = chainParams->GetConsensus();

    // hash genesis is correct
    BOOST_CHECK_EQUAL(consensus.hashGenesisBlock, chainParams->GenesisBlock().GetHash());

    // target timespan is an even multiple of spacing
    BOOST_CHECK_EQUAL(consensus.nPowTargetTimespan % consensus.nPowTargetSpacing, 0);

    // genesis nBits is positive, doesn't overflow and is lower than powLimit
    arith_uint256 pow_compact;
    bool neg, over;
    pow_compact.SetCompact(chainParams->GenesisBlock().nBits, &neg, &over);
    BOOST_CHECK(!neg && pow_compact != 0);
    BOOST_CHECK(!over);
    BOOST_CHECK(UintToArith256(consensus.powLimit) >= pow_compact);

    // check max target * 4*nPowTargetTimespan doesn't overflow -- see pow.cpp:CalculateNextWorkRequired()
    if (!consensus.fPowNoRetargeting) {
        arith_uint256 targ_max("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
        targ_max /= consensus.nPowTargetTimespan*4;
        BOOST_CHECK(UintToArith256(consensus.powLimit) < targ_max);
    }
}

BOOST_AUTO_TEST_CASE(ChainParams_MAIN_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::MAIN);
}

BOOST_AUTO_TEST_CASE(ChainParams_REGTEST_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::REGTEST);
}

BOOST_AUTO_TEST_CASE(ChainParams_TESTNET_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::TESTNET);
}

BOOST_AUTO_TEST_CASE(ChainParams_SIGNET_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::SIGNET);
}

//! A target far harder than any regtest header will meet by accident.
static const uint32_t UNREACHABLE_TARGET{0x1d00ffff};

//! Build a block carrying only a header.
//!
//! CheckBlockHeader runs before every check that inspects transactions, so a
//! block with no transactions is enough to reach it. Anything that gets past
//! the header check fails later for an unrelated reason, which is why the
//! cases below test the reject reason rather than the return value.
static CBlock HeaderOnlyBlock(int32_t version, uint32_t time, uint32_t bits)
{
    CBlock block;
    block.nVersion = version;
    block.hashPrevBlock.SetNull();
    block.hashMerkleRoot.SetNull();
    block.nTime = time;
    block.nBits = bits;
    block.nNonce = 0;
    return block;
}

//! Proof of work must actually be verified.
//!
//! This is the regression test for the stub CheckBlockHeader that shipped on
//! this line: 80b9c562c6 deleted the check during the PoSV migration, so a
//! block's claimed nBits went untested against any work for the whole of the
//! historical proof-of-work range.
BOOST_AUTO_TEST_CASE(pow_is_rejected_when_the_header_misses_its_target)
{
    const Consensus::Params& params = Params().GetConsensus();
    CBlock block = HeaderOnlyBlock(POW_BLOCK_VERSION, CHECK_POW_FROM_NTIME + 1, UNREACHABLE_TARGET);

    // The premise: this header does not meet the target it claims. Asserted
    // rather than assumed, so the case cannot quietly stop testing anything
    // if the hash or the target ever changes.
    BOOST_CHECK(!CheckProofOfWork(block.GetPoWHash(), block.nBits, params));

    BlockValidationState state;
    BOOST_CHECK(!CheckBlock(block, state, params));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), std::string{"high-hash"});
}

//! The control: a header that meets its target passes the proof-of-work check.
//! Without this, the case above would still pass if the check rejected
//! everything put in front of it.
BOOST_AUTO_TEST_CASE(pow_is_accepted_when_the_header_meets_its_target)
{
    const Consensus::Params& params = Params().GetConsensus();
    const uint32_t easiest{UintToArith256(params.powLimit).GetCompact()};
    CBlock block = HeaderOnlyBlock(POW_BLOCK_VERSION, CHECK_POW_FROM_NTIME + 1, easiest);
    while (!CheckProofOfWork(block.GetPoWHash(), block.nBits, params)) ++block.nNonce;

    BlockValidationState state;
    CheckBlock(block, state, params);
    BOOST_CHECK_NE(state.GetRejectReason(), std::string{"high-hash"});
}

//! Proof-of-stake headers carry no proof of work and are exempt.
//!
//! Note which predicate decides this. CheckBlockHeader takes a CBlockHeader,
//! so IsProofOfWork resolves to the header's version test and not to the
//! coinstake test CBlock shadows it with. Every block the miner produces
//! carries a versionbits nVersion, so the check does not apply to them: its
//! reach is the historical proof-of-work era, where blocks carry version 1
//! or 2. This case exists to pin that down, because it is not obvious from
//! reading CheckBlockHeader alone.
BOOST_AUTO_TEST_CASE(pow_is_not_checked_for_proof_of_stake_headers)
{
    const Consensus::Params& params = Params().GetConsensus();
    CBlock block = HeaderOnlyBlock(POW_BLOCK_VERSION + 1, CHECK_POW_FROM_NTIME + 1, UNREACHABLE_TARGET);
    BOOST_CHECK(block.CBlockHeader::IsProofOfStake());
    BOOST_CHECK(!CheckProofOfWork(block.GetPoWHash(), block.nBits, params));

    BlockValidationState state;
    CheckBlock(block, state, params);
    BOOST_CHECK_NE(state.GetRejectReason(), std::string{"high-hash"});
}

//! Headers at or before CHECK_POW_FROM_NTIME are exempt, matching the
//! historical chain, which is why the constant exists.
BOOST_AUTO_TEST_CASE(pow_is_not_checked_before_check_pow_from_ntime)
{
    const Consensus::Params& params = Params().GetConsensus();
    CBlock block = HeaderOnlyBlock(POW_BLOCK_VERSION, CHECK_POW_FROM_NTIME, UNREACHABLE_TARGET);
    BOOST_CHECK(!CheckProofOfWork(block.GetPoWHash(), block.nBits, params));

    BlockValidationState state;
    CheckBlock(block, state, params);
    BOOST_CHECK_NE(state.GetRejectReason(), std::string{"high-hash"});
}

BOOST_AUTO_TEST_SUITE_END()
