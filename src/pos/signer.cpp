// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/signer.h>

#include <chainparams.h>
#include <script/standard.h>

typedef std::vector<unsigned char> valtype;

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

bool CheckBlockSignature(const CBlock& block)
{
    if (block.GetHash() == Params().GetConsensus().hashGenesisBlock) {
        return block.vchBlockSig.empty();
    }

    std::vector<valtype> vSolutions;
    const CTxOut& txout = block.IsProofOfStake() ? block.vtx[1]->vout[1] : block.vtx[0]->vout[0];
    TxoutType whichType = Solver(txout.scriptPubKey, vSolutions);

    // Taproot coinstake output: verify the block's BIP340 Schnorr signature
    // against the taproot output key.
    if (whichType == TxoutType::WITNESS_V1_TAPROOT) {
        if (block.vchBlockSig.size() != 64) {
            return false;
        }
        XOnlyPubKey output_key{vSolutions[0]};
        return output_key.VerifySchnorr(block.GetHash(), block.vchBlockSig);
    }

    if (whichType != TxoutType::PUBKEY) {
        return false;
    }

    const valtype& vchPubKey = vSolutions[0];

    CPubKey key(vchPubKey);
    if (block.vchBlockSig.empty()) {
        return false;
    }

    return key.Verify(block.GetHash(), block.vchBlockSig);
}
