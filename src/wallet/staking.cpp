// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/staking.h>

#include <chainparams.h>
#include <consensus/tx_verify.h>
#include <deploymentstatus.h>
#include <index/disktxpos.h>
#include <index/txindex.h>
#include <interfaces/handler.h>
#include <node/blockstorage.h>
#include <pos/kernel.h>
#include <pos/stake.h>
#include <rpc/util.h>
#include <script/standard.h>
#include <threadsafety.h>
#include <util/system.h>
#include <wallet/coincontrol.h>
#include <wallet/rpcwallet.h>
#include <wallet/wallet.h>

typedef std::vector<unsigned char> valtype;

bool GetStakeWeight(const CWallet* pwallet, uint64_t& nAverageWeight, uint64_t & nTotalWeight, const Consensus::Params& consensusParams)
{
      // Choose coins to use
      LOCK(pwallet->cs_wallet);
      CAmount nBalance = pwallet->GetBalance().m_mine_trusted;
      CAmount nReserveBalance = 0;
      if (gArgs.IsArgSet("-reservebalance") && !ParseMoney(gArgs.GetArg("-reservebalance", ""), nReserveBalance))
          return error("CreateCoinStake : invalid reserve balance amount");
      if (nBalance <= nReserveBalance)
          return false;

      std::vector<CTransactionRef> vwtxPrev;
      std::set<CInputCoin> setCoins;

      CAmount nValueIn = 0;

      std::vector<COutput> vAvailableCoins;
      CCoinControl temp;
      CoinSelectionParams coin_selection_params;
      pwallet->AvailableCoins(vAvailableCoins, &temp);
      if (!pwallet->SelectCoins(vAvailableCoins, nBalance - nReserveBalance, setCoins, nValueIn, temp, coin_selection_params))
          return false;
      if (setCoins.empty())
          return false;

      nAverageWeight = nTotalWeight = 0;
      uint64_t nWeightCount = 0;

      for (const auto& pcoin : setCoins)
      {
          CDiskTxPos postx;
          if (!g_txindex->FindTxPosition(pcoin.outpoint.hash, postx))
              continue;

          // Read block header
          CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
          CBlockHeader header;
          CTransactionRef txRef;
          try {
              file >> header;
              fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
              file >> txRef;
          } catch (std::exception &e) {
              return error("%s() : deserialize or I/O error in GetStakeWeight()", __PRETTY_FUNCTION__);
          }

          CMutableTransaction tx(*txRef);

          // Deal with transaction timestamp
          unsigned int nTimeTx = tx.nTime ? tx.nTime : header.GetBlockTime();

          int64_t nTimeWeight = GetCoinAgeWeight((int64_t)nTimeTx, (int64_t)GetTime(), consensusParams);
          arith_uint512 bnCoinDayWeight = arith_uint512(pcoin.txout.nValue) * nTimeWeight / COIN / (24 * 60 * 60);

          // Weight is greater than zero
          if (nTimeWeight > 0)
          {
              nTotalWeight += bnCoinDayWeight.GetLow64();
              nWeightCount++;
          }

      }

  if (nWeightCount > 0)
      nAverageWeight = nTotalWeight / nWeightCount;

  return true;
}

