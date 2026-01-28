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

    if (Solver(txout.scriptPubKey, vSolutions) != TxoutType::PUBKEY) {
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
            // For descriptor wallets with P2PK coinstake outputs:
            // The original script was P2PKH but was converted to P2PK for staking.
            // We need to construct a P2PKH script to query the descriptor wallet,
            // since that's what it tracks internally.
            CScript p2pkh_script = GetScriptForDestination(PKHash(keyid));
            if (desc->GetKey(p2pkh_script, keyid, key)) {
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

    if (Solver(txout.scriptPubKey, vSolutions) != TxoutType::PUBKEY) {
        return false;
    }

    const valtype& vchPubKey = vSolutions[0];

    CPubKey key(vchPubKey);
    if (block.vchBlockSig.empty()) {
        return false;
    }

    return key.Verify(block.GetHash(), block.vchBlockSig);
}
