// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/signer.h>

#include <chainparams.h>
#include <script/standard.h>

typedef std::vector<unsigned char> valtype;

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