namespace {

// The following split & combine thresholds are important to security
// Should not be adjusted if you don't understand the consequences
static const unsigned int nStakeSplitAge = (60 * 60 * 24 * 45);
static const int64_t nCombineThreshold = 2000000 * COIN;

enum class CoinSource { OK, MISSING, FAILED };

//! Read the block header and transaction that created a coin, through the
//! transaction index. MISSING when the index has no entry for it, FAILED on
//! a read error; callers skip the first and abandon the pass on the second,
//! as the search always has.
CoinSource ReadCoinSource(const COutPoint& outpoint, CBlockHeader& header, CTransactionRef& tx)
{
    // Transaction index is required to get to block header
    if (!g_txindex) {
        error("CreateCoinStake : transaction index unavailable");
        return CoinSource::FAILED;
    }
    CDiskTxPos postx;
    if (!g_txindex->FindTxPosition(outpoint.hash, postx))
        return CoinSource::MISSING;
    CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
    try {
        file >> header;
        fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
        file >> tx;
    } catch (const std::exception& e) {
        error("%s() : deserialize or I/O error reading %s", __func__, outpoint.ToString());
        return CoinSource::FAILED;
    }
    return CoinSource::OK;
}

//! Whether an output carries witness data. Such a coin can only be staked
//! once SegWit is active for the next block: a coinstake spending it before
//! then makes a block that ContextualCheckBlock rejects for unexpected
//! witness data.
bool IsWitnessOutput(const CScript& scriptPubKey)
{
    std::vector<valtype> vSolutions;
    const TxoutType type = Solver(scriptPubKey, vSolutions);
    return type == TxoutType::WITNESS_V0_KEYHASH ||
           type == TxoutType::WITNESS_V0_SCRIPTHASH ||
           type == TxoutType::WITNESS_V1_TAPROOT ||
           type == TxoutType::WITNESS_UNKNOWN;
}

//! The coinstake output script for a kernel, and whether the wallet holds
//! the key to sign for it: a keyhash kernel pays to its public key, a pubkey
//! or taproot kernel to its own script. Takes cs_wallet for the lookup alone
//! and must not be called with cs_main held, which is the lock order the
//! staking path keeps.
bool KernelOutputScript(const CWallet* pwallet, const CScript& scriptPubKeyKernel, TxoutType& whichType, CScript& scriptPubKeyOut)
{
    std::vector<valtype> vSolutions;
    whichType = Solver(scriptPubKeyKernel, vSolutions);
    if (whichType != TxoutType::PUBKEY &&
        whichType != TxoutType::PUBKEYHASH &&
        whichType != TxoutType::WITNESS_V0_KEYHASH &&
        whichType != TxoutType::WITNESS_V1_TAPROOT) {
        LogPrintf("CreateCoinStake : no support for kernel type=%s\n", GetTxnOutputType(whichType));
        return false;
    }

    LOCK(pwallet->cs_wallet);
    scriptPubKeyOut.clear();
    if (whichType == TxoutType::PUBKEYHASH || whichType == TxoutType::WITNESS_V0_KEYHASH) // pay to address type or witness keyhash
    {
        // convert to pay to public key type
        CKey key;
        CKeyID keyid{uint160{vSolutions[0]}};
        bool found_key = false;

        // Try all ScriptPubKeyMans (supports both legacy and descriptor wallets)
        for (ScriptPubKeyMan* spk_man : pwallet->GetAllScriptPubKeyMans()) {
            SignatureData sigdata;
            if (spk_man->CanProvide(scriptPubKeyKernel, sigdata)) {
                if (auto* legacy = dynamic_cast<LegacyScriptPubKeyMan*>(spk_man)) {
                    if (legacy->GetKey(keyid, key)) {
                        found_key = true;
                        break;
                    }
                } else if (auto* desc = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man)) {
                    if (desc->GetKey(scriptPubKeyKernel, keyid, key)) {
                        found_key = true;
                        break;
                    }
                }
            }
        }

        if (!found_key) {
            LogPrintf("CreateCoinStake : failed to get key for kernel type=%s\n", GetTxnOutputType(whichType));
            return false;
        }
        scriptPubKeyOut << ToByteVector(key.GetPubKey()) << OP_CHECKSIG;
        return true;
    }
    if (whichType == TxoutType::WITNESS_V1_TAPROOT)
    {
        // Taproot: verify we have the key for key-path spending
        // vSolutions[0] contains the 32-byte x-only pubkey
        bool found_key = false;

        // Try all ScriptPubKeyMans (supports both legacy and descriptor wallets)
        for (ScriptPubKeyMan* spk_man : pwallet->GetAllScriptPubKeyMans()) {
            SignatureData sigdata;
            if (spk_man->CanProvide(scriptPubKeyKernel, sigdata)) {
                found_key = true;
                break;
            }
        }

        if (!found_key) {
            LogPrintf("CreateCoinStake : failed to get key for kernel type=%s\n", GetTxnOutputType(whichType));
            return false;
        }
        scriptPubKeyOut = scriptPubKeyKernel;
        return true;
    }

    // P2PK: verify we have the key
    // vSolutions[0] contains the pubkey
    CKeyID keyid = CPubKey(vSolutions[0]).GetID();
    bool found_key = false;

    // Try all ScriptPubKeyMans (supports both legacy and descriptor wallets)
    for (ScriptPubKeyMan* spk_man : pwallet->GetAllScriptPubKeyMans()) {
        if (auto* legacy = dynamic_cast<LegacyScriptPubKeyMan*>(spk_man)) {
            CKey key;
            if (legacy->GetKey(keyid, key)) {
                found_key = true;
                break;
            }
        } else if (auto* desc = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man)) {
            // For descriptor wallets, try with a P2PKH script since that's what they track
            CScript p2pkh_script = GetScriptForDestination(PKHash(keyid));
            CKey key;
            if (desc->GetKey(p2pkh_script, keyid, key)) {
                found_key = true;
                break;
            }
        }
    }

    if (!found_key) {
        LogPrintf("CreateCoinStake : failed to get key for kernel type=%s\n", GetTxnOutputType(whichType));
        return false;
    }
    scriptPubKeyOut = scriptPubKeyKernel;
    return true;
}

} // namespace

bool CollectStakeCandidates(const CWallet* pwallet, StakeCandidates& candidates)
{
    candidates = StakeCandidates{};

    LOCK(pwallet->cs_wallet);
    candidates.hashLastBlock = pwallet->GetLastBlockHash();
    if (gArgs.IsArgSet("-reservebalance") && !ParseMoney(gArgs.GetArg("-reservebalance", ""), candidates.nReserveBalance))
        return error("CreateCoinStake : invalid reserve balance amount");
    candidates.collected = true;

    std::vector<COutput> vAvailableCoins;
    CCoinControl temp;
    pwallet->AvailableCoins(vAvailableCoins, &temp);
    // For staking, use all available coins directly: SelectCoins applies
    // confirmation filters that reject recently-received coins, but staking
    // only needs any single UTXO with sufficient age to find a valid kernel.
    for (const COutput& coin : vAvailableCoins) {
        candidates.coins.insert(coin.GetInputCoin());
        candidates.nAvailable += coin.GetInputCoin().txout.nValue;
    }
    if (candidates.nAvailable <= candidates.nReserveBalance) {
        candidates.coins.clear();
        return false;
    }
    return !candidates.coins.empty();
}

