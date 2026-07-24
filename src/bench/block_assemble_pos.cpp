// Copyright (c) 2023-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <chainparams.h>
#include <miner.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <txmempool.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <validation.h>
#include <wallet/wallet.h>

#include <cassert>
#include <cstdint>

// ReddCoin: benchmark proof-of-stake block assembly, the counterpart to the
// proof-of-work AssembleBlock benchmark. TestChain100Setup pre-stakes 11 PoS
// blocks (heights 90-100) with a live staking wallet, so the tip is already in
// the PoS phase and CreateNewBlock takes the coinstake path.
static void AssemblePoSBlock(benchmark::Bench& bench)
{
    const auto test_setup = MakeNoLogFileContext<const TestChain100Setup>();
    CWallet* const pwallet = test_setup->m_wallet.get();
    assert(pwallet);

    const CChainParams& chainparams = Params();
    CChainState& chainstate = test_setup->m_node.chainman->ActiveChainstate();
    CTxMemPool& mempool = *test_setup->m_node.mempool;
    const CScript scriptPubKey = CScript() << ToByteVector(test_setup->coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    int64_t t = GetTime();
    bench.run([&] {
        // Advance mocktime each iteration so the coinstake kernel search runs over
        // a fresh, non-empty window: PoS CreateNewBlock only searches when the
        // current time is past its last search time. No block is processed, so the
        // tip and the stakeable coins stay fixed and the loop runs indefinitely.
        // With a wallet set, CreateNewBlock returns nullptr when no kernel is found
        // (it does not fall through to the pow-ended PoW path), so this never
        // throws; some iterations find a stake and some do not, which is the cost
        // this microbenchmark measures.
        t += 64;
        SetMockTime(t);
        bool fPoSCancel{false};
        LOCK(pwallet->cs_wallet);
        BlockAssembler{chainstate, mempool, chainparams}.CreateNewBlock(scriptPubKey, pwallet, &fPoSCancel);
    });

    SetMockTime(0);
}

BENCHMARK(AssemblePoSBlock);
