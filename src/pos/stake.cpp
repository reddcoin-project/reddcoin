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

// Reddcoin: create coin stake transaction
typedef std::vector<unsigned char> valtype;
bool CreateCoinStake(const CWallet* pwallet, CChainState* chainstate, unsigned int nBits, int64_t nSearchInterval, CMutableTransaction& txNew, const Consensus::Params& consensusParams, StakeWeightSummary* weight)
{
    // The following split & combine thresholds are important to security
    // Should not be adjusted if you don't understand the consequences
    static unsigned int nStakeSplitAge = (60 * 60 * 24 * 45);
    int64_t nCombineThreshold = 2000000 * COIN;

    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);

    // Transaction index is required to get to block header
    if (!g_txindex)
        return error("CreateCoinStake : transaction index unavailable");

    LOCK2(cs_main, pwallet->cs_wallet);
    txNew.vin.clear();
    txNew.vout.clear();

    // Mark coin stake transaction
    CScript scriptEmpty;
    scriptEmpty.clear();
    txNew.vout.push_back(CTxOut(0, scriptEmpty));

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

    // Choose coins to use
    CAmount nBalance = pwallet->GetBalance().m_mine_trusted;
    CAmount nReserveBalance = 0;
    if (gArgs.IsArgSet("-reservebalance") && !ParseMoney(gArgs.GetArg("-reservebalance", ""), nReserveBalance))
        return error("CreateCoinStake : invalid reserve balance amount");
    if (nBalance <= nReserveBalance) {
        publish_weight(); // nothing stakeable: zero weight, and known to be zero
        return false;
    }
    std::set<CInputCoin> setCoins;
    std::vector<CTransactionRef> vwtxPrev;
    CAmount nValueIn = 0;
    std::vector<COutput> vAvailableCoins;
    CCoinControl temp;
    CoinSelectionParams coin_selection_params;
    pwallet->AvailableCoins(vAvailableCoins, &temp);
    if (!pwallet->SelectCoins(vAvailableCoins, nBalance - nReserveBalance, setCoins, nValueIn, temp, coin_selection_params)) {
        publish_weight();
        return false;
    }
    if (setCoins.empty()) {
        publish_weight();
        return false;
    }
    CAmount nCredit = 0;
    CScript scriptPubKeyKernel;
    bool fKernelFound = false;
    for (const auto& pcoin : setCoins)
    {
        CDiskTxPos postx;
        if (!g_txindex->FindTxPosition(pcoin.outpoint.hash, postx))
            continue;

        // Read block header
        CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
        CBlockHeader header;
        CTransactionRef tx;
        try {
            file >> header;
            fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
            file >> tx;
        } catch (std::exception &e) {
            return error("%s() : deserialize or I/O error in CreateCoinStake()", __PRETTY_FUNCTION__);
        }

        // Weight, before the age gate below: GetStakeWeight counts every coin
        // with positive weight, and a coin can carry a little while still
        // inside the search margin.
        {
            const unsigned int nTimeTx = tx->nTime ? tx->nTime : header.GetBlockTime();
            const int64_t nTimeWeight = GetCoinAgeWeight((int64_t)nTimeTx, (int64_t)txNew.nTime, consensusParams);
            if (nTimeWeight > 0) {
                const arith_uint512 bnCoinDayWeight = arith_uint512(pcoin.txout.nValue) * nTimeWeight / COIN / (24 * 60 * 60);
                weight_summary.total += bnCoinDayWeight.GetLow64();
                nWeightCount++;
            }
        }

        static int nMaxStakeSearchInterval = 60;
        if (header.GetBlockTime() + consensusParams.nStakeMinAge > txNew.nTime - nMaxStakeSearchInterval)
            continue; // only count coins meeting min age requirement

        for (unsigned int n=0; n<std::min(nSearchInterval,(int64_t)nMaxStakeSearchInterval) && !fKernelFound; n++)
        {
            // Search backward in time from the given txNew timestamp
            // Search nSearchInterval seconds back up to nMaxStakeSearchInterval
            uint256 hashProofOfStake = uint256();
            COutPoint prevoutStake = pcoin.outpoint;
            bool foundStake = CheckStakeKernelHash(chainstate, nBits, header, prevoutStake.n, tx, prevoutStake, txNew.nTime - n, hashProofOfStake);
            if (foundStake)
            {
                // Found a kernel
                if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printcoinstake", DEFAULT_PRINTCOINSTAKE))
                    LogPrintf("CreateCoinStake : kernel found\n");
                std::vector<valtype> vSolutions;
                CScript scriptPubKeyOut;
                scriptPubKeyKernel = pcoin.txout.scriptPubKey;
                TxoutType whichType = Solver(scriptPubKeyKernel, vSolutions);
                if (whichType != TxoutType::PUBKEY && whichType != TxoutType::PUBKEYHASH && whichType != TxoutType::WITNESS_V0_KEYHASH) {
                    LogPrintf("CreateCoinStake : no support for kernel type=%s\n", GetTxnOutputType(whichType));
                    break;
                }
                if (whichType == TxoutType::PUBKEYHASH || whichType == TxoutType::WITNESS_V0_KEYHASH) // pay to address type or witness keyhash
                {
                    // convert to pay to public key type
                    CKey key;
                    if (!pwallet->GetLegacyScriptPubKeyMan()->GetKey(CKeyID(uint160(vSolutions[0])), key)) {
                        LogPrintf("CreateCoinStake : failed to get key for kernel type=%s\n", GetTxnOutputType(whichType));
                        break;
                    }
                    scriptPubKeyOut << ToByteVector(key.GetPubKey()) << OP_CHECKSIG;
                }
                else
                    scriptPubKeyOut = scriptPubKeyKernel;

                txNew.nTime -= n;
                txNew.vin.push_back(CTxIn(pcoin.outpoint.hash, pcoin.outpoint.n));
                nCredit += pcoin.txout.nValue;
                vwtxPrev.push_back(tx);
                txNew.vout.push_back(CTxOut(0, scriptPubKeyOut));
                if (GetCoinAgeWeight(header.GetBlockTime(), (int64_t)txNew.nTime, consensusParams) < nStakeSplitAge && nCredit >= nCombineThreshold)
                    txNew.vout.push_back(CTxOut(0, scriptPubKeyOut)); // Split stake
                LogPrintf("CreateCoinStake : added kernel type=%s\n", GetTxnOutputType(whichType));
                fKernelFound = true;
                break;
            }
        }
        if (fKernelFound)
            break; // if kernel is found stop searching
    }
    // A pass that stopped at a kernel saw only part of the set; leave the
    // previously published weight standing rather than report a partial sum.
    if (!fKernelFound)
        publish_weight();
    if (nCredit == 0 || nCredit > nBalance - nReserveBalance)
        return false;
    for (const auto& pcoin : setCoins)
    {
        CDiskTxPos postx;
        if (!g_txindex->FindTxPosition(pcoin.outpoint.hash, postx))
            continue;

        // Read block header
        CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
        CBlockHeader header;
        CTransactionRef tx;
        try {
            file >> header;
            fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
            file >> tx;
        } catch (std::exception &e) {
            return error("%s() : deserialize or I/O error in CreateCoinStake()", __PRETTY_FUNCTION__);
        }


        // Attempt to add more inputs
        // Only add coins of the same key/address as kernel
        if (txNew.vout.size() == 2 && ((pcoin.txout.scriptPubKey == scriptPubKeyKernel || pcoin.txout.scriptPubKey == txNew.vout[1].scriptPubKey))
            && pcoin.outpoint.hash != txNew.vin[0].prevout.hash)
        {
            // Stop adding more inputs if already too many inputs
            if (txNew.vin.size() >= 100)
                break;
            // Stop adding more inputs if value is already pretty significant
            if (nCredit > nCombineThreshold)
                break;
            // Stop adding inputs if reached reserve limit
            if (nCredit + pcoin.txout.nValue > nBalance - nReserveBalance)
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