bool SearchStakeKernel(const CWallet* pwallet, CChainState* chainstate, const StakeCandidates& candidates, unsigned int nBits, int64_t nSearchInterval, uint32_t nTimeTx, const Consensus::Params& consensusParams, StakeKernel& kernel, StakeWeightSummary* weight)
{
    // Every pass examines every stakeable coin, so it can report the stake
    // weight GetStakeWeight would compute, at no extra cost: the same value
    // and timestamp feed both. The GUI status and getstakinginfo read the
    // published result instead of rescanning the wallet themselves.
    StakeWeightSummary weight_summary;
    uint64_t nWeightCount = 0;
    const auto publish_weight = [&]() {
        if (!weight) return;
        if (nWeightCount > 0) weight_summary.average = weight_summary.total / nWeightCount;
        weight_summary.complete = true;
        *weight = weight_summary;
    };

    // The chain context for this pass, read once. The tip can move while the
    // pass runs; BuildCoinStake re-checks the kernel against the tip the
    // block is built on.
    bool fSegwitActive;
    int nSpendHeight;
    {
        LOCK(cs_main);
        const CBlockIndex* pindexTip = chainstate->m_chain.Tip();
        // Check if SegWit is active for the next block, needed to filter witness UTXOs
        fSegwitActive = DeploymentActiveAfter(pindexTip, consensusParams, Consensus::DEPLOYMENT_SEGWIT);
        // Height the coinstake being built would be spent at.
        nSpendHeight = pindexTip->nHeight + 1;
    }

    bool fKernelFound = false;
    for (const CInputCoin& pcoin : candidates.coins)
    {
        // Re-check maturity against the UTXO set rather than trusting the
        // wallet's cached confirmation depth. BlockDisconnected reaches the
        // wallet through the validation interface queue and nothing on the
        // staking path drains it first, so for a short window after a reorg
        // AvailableCoins still offers coinstake outputs that the rollback has
        // made immature again: their cached depth predates the disconnect.
        // Staking one produces a block that fails TestBlockValidity with
        // bad-txns-premature-spend-of-coinbase/coinstake, which is the same
        // rule CheckTxInputs applies. The chainstate is authoritative and
        // current, so ask it instead. Held for this lookup alone; the disk
        // read and the age arithmetic below need no lock.
        {
            LOCK(cs_main);
            const Coin& coin = chainstate->CoinsTip().AccessCoin(pcoin.outpoint);
            if (coin.IsSpent())
                continue;
            if ((coin.IsCoinBase() || coin.IsCoinStake()) &&
                nSpendHeight - coin.nHeight < consensusParams.GetCoinbaseMaturity())
                continue;
        }

        // Skip witness UTXOs if SegWit is not yet active
        if (!fSegwitActive && IsWitnessOutput(pcoin.txout.scriptPubKey))
            continue;

        CBlockHeader header;
        CTransactionRef tx;
        switch (ReadCoinSource(pcoin.outpoint, header, tx)) {
        case CoinSource::MISSING: continue;
        case CoinSource::FAILED: return false;
        case CoinSource::OK: break;
        }

        // Weight, before the age gate below: GetStakeWeight counts every coin
        // with positive weight, and a coin can carry a little while still
        // inside the search margin.
        {
            const unsigned int nTimeTxPrev = tx->nTime ? tx->nTime : header.GetBlockTime();
            const int64_t nTimeWeight = GetCoinAgeWeight((int64_t)nTimeTxPrev, (int64_t)nTimeTx, consensusParams);
            if (nTimeWeight > 0) {
                const arith_uint512 bnCoinDayWeight = arith_uint512(pcoin.txout.nValue) * nTimeWeight / COIN / (24 * 60 * 60);
                weight_summary.total += bnCoinDayWeight.GetLow64();
                nWeightCount++;
            }
        }

        // The search ends at the first kernel, but the weight must cover the
        // whole set: a wallet that finds a kernel on every pass, as a large
        // one does on a quiet network, would otherwise never publish. The
        // remaining coins are read once more here, which the combine loop
        // in BuildCoinStake does anyway on a pass that found a kernel.
        if (fKernelFound)
            continue;

        static const int nMaxStakeSearchInterval = 60;
        if (header.GetBlockTime() + consensusParams.nStakeMinAge > nTimeTx - nMaxStakeSearchInterval)
            continue; // only count coins meeting min age requirement

        // Search backward in time from the given timestamp, nSearchInterval
        // seconds back up to nMaxStakeSearchInterval. The kernel check reads
        // the tip and the block index, so cs_main is held for this coin's
        // checks and released before the next coin's disk read: that is what
        // lets the pass run while the node validates blocks and the GUI
        // reads the chain.
        bool foundStake = false;
        unsigned int nKernelOffset = 0;
        {
            LOCK(cs_main);
            // When creating a new stake block, use current chain tip as parent
            CBlockIndex* pindexPrev = chainstate->m_chain.Tip();
            for (unsigned int n = 0; n < std::min(nSearchInterval, (int64_t)nMaxStakeSearchInterval); n++)
            {
                uint256 hashProofOfStake = uint256();
                if (CheckStakeKernelHash(chainstate, pindexPrev, nBits, header, pcoin.outpoint.n, tx, pcoin.outpoint, nTimeTx - n, hashProofOfStake)) {
                    foundStake = true;
                    nKernelOffset = n;
                    break;
                }
            }
        }
        if (!foundStake)
            continue;

        // Found a kernel
        if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printcoinstake", DEFAULT_PRINTCOINSTAKE))
            LogPrintf("CreateCoinStake : kernel found\n");
        TxoutType whichType;
        CScript scriptPubKeyOut;
        if (!KernelOutputScript(pwallet, pcoin.txout.scriptPubKey, whichType, scriptPubKeyOut))
            continue; // unsupported type or no key for it: logged, and the search moves on as it always has

        kernel.outpoint = pcoin.outpoint;
        kernel.txout = pcoin.txout;
        kernel.header = header;
        kernel.txPrev = tx;
        kernel.nTime = nTimeTx - nKernelOffset;
        kernel.scriptPubKeyOut = scriptPubKeyOut;
        kernel.type = whichType;
        fKernelFound = true;
    }
    publish_weight();
    return fKernelFound;
}

