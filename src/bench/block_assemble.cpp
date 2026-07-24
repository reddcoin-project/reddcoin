// Copyright (c) 2011-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <chainparams.h>
#include <consensus/consensus.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <crypto/sha256.h>
#include <test/util/mining.h>
#include <test/util/script.h>
#include <test/util/setup_common.h>
#include <test/util/wallet.h>
#include <txmempool.h>
#include <validation.h>

#include <algorithm>
#include <vector>

static void AssembleBlock(benchmark::Bench& bench)
{
    // TestingSetup calls SelectParams(), which populates globalChainParams; it
    // must be constructed before Params() is read, or Params() aborts on the
    // globalChainParams assertion when this benchmark runs first under `make check`.
    const auto test_setup = MakeNoLogFileContext<const TestingSetup>();
    const CChainParams& chainparams = Params();

    CScriptWitness witness;
    witness.stack.push_back(WITNESS_STACK_ELEM_OP_TRUE);

    // Collect some loose transactions that spend the coinbases of our mined blocks.
    // ReddCoin: PoW ends at nLastPowHeight (89 on regtest); a PoW CreateNewBlock
    // above that height is rejected with "pow-ended". MineBlock advances the tip and
    // the benchmarked PrepareBlock assembles one more block on top of it, so cap the
    // mined count at nLastPowHeight - 1 to keep every template within the PoW range.
    // Chains where PoW does not end early (nLastPowHeight is large) still mine 200.
    const int NUM_BLOCKS{std::min(200, chainparams.GetConsensus().nLastPowHeight - 1)};
    const int COINBASE_MATURITY = chainparams.GetConsensus().GetCoinbaseMaturity();
    std::vector<CTransactionRef> txs;
    for (int b{0}; b < NUM_BLOCKS; ++b) {
        CMutableTransaction tx;
        tx.vin.push_back(MineBlock(test_setup->m_node, P2WSH_OP_TRUE));
        tx.vin.back().scriptWitness = witness;
        tx.vout.emplace_back(1337, P2WSH_OP_TRUE);
        if (NUM_BLOCKS - b >= COINBASE_MATURITY)
            txs.push_back(MakeTransactionRef(tx));
    }
    {
        LOCK(::cs_main); // Required for ::AcceptToMemoryPool.

        for (const auto& txr : txs) {
            const MempoolAcceptResult res = ::AcceptToMemoryPool(test_setup->m_node.chainman->ActiveChainstate(), *test_setup->m_node.mempool, txr, false /* bypass_limits */);
            assert(res.m_result_type == MempoolAcceptResult::ResultType::VALID);
        }
    }

    bench.run([&] {
        PrepareBlock(test_setup->m_node, P2WSH_OP_TRUE);
    });
}

BENCHMARK(AssembleBlock);
