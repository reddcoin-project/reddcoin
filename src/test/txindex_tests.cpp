// Copyright (c) 2017-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <index/txindex.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <util/time.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(txindex_tests)

BOOST_FIXTURE_TEST_CASE(txindex_initial_sync, TestChain100Setup)
{
    TxIndex txindex(1 << 20, true);

    CTransactionRef tx_disk;
    uint256 block_hash;

    // Transaction should not be found in the index before it is started.
    for (const auto& txn : m_coinbase_txns) {
        BOOST_CHECK(!txindex.FindTx(txn->GetHash(), block_hash, tx_disk));
    }

    // BlockUntilSyncedToCurrentChain should return false before txindex is started.
    BOOST_CHECK(!txindex.BlockUntilSyncedToCurrentChain());

    BOOST_REQUIRE(txindex.Start(m_node.chainman->ActiveChainstate()));

    // Allow tx index to catch up with the block index.
    constexpr int64_t timeout_ms = 10 * 1000;
    int64_t time_start = GetTimeMillis();
    while (!txindex.BlockUntilSyncedToCurrentChain()) {
        BOOST_REQUIRE(time_start + timeout_ms > GetTimeMillis());
        UninterruptibleSleep(std::chrono::milliseconds{100});
    }

    // Check that txindex excludes genesis block transactions.
    const CBlock& genesis_block = Params().GenesisBlock();
    for (const auto& txn : genesis_block.vtx) {
        BOOST_CHECK(!txindex.FindTx(txn->GetHash(), block_hash, tx_disk));
    }

    // Check that txindex has all txs that were in the chain before it started.
    for (const auto& txn : m_coinbase_txns) {
        if (!txindex.FindTx(txn->GetHash(), block_hash, tx_disk)) {
            BOOST_ERROR("FindTx failed");
        } else if (tx_disk->GetHash() != txn->GetHash()) {
            BOOST_ERROR("Read incorrect tx");
        }
    }

    // Check that new transactions in new blocks make it into the index.
    // Note: TestChain100Setup creates blocks up to height 100, with PoW ending at block 89
    // So we must use PoS blocks for additional blocks
    for (int i = 0; i < 10; i++) {
        CScript coinbase_script_pub_key = GetScriptForDestination(PKHash(coinbaseKey.GetPubKey()));
        std::vector<CMutableTransaction> no_txns;
        const CBlock& block = CreateAndProcessPoSBlock(no_txns, coinbase_script_pub_key, m_wallet);

        // CRITICAL: Ensure wallet receives and processes block notifications before creating next block
        // 1. Sync validation interface queue to process all pending notifications
        SyncWithValidationInterfaceQueue();
        // 2. Wait for wallet to sync to the current chain tip
        m_wallet->BlockUntilSyncedToCurrentChain();
        // 3. Force wallet to re-evaluate all transactions and update spent status
        {
            LOCK(m_wallet->cs_wallet);
            m_wallet->ReacceptWalletTransactions();
        }
        // 4. Extra sync to ensure wallet has fully processed the coinstake transaction
        SyncWithValidationInterfaceQueue();
        m_wallet->BlockUntilSyncedToCurrentChain();

        // PoS blocks have coinstake as vtx[1]
        BOOST_REQUIRE(block.vtx.size() >= 2);
        const CTransaction& txn = *block.vtx[1];

        BOOST_CHECK(txindex.BlockUntilSyncedToCurrentChain());
        if (!txindex.FindTx(txn.GetHash(), block_hash, tx_disk)) {
            BOOST_ERROR("FindTx failed");
        } else if (tx_disk->GetHash() != txn.GetHash()) {
            BOOST_ERROR("Read incorrect tx");
        }

        // Increment mock time by 60 seconds to allow coins to gain stake age for next block
        SetMockTime(GetTime() + 60);
    }

    // shutdown sequence (c.f. Shutdown() in init.cpp)
    txindex.Stop();

    // Let scheduler events finish running to avoid accessing any memory related to txindex after it is destructed
    SyncWithValidationInterfaceQueue();
}

BOOST_AUTO_TEST_SUITE_END()