bool BuildCoinStake(const CWallet* pwallet, CChainState* chainstate, unsigned int nBits, const StakeCandidates& candidates, const StakeKernel& kernel, CMutableTransaction& txNew, const Consensus::Params& consensusParams)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(pwallet->cs_wallet);

    CBlockIndex* pindexPrev = chainstate->m_chain.Tip();
    const int nSpendHeight = pindexPrev->nHeight + 1;

    // The kernel was found without the wallet lock and possibly against an
    // older tip. Anything that changed since costs this pass, never a block
    // the node would reject.
    if (pwallet->IsSpent(kernel.outpoint.hash, kernel.outpoint.n)) {
        LogPrintf("CreateCoinStake : kernel %s spent by the wallet since it was found\n", kernel.outpoint.ToString());
        return false;
    }
    {
        const Coin& coin = chainstate->CoinsTip().AccessCoin(kernel.outpoint);
        if (coin.IsSpent()) {
            LogPrintf("CreateCoinStake : kernel %s spent on the chain since it was found\n", kernel.outpoint.ToString());
            return false;
        }
        if ((coin.IsCoinBase() || coin.IsCoinStake()) && nSpendHeight - coin.nHeight < consensusParams.GetCoinbaseMaturity()) {
            LogPrintf("CreateCoinStake : kernel %s no longer mature at height %d\n", kernel.outpoint.ToString(), nSpendHeight);
            return false;
        }
    }
    {
        uint256 hashProofOfStake;
        if (!CheckStakeKernelHash(chainstate, pindexPrev, nBits, kernel.header, kernel.outpoint.n, kernel.txPrev, kernel.outpoint, kernel.nTime, hashProofOfStake)) {
            LogPrintf("CreateCoinStake : kernel %s no longer meets the target at the current tip\n", kernel.outpoint.ToString());
            return false;
        }
    }

    txNew.vin.clear();
    txNew.vout.clear();
    txNew.nVersion = POSV_TX_VERSION;   // Self-contained invariant: CreateCoinStake produces a v2 tx
    txNew.nTime = kernel.nTime;

    // Mark coin stake transaction
    CScript scriptEmpty;
    scriptEmpty.clear();
    txNew.vout.push_back(CTxOut(0, scriptEmpty));

    std::vector<CTransactionRef> vwtxPrev;
    CAmount nCredit = 0;
    const CScript& scriptPubKeyKernel = kernel.txout.scriptPubKey;

    txNew.vin.push_back(CTxIn(kernel.outpoint.hash, kernel.outpoint.n));
    nCredit += kernel.txout.nValue;
    vwtxPrev.push_back(kernel.txPrev);
    txNew.vout.push_back(CTxOut(0, kernel.scriptPubKeyOut));
    // Age the kernel from the UTXO Coin (single source of truth,
    // same source GetCoinAge/ConnectBlock use). Value is identical
    // to the disk header.GetBlockTime(); fall back to it defensively.
    uint32_t nKernelBlockTime = kernel.header.GetBlockTime();
    uint32_t nKernelTxPrevTime;
    GetCoinAgeTimes(chainstate, chainstate->CoinsTip(), kernel.outpoint, nKernelBlockTime, nKernelTxPrevTime);
    if (GetCoinAgeWeight(nKernelBlockTime, (int64_t)txNew.nTime, consensusParams) < nStakeSplitAge && nCredit >= nCombineThreshold)
        txNew.vout.push_back(CTxOut(0, kernel.scriptPubKeyOut)); // Split stake
    LogPrintf("CreateCoinStake : added kernel type=%s\n", GetTxnOutputType(kernel.type));

    if (nCredit == 0 || nCredit > candidates.nAvailable - candidates.nReserveBalance)
        return false;
    for (const CInputCoin& pcoin : candidates.coins)
    {
        // Attempt to add more inputs
        // Only add coins of the same key/address as kernel
        if (txNew.vout.size() == 2 && ((pcoin.txout.scriptPubKey == scriptPubKeyKernel || pcoin.txout.scriptPubKey == txNew.vout[1].scriptPubKey))
            && pcoin.outpoint.hash != txNew.vin[0].prevout.hash)
        {
            // The candidates were collected before the search; a coin the
            // wallet or the chain has spent since would invalidate the block.
            if (pwallet->IsSpent(pcoin.outpoint.hash, pcoin.outpoint.n) || chainstate->CoinsTip().AccessCoin(pcoin.outpoint).IsSpent())
                continue;
            CBlockHeader header;
            CTransactionRef tx;
            switch (ReadCoinSource(pcoin.outpoint, header, tx)) {
            case CoinSource::MISSING: continue;
            case CoinSource::FAILED: return false;
            case CoinSource::OK: break;
            }

            // Stop adding more inputs if already too many inputs
            if (txNew.vin.size() >= 100)
                break;
            // Stop adding more inputs if value is already pretty significant
            if (nCredit > nCombineThreshold)
                break;
            // Stop adding inputs if reached reserve limit
            if (nCredit + pcoin.txout.nValue > candidates.nAvailable - candidates.nReserveBalance)
                break;
            // Do not add additional significant input
            if (pcoin.txout.nValue > nCombineThreshold)
                continue;
            // Do not add input that is still too young. Age from the UTXO Coin
            // (single source of truth); raw coin.nTime is identical to the disk
            // tx->nTime, so behaviour is unchanged. Fall back defensively.
            uint32_t nCombineBlockTime, nCombineTxPrevTime = tx->nTime;
            GetCoinAgeTimes(chainstate, chainstate->CoinsTip(), pcoin.outpoint, nCombineBlockTime, nCombineTxPrevTime);
            if (nCombineTxPrevTime + consensusParams.nStakeMaxAge > txNew.nTime)
                continue;
            txNew.vin.push_back(CTxIn(pcoin.outpoint.hash, pcoin.outpoint.n));
            nCredit += pcoin.txout.nValue;
            vwtxPrev.push_back(tx);
        }
    }

    // Add Dev fund output
    txNew.vout.push_back(CTxOut(0, consensusParams.devScript.front()));
    CAmount nEndCredit = 0;
    CAmount nDevCredit = 0;

    // Calculate coin age reward
    {
        uint64_t nCoinAge = GetCoinAge(chainstate, (const CTransaction)txNew, consensusParams);
        CCoinsViewCache view(&chainstate->CoinsTip());
        if (!nCoinAge)
            return error("CreateCoinStake : failed to calculate coin age");

        double fInflationAdjustment = GetInflationAdjustment(chainstate, consensusParams);
        CAmount nReward = GetProofOfStakeReward(nCoinAge, 0 * COIN, fInflationAdjustment);

        // Refuse to create mint that has zero or negative reward
        if(nReward <= 0) {
          return false;
        }

        LogPrintf("nReward=%llu RDD\n", nReward);

        nEndCredit += nReward * 0.92;
        nDevCredit += nReward - nEndCredit;
        nCredit += nEndCredit;

        LogPrintf("nCredit=%llu RDD\n", nCredit);
    }

    CAmount nMinFee = 0;
    CAmount nMinFeeBase = MIN_TX_FEE;

    while(true)
    {
        // Set output amount
        if (txNew.vout.size() == 4)
        {
            txNew.vout[1].nValue = (nCredit / 2 / CENT) * CENT;
            txNew.vout[2].nValue = nCredit - txNew.vout[1].nValue;
            txNew.vout[3].nValue = nDevCredit;
        }
        else
        {
            txNew.vout[1].nValue = nCredit;
            txNew.vout[2].nValue = nDevCredit;
        }

        // Sign using wallet's SignTransaction (supports both legacy and descriptor wallets)
        std::map<COutPoint, Coin> coins;
        for (size_t i = 0; i < vwtxPrev.size(); ++i) {
            const CTxIn& txin = txNew.vin[i];
            const CTransactionRef& prevTx = vwtxPrev[i];
            coins[txin.prevout] = Coin(prevTx->vout[txin.prevout.n], 0, prevTx->IsCoinBase(), prevTx->IsCoinStake(), prevTx->nTime);
        }
        std::map<int, std::string> input_errors;
        if (!pwallet->SignTransaction(txNew, coins, SIGHASH_ALL, input_errors)) {
            for (const auto& err : input_errors) {
                LogPrintf("CreateCoinStake : sign error input %d: %s\n", err.first, err.second);
            }
            return error("CreateCoinStake : failed to sign coinstake");
        }

        // Limit size
        unsigned int nBytes = ::GetSerializeSize(txNew, PROTOCOL_VERSION);
        if (nBytes >= 1000000/5)
            return error("CreateCoinStake : exceeded coinstake size limit");

        // Check enough fee is paid
        if (nMinFee < GetMinFee(CTransaction(txNew)) - nMinFeeBase)
        {
            nMinFee = GetMinFee(CTransaction(txNew)) - nMinFeeBase;
            continue; // try signing again
        }
        else
        {
            if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printfee", false))
                LogPrintf("CreateCoinStake : fee for coinstake %s\n", FormatMoney(nMinFee).c_str());
            break;
        }
    }

    // Successfully generated coinstake
    return true;
}

