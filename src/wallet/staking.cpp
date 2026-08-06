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

// Reddcoin: create coin stake transaction
bool CreateCoinStake(const CWallet* pwallet, CChainState* chainstate, unsigned int nBits, int64_t nSearchInterval, CMutableTransaction& txNew, const Consensus::Params& consensusParams)
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
    txNew.nVersion = POSV_TX_VERSION;   // Self-contained invariant: CreateCoinStake produces a v2 tx

    // Mark coin stake transaction
    CScript scriptEmpty;
    scriptEmpty.clear();
    txNew.vout.push_back(CTxOut(0, scriptEmpty));

    // Choose coins to use
    CAmount nReserveBalance = 0;
    if (gArgs.IsArgSet("-reservebalance") && !ParseMoney(gArgs.GetArg("-reservebalance", ""), nReserveBalance))
        return error("CreateCoinStake : invalid reserve balance amount");
    std::set<CInputCoin> setCoins;
    std::vector<CTransactionRef> vwtxPrev;
    CAmount nValueIn = 0;
    std::vector<COutput> vAvailableCoins;
    CCoinControl temp;
    CoinSelectionParams coin_selection_params;
    pwallet->AvailableCoins(vAvailableCoins, &temp);
    // For staking, use all available coins directly — SelectCoins applies
    // confirmation filters that reject recently-received coins, but staking
    // only needs any single UTXO with sufficient age to find a valid kernel.
    CAmount nAvailable = 0;
    for (const auto& coin : vAvailableCoins) {
        setCoins.insert(coin.GetInputCoin());
        nAvailable += coin.GetInputCoin().txout.nValue;
    }
    nValueIn = nAvailable;
    if (nAvailable <= nReserveBalance)
        return false;
    if (setCoins.empty())
        return false;
    CAmount nCredit = 0;
    CScript scriptPubKeyKernel;
    // Check if SegWit is active for the next block — needed to filter witness UTXOs
    CBlockIndex* pindexTip = chainstate->m_chain.Tip();
    bool fSegwitActive = DeploymentActiveAfter(pindexTip, consensusParams, Consensus::DEPLOYMENT_SEGWIT);

    // Height the coinstake being built would be spent at.
    const int nSpendHeight = pindexTip->nHeight + 1;

    for (const auto& pcoin : setCoins)
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
        // current, so ask it instead.
        //
        // Background staking runs off its own thread, so draining the queue in
        // the RPC path alone would not cover this.
        const Coin& coin = chainstate->CoinsTip().AccessCoin(pcoin.outpoint);
        if (coin.IsSpent())
            continue;
        if ((coin.IsCoinBase() || coin.IsCoinStake()) &&
            nSpendHeight - coin.nHeight < consensusParams.GetCoinbaseMaturity())
            continue;

        // Skip witness UTXOs if SegWit is not yet active — including them in a
        // coinstake would produce a block with witness data that gets rejected
        // with "unexpected witness data" during ContextualCheckBlock.
        if (!fSegwitActive) {
            std::vector<std::vector<unsigned char>> vSolutions;
            TxoutType type = Solver(pcoin.txout.scriptPubKey, vSolutions);
            if (type == TxoutType::WITNESS_V0_KEYHASH ||
                type == TxoutType::WITNESS_V0_SCRIPTHASH ||
                type == TxoutType::WITNESS_V1_TAPROOT ||
                type == TxoutType::WITNESS_UNKNOWN) {
                continue;
            }
        }

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

        static int nMaxStakeSearchInterval = 60;
        if (header.GetBlockTime() + consensusParams.nStakeMinAge > txNew.nTime - nMaxStakeSearchInterval)
            continue; // only count coins meeting min age requirement

        bool fKernelFound = false;
        for (unsigned int n=0; n<std::min(nSearchInterval,(int64_t)nMaxStakeSearchInterval) && !fKernelFound; n++)
        {
            // Search backward in time from the given txNew timestamp
            // Search nSearchInterval seconds back up to nMaxStakeSearchInterval
            uint256 hashProofOfStake = uint256();
            COutPoint prevoutStake = pcoin.outpoint;
            // When creating a new stake block, use current chain tip as parent
            CBlockIndex* pindexPrev = chainstate->m_chain.Tip();
            bool foundStake = CheckStakeKernelHash(chainstate, pindexPrev, nBits, header, prevoutStake.n, tx, prevoutStake, txNew.nTime - n, hashProofOfStake);
            if (foundStake)
            {
                // Found a kernel
                if (gArgs.GetBoolArg("-debug", false) && gArgs.GetBoolArg("-printcoinstake", DEFAULT_PRINTCOINSTAKE))
                    LogPrintf("CreateCoinStake : kernel found\n");
                std::vector<valtype> vSolutions;
                CScript scriptPubKeyOut;
                scriptPubKeyKernel = pcoin.txout.scriptPubKey;
                TxoutType whichType = Solver(scriptPubKeyKernel, vSolutions);
                if (whichType != TxoutType::PUBKEY &&
                    whichType != TxoutType::PUBKEYHASH &&
                    whichType != TxoutType::WITNESS_V0_KEYHASH &&
                    whichType != TxoutType::WITNESS_V1_TAPROOT) {
                    LogPrintf("CreateCoinStake : no support for kernel type=%s\n", GetTxnOutputType(whichType));
                    break;
                }
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
                        break;
                    }
                    scriptPubKeyOut << ToByteVector(key.GetPubKey()) << OP_CHECKSIG;
                }
                else if (whichType == TxoutType::WITNESS_V1_TAPROOT)
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
                        break;
                    }
                    scriptPubKeyOut = scriptPubKeyKernel;
                }
                else if (whichType == TxoutType::PUBKEY)
                {
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
                        break;
                    }
                    scriptPubKeyOut = scriptPubKeyKernel;
                }

                txNew.nTime -= n;
                txNew.vin.push_back(CTxIn(pcoin.outpoint.hash, pcoin.outpoint.n));
                nCredit += pcoin.txout.nValue;
                vwtxPrev.push_back(tx);
                txNew.vout.push_back(CTxOut(0, scriptPubKeyOut));
                // Age the kernel from the UTXO Coin (single source of truth,
                // same source GetCoinAge/ConnectBlock use). Value is identical
                // to the disk header.GetBlockTime(); fall back to it defensively.
                uint32_t nKernelBlockTime = header.GetBlockTime();
                uint32_t nKernelTxPrevTime;
                GetCoinAgeTimes(chainstate, chainstate->CoinsTip(), pcoin.outpoint, nKernelBlockTime, nKernelTxPrevTime);
                if (GetCoinAgeWeight(nKernelBlockTime, (int64_t)txNew.nTime, consensusParams) < nStakeSplitAge && nCredit >= nCombineThreshold)
                    txNew.vout.push_back(CTxOut(0, scriptPubKeyOut)); // Split stake
                LogPrintf("CreateCoinStake : added kernel type=%s\n", GetTxnOutputType(whichType));
                fKernelFound = true;
                break;
            }
        }
        if (fKernelFound)
            break; // if kernel is found stop searching
    }
    if (nCredit == 0 || nCredit > nAvailable - nReserveBalance)
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
            if (nCredit + pcoin.txout.nValue > nAvailable - nReserveBalance)
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

    void setLastCoinStakeSearchInterval(int64_t interval) override
    {
        m_wallet->SetLastCoinStakeSearchInterval(interval);
    }

    size_t getAvailableCoinCount() override
    {
        LOCK(m_wallet->cs_wallet);
        std::vector<COutput> coins;
        CCoinControl coincontrol;
        m_wallet->AvailableCoins(coins, &coincontrol);
        return coins.size();
    }

    void blockUntilSyncedToCurrentChain() override { m_wallet->BlockUntilSyncedToCurrentChain(); }
    int64_t getLastCoinStakeSearchInterval() override { return m_wallet->GetLastCoinStakeSearchInterval(); }

    bool getStakeWeight(uint64_t& average_weight, uint64_t& total_weight) override
    {
        return GetStakeWeight(m_wallet.get(), average_weight, total_weight, Params().GetConsensus());
    }

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

    bool createCoinStake(CChainState& chainstate,
        unsigned int nBits,
        int64_t nSearchInterval,
        CMutableTransaction& tx_new,
        const Consensus::Params& consensus_params) override
    {
        return CreateCoinStake(m_wallet.get(), &chainstate, nBits, nSearchInterval, tx_new, consensus_params);
    }

    bool finalizeCoinStakeReward(CChainState& chainstate,
        CMutableTransaction& tx_coinstake,
        const CAmount& fees,
        const Consensus::Params& consensus_params) override
    {
        return FinalizeCoinStakeReward(m_wallet.get(), &chainstate, tx_coinstake, fees, consensus_params);
    }

    bool signBlock(CBlock& block) override { return SignBlock(block, *m_wallet); }

    bool getStakeCoins(std::vector<interfaces::StakeCoin>& coins) override
    {
        std::set<CInputCoin> set_coins;
        if (!m_wallet->GetStakeWeightSet(set_coins)) return false;
        coins.clear();
        coins.reserve(set_coins.size());
        for (const CInputCoin& coin : set_coins) {
            coins.push_back({coin.outpoint, coin.txout.nValue});
        }
        return true;
    }

private:
    std::shared_ptr<CWallet> m_wallet;
    //! Destination reserved for the coinstake, released unless keepDestination()
    //! is called. Held for this object's lifetime, matching the ReserveDestination
    //! that PoSMiner used to keep on its stack for the whole staking loop.
    std::unique_ptr<ReserveDestination> m_reservedest;
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
