// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/stake.h>

#include <chainparams.h>
#include <consensus/tx_verify.h>
#include <index/disktxpos.h>
#include <index/txindex.h>
#include <node/blockstorage.h>
#include <pos/kernel.h>
#include <wallet/coincontrol.h>

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

typedef std::vector<unsigned char> valtype;

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

} // namespace

bool CollectStakeCandidates(const CWallet* pwallet, StakeCandidates& candidates)
{
    candidates = StakeCandidates{};

    LOCK(pwallet->cs_wallet);
    candidates.hashLastBlock = pwallet->GetLastBlockHash();
    candidates.nBalance = pwallet->GetBalance().m_mine_trusted;
    if (gArgs.IsArgSet("-reservebalance") && !ParseMoney(gArgs.GetArg("-reservebalance", ""), candidates.nReserveBalance))
        return error("CreateCoinStake : invalid reserve balance amount");
    candidates.collected = true;
    if (candidates.nBalance <= candidates.nReserveBalance)
        return false;

    std::vector<COutput> vAvailableCoins;
    CCoinControl temp;
    CoinSelectionParams coin_selection_params;
    CAmount nValueIn = 0;
    pwallet->AvailableCoins(vAvailableCoins, &temp);
    if (!pwallet->SelectCoins(vAvailableCoins, candidates.nBalance - candidates.nReserveBalance, candidates.coins, nValueIn, temp, coin_selection_params)) {
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

    bool fKernelFound = false;
    for (const CInputCoin& pcoin : candidates.coins)
    {
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

        for (unsigned int n=0; n<std::min(nSearchInterval,(int64_t)nMaxStakeSearchInterval) && !fKernelFound; n++)
        {
            // Search backward in time from the given txNew timestamp
            // Search nSearchInterval seconds back up to nMaxStakeSearchInterval
            uint256 hashProofOfStake = uint256();
            bool foundStake;
            {
                // The kernel check reads the tip and the block index. Hold
                // cs_main for the check alone, not for the pass: this is
                // what lets the search run while the node validates blocks
                // and the GUI reads the chain.
                LOCK(cs_main);
                foundStake = CheckStakeKernelHash(chainstate, nBits, header, pcoin.outpoint.n, tx, pcoin.outpoint, nTimeTx - n, hashProofOfStake);
            }
            if (foundStake)
            {
                // Found a kernel
                if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printcoinstake", DEFAULT_PRINTCOINSTAKE))
                    LogPrintf("CreateCoinStake : kernel found\n");
                std::vector<valtype> vSolutions;
                CScript scriptPubKeyOut;
                const CScript& scriptPubKeyKernel = pcoin.txout.scriptPubKey;
                TxoutType whichType = Solver(scriptPubKeyKernel, vSolutions);
                if (whichType != TxoutType::PUBKEY && whichType != TxoutType::PUBKEYHASH && whichType != TxoutType::WITNESS_V0_KEYHASH) {
                    LogPrintf("CreateCoinStake : no support for kernel type=%s\n", GetTxnOutputType(whichType));
                    break;
                }
                if (whichType == TxoutType::PUBKEYHASH || whichType == TxoutType::WITNESS_V0_KEYHASH) // pay to address type or witness keyhash
                {
                    // convert to pay to public key type
                    CKey key;
                    bool have_key;
                    {
                        // The key store is the only wallet state this
                        // needs; take the wallet lock for the lookup alone.
                        LOCK(pwallet->cs_wallet);
                        have_key = pwallet->GetLegacyScriptPubKeyMan()->GetKey(CKeyID(uint160(vSolutions[0])), key);
                    }
                    if (!have_key) {
                        LogPrintf("CreateCoinStake : failed to get key for kernel type=%s\n", GetTxnOutputType(whichType));
                        break;
                    }
                    scriptPubKeyOut << ToByteVector(key.GetPubKey()) << OP_CHECKSIG;
                }
                else
                    scriptPubKeyOut = scriptPubKeyKernel;

                kernel.outpoint = pcoin.outpoint;
                kernel.txout = pcoin.txout;
                kernel.header = header;
                kernel.txPrev = tx;
                kernel.nTime = nTimeTx - n;
                kernel.scriptPubKeyOut = scriptPubKeyOut;
                kernel.type = whichType;
                fKernelFound = true;
                break;
            }
        }
    }
    publish_weight();
    return fKernelFound;
}

bool BuildCoinStake(const CWallet* pwallet, CChainState* chainstate, unsigned int nBits, const StakeCandidates& candidates, const StakeKernel& kernel, CMutableTransaction& txNew, const Consensus::Params& consensusParams)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(pwallet->cs_wallet);

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
        const int nSpendHeight = chainstate->m_chain.Tip()->nHeight + 1;
        if ((coin.IsCoinBase() || coin.IsCoinStake()) && nSpendHeight - coin.nHeight < consensusParams.GetCoinbaseMaturity()) {
            LogPrintf("CreateCoinStake : kernel %s no longer mature at height %d\n", kernel.outpoint.ToString(), nSpendHeight);
            return false;
        }
    }
    {
        uint256 hashProofOfStake;
        if (!CheckStakeKernelHash(chainstate, nBits, kernel.header, kernel.outpoint.n, kernel.txPrev, kernel.outpoint, kernel.nTime, hashProofOfStake)) {
            LogPrintf("CreateCoinStake : kernel %s no longer meets the target at the current tip\n", kernel.outpoint.ToString());
            return false;
        }
    }

    txNew.vin.clear();
    txNew.vout.clear();
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
    if (GetCoinAgeWeight(kernel.header.GetBlockTime(), (int64_t)txNew.nTime, consensusParams) < nStakeSplitAge && nCredit >= nCombineThreshold)
        txNew.vout.push_back(CTxOut(0, kernel.scriptPubKeyOut)); // Split stake
    LogPrintf("CreateCoinStake : added kernel type=%s\n", GetTxnOutputType(kernel.type));

    if (nCredit == 0 || nCredit > candidates.nBalance - candidates.nReserveBalance)
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
            if (nCredit + pcoin.txout.nValue > candidates.nBalance - candidates.nReserveBalance)
                break;
            // Do not add additional significant input
            if (pcoin.txout.nValue > nCombineThreshold)
                continue;
            // Do not add input that is still too young
            if (tx->nTime + consensusParams.nStakeMaxAge > txNew.nTime)
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

        // Sign
        int nIn = 0;
        for (const auto& pcoin : vwtxPrev)
        {
            if (!SignSignature(*pwallet->GetLegacyScriptPubKeyMan(), *pcoin, txNew, nIn++, SIGHASH_ALL))
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