// Reddcoin: create coin stake transaction
bool CreateCoinStake(const CWallet* pwallet, CChainState* chainstate, unsigned int nBits, int64_t nSearchInterval, CMutableTransaction& txNew, const Consensus::Params& consensusParams, StakeWeightSummary* weight)
{
    // Transaction index is required to get to block header
    if (!g_txindex)
        return error("CreateCoinStake : transaction index unavailable");

    LOCK2(cs_main, pwallet->cs_wallet);

    StakeCandidates candidates;
    CollectStakeCandidates(pwallet, candidates);
    if (!candidates.collected)
        return false; // the reserve balance setting could not be parsed

    // An empty set still runs the search, which reports a known zero weight.
    StakeKernel kernel;
    if (!SearchStakeKernel(pwallet, chainstate, candidates, nBits, nSearchInterval, txNew.nTime, consensusParams, kernel, weight))
        return false;
    return BuildCoinStake(pwallet, chainstate, nBits, candidates, kernel, txNew, consensusParams);
}

bool FinalizeCoinStakeReward(const CWallet* pwallet, CChainState* chainstate, CMutableTransaction& txCoinStake, const CAmount& nFees, const Consensus::Params& consensusParams)
{
    LOCK2(cs_main, pwallet->cs_wallet);

    // Recompute the stake reward WITH the block's transaction fees, exactly as
    // the validator does in ConnectBlock:
    //   nCalculatedStakeReward = GetProofOfStakeReward(nCoinAge, nFees, fInflationAdjustment)
    // Coin age is read from the same UTXO source (CoinsTip) the validator uses,
    // over the final coinstake inputs (still unspent at the tip during mining).
    uint64_t nCoinAge = GetCoinAge(chainstate, (const CTransaction)txCoinStake, consensusParams);
    if (!nCoinAge)
        return error("FinalizeCoinStakeReward : failed to calculate coin age");

    double fInflationAdjustment = GetInflationAdjustment(chainstate, consensusParams);
    CAmount nReward = GetProofOfStakeReward(nCoinAge, nFees, fInflationAdjustment);
    if (nReward <= 0)
        return error("FinalizeCoinStakeReward : non-positive reward");

    // Same truncating 92/8 split as CreateCoinStake and ConnectBlock, so the dev
    // output equals the validator's nCalculatedDevEndCredit exactly.
    CAmount nEndCredit = nReward * 0.92;
    CAmount nDevCredit = nReward - nEndCredit;

    // Sum the staked inputs from the UTXO set to recompute the staker credit.
    CCoinsViewCache& view = chainstate->CoinsTip();
    CAmount nCredit = nEndCredit;
    for (const CTxIn& txin : txCoinStake.vin) {
        const Coin& coin = view.AccessCoin(txin.prevout);
        if (coin.IsSpent())
            return error("FinalizeCoinStakeReward : coinstake input not available");
        nCredit += coin.out.nValue;
    }

    // Rewrite outputs using the existing coinstake layout (dev output is last).
    if (txCoinStake.vout.size() == 4) {
        txCoinStake.vout[1].nValue = (nCredit / 2 / CENT) * CENT;
        txCoinStake.vout[2].nValue = nCredit - txCoinStake.vout[1].nValue;
        txCoinStake.vout[3].nValue = nDevCredit;
    } else {
        txCoinStake.vout[1].nValue = nCredit;
        txCoinStake.vout[2].nValue = nDevCredit;
    }

    // Re-sign over the new output amounts. Build the prevout map from the UTXO
    // Coins (they carry the correct nTime / coinbase / coinstake flags).
    std::map<COutPoint, Coin> coins;
    for (const CTxIn& txin : txCoinStake.vin)
        coins[txin.prevout] = view.AccessCoin(txin.prevout);
    std::map<int, std::string> input_errors;
    if (!pwallet->SignTransaction(txCoinStake, coins, SIGHASH_ALL, input_errors)) {
        for (const auto& err : input_errors)
            LogPrintf("FinalizeCoinStakeReward : sign error input %d: %s\n", err.first, err.second);
        return error("FinalizeCoinStakeReward : failed to sign coinstake");
    }

    unsigned int nBytes = ::GetSerializeSize(txCoinStake, PROTOCOL_VERSION);
    if (nBytes >= 1000000 / 5)
        return error("FinalizeCoinStakeReward : exceeded coinstake size limit");

    return true;
}

