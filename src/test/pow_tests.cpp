// Copyright (c) 2015-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <crypto/scrypt.h>
#include <pow.h>
#include <primitives/block.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#ifndef WIN32
#include <algorithm>
#include <climits>
#include <pthread.h>
#endif

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

/* Test CalculateNextWorkRequired stub returns 0 */
// NOTE: Reddcoin uses Kimoto Gravity Well, not Bitcoin's CalculateNextWorkRequired
// CalculateNextWorkRequired() is a stub that returns 0 in Reddcoin
BOOST_AUTO_TEST_CASE(calculate_next_work_stub)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    CBlockIndex pindexLast;
    pindexLast.nHeight = 1000;
    pindexLast.nTime = 1391411877;
    pindexLast.nBits = 0x1c05f176;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, 0, chainParams->GetConsensus()), 0U);
}

/* Test GetNextWorkRequired with actual Reddcoin mainnet blocks */
// Kimoto Gravity Well difficulty adjustment
BOOST_AUTO_TEST_CASE(get_next_work_kgw)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);

    // Build a simple chain to test KGW
    // Using actual mainnet block data: height 1000, time 1391411877, bits 0x1c05f176
    CBlockIndex pindexLast;
    pindexLast.nHeight = 1000;
    pindexLast.nTime = 1391411877;
    pindexLast.nBits = 0x1c05f176;
    pindexLast.pprev = nullptr;

    // Test that GetNextWorkRequired returns a valid difficulty
    CBlockHeader blockHeader;
    blockHeader.nTime = pindexLast.nTime + 60; // 1 minute later
    unsigned int nBits = GetNextWorkRequired(&pindexLast, &blockHeader, chainParams->GetConsensus());

    // Verify it's within powLimit
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);
    BOOST_CHECK(bnTarget <= UintToArith256(chainParams->GetConsensus().powLimit));
    BOOST_CHECK(bnTarget > 0);
}

/* Test GetNextWorkRequired returns powLimit for very early blocks */
BOOST_AUTO_TEST_CASE(get_next_work_pow_limit)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);

    // Very early block should return powLimit
    CBlockIndex pindexLast;
    pindexLast.nHeight = 5;
    pindexLast.nTime = 1390280460; // shortly after genesis
    pindexLast.nBits = 0x1e0ffff0;
    pindexLast.pprev = nullptr;

    CBlockHeader blockHeader;
    blockHeader.nTime = pindexLast.nTime + 60;
    unsigned int nBits = GetNextWorkRequired(&pindexLast, &blockHeader, chainParams->GetConsensus());

    // Should return powLimit for blocks with height < PastBlocksMin
    BOOST_CHECK_EQUAL(nBits, UintToArith256(chainParams->GetConsensus().powLimit).GetCompact());
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

#ifndef WIN32
//! The proof-of-work hash must not depend on the size of the thread stack.
//!
//! scrypt's scratchpad is 128 KiB, and musl gives every thread but the main
//! one a 128 KiB stack. While the scratchpad sat on the stack, reddcoind
//! segfaulted in the Alpine docker image the first time a header was hashed
//! off the main thread: at the testnet genesis block used here on testnet and
//! regtest, and during header sync on mainnet.
//!
//! The expected hash was computed independently with OpenSSL's scrypt
//! (N=1024, r=1, p=1). The guard below the stack is made at least as large as
//! the scratchpad, so a scratchpad back on the stack faults rather than
//! writing over whatever is mapped below it.
BOOST_AUTO_TEST_CASE(pow_hash_on_a_small_thread_stack)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::TESTNET);
    struct Job {
        CBlockHeader header;
        uint256 hash;
    } job{chainParams->GenesisBlock().GetBlockHeader(), {}};

    pthread_attr_t attr;
    BOOST_REQUIRE_EQUAL(pthread_attr_init(&attr), 0);
    BOOST_REQUIRE_EQUAL(pthread_attr_setstacksize(&attr, std::max<size_t>(64 * 1024, PTHREAD_STACK_MIN)), 0);
    BOOST_REQUIRE_EQUAL(pthread_attr_setguardsize(&attr, SCRYPT_SCRATCHPAD_SIZE), 0);
    pthread_t thread;
    const int created{pthread_create(&thread, &attr, [](void* arg) -> void* {
        auto* job = static_cast<Job*>(arg);
        job->hash = job->header.GetPoWHash();
        return nullptr;
    }, &job)};
    pthread_attr_destroy(&attr);
    BOOST_REQUIRE_EQUAL(created, 0);
    BOOST_REQUIRE_EQUAL(pthread_join(thread, nullptr), 0);

    BOOST_CHECK_EQUAL(job.hash, uint256S("00000a9a56d92855f2590eb528ddfb52ac93811c53c9b50a3e8b25653efd509b"));
    BOOST_CHECK(CheckProofOfWork(job.hash, job.header.nBits, chainParams->GetConsensus()));
}
#endif

BOOST_AUTO_TEST_SUITE_END()
