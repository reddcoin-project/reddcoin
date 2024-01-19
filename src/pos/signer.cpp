// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/signer.h>

#include <chainparams.h>

typedef std::vector<unsigned char> valtype;

bool SignBlock(CBlock& block, const CWallet& keystore)
{
    std::vector<valtype> vSolutions;
    const CTxOut& txout = block.IsProofOfStake() ? block.vtx[1]->vout[1] : block.vtx[0]->vout[0];

    if (Solver(txout.scriptPubKey, vSolutions) != TxoutType::PUBKEY) {
        return false;
    }

    const valtype& vchPubKey = vSolutions[0];

    CKey key;
    if (!keystore.GetLegacyScriptPubKeyMan()->GetKey(CKeyID(Hash160(vchPubKey)), key)) {
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