bool SignBlock(CBlock& block, const CWallet& keystore)
{
    std::vector<valtype> vSolutions;
    const CTxOut& txout = block.IsProofOfStake() ? block.vtx[1]->vout[1] : block.vtx[0]->vout[0];
    TxoutType whichType = Solver(txout.scriptPubKey, vSolutions);

    // Taproot coinstake output: BIP340 Schnorr-sign the block with the key-path
    // key. Only descriptor wallets can hold taproot keys.
    if (whichType == TxoutType::WITNESS_V1_TAPROOT) {
        for (ScriptPubKeyMan* spk_man : keystore.GetAllScriptPubKeyMans()) {
            if (auto* desc = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man)) {
                if (desc->SignBlockSchnorr(txout.scriptPubKey, block.GetHash(), block.vchBlockSig)) {
                    return true;
                }
            }
        }
        return false;
    }

    if (whichType != TxoutType::PUBKEY) {
        return false;
    }

    const valtype& vchPubKey = vSolutions[0];
    CKeyID keyid(Hash160(vchPubKey));

    CKey key;
    bool found_key = false;

    // Try all ScriptPubKeyMans (supports both legacy and descriptor wallets)
    // Note: For P2PK scripts, CanProvide may return false for descriptor wallets
    // because the wallet tracks P2PKH scripts, not P2PK. So we try GetKey directly
    // by keyid without relying on CanProvide for descriptor wallets.
    for (ScriptPubKeyMan* spk_man : keystore.GetAllScriptPubKeyMans()) {
        if (auto* legacy = dynamic_cast<LegacyScriptPubKeyMan*>(spk_man)) {
            if (legacy->GetKey(keyid, key)) {
                found_key = true;
                break;
            }
        } else if (auto* desc = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man)) {
            // The coinstake output is P2PK, but the descriptor wallet indexes keys
            // by the original script type. Try P2PKH first, then P2WPKH, since
            // the staking UTXO could have been either address type.
            CScript p2pkh_script = GetScriptForDestination(PKHash(keyid));
            if (desc->GetKey(p2pkh_script, keyid, key)) {
                found_key = true;
                break;
            }
            CScript p2wpkh_script = GetScriptForDestination(WitnessV0KeyHash(keyid));
            if (desc->GetKey(p2wpkh_script, keyid, key)) {
                found_key = true;
                break;
            }
        }
    }

    if (!found_key) {
        return false;
    }

    if (key.GetPubKey() != CPubKey(vchPubKey)) {
        return false;
    }

    return key.Sign(block.GetHash(), block.vchBlockSig, 0);
}

namespace {

//! Holds cs_wallet for the caller, see interfaces::StakingWallet::Lock.
//!
//! UniqueLock is SCOPED_LOCKABLE, so clang's thread-safety analysis expects it
//! to be released in the scope that acquired it. Here the whole point is to hand
//! the lock across an interface boundary and let the caller decide the scope, so
//! the analysis cannot follow it and is disabled for the acquire and release.
//! UniqueLock is still used rather than a raw std::unique_lock so the runtime
//! DEBUG_LOCKORDER checks continue to see this acquisition, which is what
//! enforces the cs_wallet-before-cs_main order on the staking path.
class StakingWalletLock : public interfaces::StakingWallet::Lock
{
public:
    explicit StakingWalletLock(RecursiveMutex& mutex) NO_THREAD_SAFETY_ANALYSIS
        : m_lock(mutex, "cs_wallet", __FILE__, __LINE__) {}

    ~StakingWalletLock() NO_THREAD_SAFETY_ANALYSIS {}

private:
    UniqueLock<RecursiveMutex> m_lock;
};

//! interfaces::StakingWallet over a loaded CWallet.
class StakingWalletImpl : public interfaces::StakingWallet
{
public:
    explicit StakingWalletImpl(const std::shared_ptr<CWallet>& wallet) : m_wallet(wallet) {}

    std::unique_ptr<Lock> lock() override
    {
        return std::make_unique<StakingWalletLock>(m_wallet->cs_wallet);
    }

    std::string getName() const override { return m_wallet->GetName(); }
    bool isLocked() const override { return m_wallet->IsLocked(); }
    bool getEnableStaking() const override { return m_wallet->GetEnableStaking(); }
    void setEnableStaking(bool enable) override { m_wallet->SetEnableStaking(enable); }

    bool canStake(std::string& reason) override
    {
        if (m_wallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
            reason = "Disable private keys flag set";
            return false;
        }
        if (m_wallet->IsWalletFlagSet(WALLET_FLAG_BLANK_WALLET)) {
            reason = "Blank wallet flag set";
            return false;
        }
        return true;
    }

    void notifyStakingStatusChanged() override { m_wallet->NotifyWalletStakingStatusChanged(); }

    std::unique_ptr<interfaces::Handler> handleUnload(UnloadFn fn) override
    {
        return interfaces::MakeHandler(m_wallet->NotifyUnload.connect(fn));
    }

    bool isUnloading() const override { return m_wallet->IsUnloading(); }

    void setLastCoinStakeSearchInterval(int64_t interval) override
    {
        m_wallet->SetLastCoinStakeSearchInterval(interval);
    }

    void blockUntilSyncedToCurrentChain() override { m_wallet->BlockUntilSyncedToCurrentChain(); }
    int64_t getLastCoinStakeSearchInterval() override { return m_wallet->GetLastCoinStakeSearchInterval(); }

    bool getStakeWeight(uint64_t& average_weight, uint64_t& total_weight) override
    {
        // While the staking thread runs it publishes the weight of the coins
        // it examined on its last complete pass; read that rather than
        // rescanning the wallet under cs_wallet, which on a large wallet
        // takes seconds per call and stalls everything else that needs the
        // wallet. A wallet that is not staking has nothing published, so
        // compute it for that case.
        if (m_wallet->GetPublishedStakeWeight(average_weight, total_weight)) return true;
        return GetStakeWeight(m_wallet.get(), average_weight, total_weight, Params().GetConsensus());
    }

    void resetPublishedStakeWeight() override { m_wallet->ResetPublishedStakeWeight(); }

    bool reserveDestination(CTxDestination& dest, std::string& error) override
    {
        if (!m_reservedest) {
            m_reservedest = std::make_unique<ReserveDestination>(m_wallet.get(), m_wallet->m_default_address_type);
        }
        LOCK(m_wallet->cs_wallet);
        return m_reservedest->GetReservedDestination(dest, true, error);
    }

    void keepDestination() override
    {
        if (m_reservedest) m_reservedest->KeepDestination();
    }

    bool collectStakeCandidates() override
    {
        return CollectStakeCandidates(m_wallet.get(), m_candidates);
    }

    bool stakeCandidatesCurrent() override
    {
        if (!m_candidates.collected) return false;
        return WITH_LOCK(m_wallet->cs_wallet, return m_wallet->GetLastBlockHash()) == m_candidates.hashLastBlock;
    }

    size_t stakeCandidateCount() override { return m_candidates.coins.size(); }

    bool searchStakeKernel(CChainState& chainstate,
        unsigned int nBits,
        int64_t nSearchInterval,
        uint32_t nTimeTx,
        const Consensus::Params& consensus_params) override
    {
        // Publish what the pass measured, so the GUI and getstakinginfo need
        // not rescan the wallet. A pass that failed before it could examine
        // the set leaves the previous value standing.
        StakeWeightSummary weight;
        m_kernel_found = SearchStakeKernel(m_wallet.get(), &chainstate, m_candidates, nBits, nSearchInterval, nTimeTx, consensus_params, m_kernel, &weight);
        if (weight.complete) m_wallet->PublishStakeWeight(weight.average, weight.total);
        return m_kernel_found;
    }

    bool buildCoinStake(CChainState& chainstate,
        unsigned int nBits,
        CMutableTransaction& tx_new,
        const Consensus::Params& consensus_params) override NO_THREAD_SAFETY_ANALYSIS
    {
        if (!m_kernel_found) return false;
        m_kernel_found = false;
        if (BuildCoinStake(m_wallet.get(), &chainstate, nBits, m_candidates, m_kernel, tx_new, consensus_params)) return true;
        // The kernel did not survive the re-check under the locks, most often
        // because the wallet spent the coin since the candidates were
        // collected. Have the next pass collect again rather than wait for a
        // block to move the wallet's view.
        m_candidates.collected = false;
        return false;
    }

    bool createCoinStake(CChainState& chainstate,
        unsigned int nBits,
        int64_t nSearchInterval,
        CMutableTransaction& tx_new,
        const Consensus::Params& consensus_params) override
    {
        // Publish what the pass measured, so the GUI and getstakinginfo need
        // not rescan the wallet. A pass that failed before it could examine
        // the set leaves the previous value standing.
        StakeWeightSummary weight;
        const bool found = CreateCoinStake(m_wallet.get(), &chainstate, nBits, nSearchInterval, tx_new, consensus_params, &weight);
        if (weight.complete) m_wallet->PublishStakeWeight(weight.average, weight.total);
        return found;
    }

    bool finalizeCoinStakeReward(CChainState& chainstate,
        CMutableTransaction& tx_coinstake,
        const CAmount& fees,
        const Consensus::Params& consensus_params) override
    {
        return FinalizeCoinStakeReward(m_wallet.get(), &chainstate, tx_coinstake, fees, consensus_params);
    }

    bool signBlock(CBlock& block) override { return SignBlock(block, *m_wallet); }

private:
    std::shared_ptr<CWallet> m_wallet;
    //! Destination reserved for the coinstake, released unless keepDestination()
    //! is called. Held for this object's lifetime, matching the ReserveDestination
    //! that PoSMiner used to keep on its stack for the whole staking loop.
    std::unique_ptr<ReserveDestination> m_reservedest;
    //! The coins the staking thread searches, collected under cs_wallet once
    //! per block, and the kernel its last search found, handed to the block
    //! assembler through buildCoinStake(). Owned here rather than by the
    //! thread so that libbitcoin_server never names a wallet type.
    StakeCandidates m_candidates;
    StakeKernel m_kernel;
    bool m_kernel_found{false};
};

//! interfaces::StakingSupport over the process's loaded wallets.
class StakingSupportImpl : public interfaces::StakingSupport
{
public:
    std::vector<std::unique_ptr<interfaces::StakingWallet>> getStakingWallets() override
    {
        std::vector<std::unique_ptr<interfaces::StakingWallet>> result;
        for (const std::shared_ptr<CWallet>& wallet : GetWallets()) {
            result.push_back(MakeStakingWallet(wallet));
        }
        return result;
    }

    std::unique_ptr<interfaces::StakingWallet> getStakingWallet(const std::string& name) override
    {
        return MakeStakingWallet(GetWallet(name));
    }

    std::unique_ptr<interfaces::StakingWallet> getStakingWalletForRequest(const JSONRPCRequest& request) override
    {
        return MakeStakingWallet(GetWalletForJSONRPCRequest(request));
    }
};

} // namespace

std::unique_ptr<interfaces::StakingWallet> MakeStakingWallet(const std::shared_ptr<CWallet>& wallet)
{
    if (!wallet) return nullptr;
    return std::make_unique<StakingWalletImpl>(wallet);
}

interfaces::StakingSupport& GetWalletStakingSupport()
{
    static StakingSupportImpl support;
    return support;
}
