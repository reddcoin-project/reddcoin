// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <id/transaction_handler.h>
#include <id/reddid.h>
#include <node/context.h>

#include <logging.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>

extern NodeContext* g_node_context;

bool ProcessReddIDTransaction(const CTransaction& tx, int nHeight)
{
    // Skip coinbase transactions
    if (tx.IsCoinBase()) {
        return true;
    }
    
    bool processed = false;
    
    // Use the ReddID manager from node context
    if (g_node_context && g_node_context->reddid) {
        processed = g_node_context->reddid->ProcessTransaction(tx, nHeight);
    }
    
    return processed;
}

bool ParseReddIDScript(const CScript& script, unsigned char& opCode, std::vector<unsigned char>& data)
{
    if (script.size() < 2 || script[0] != OP_RETURN) {
        return false;
    }
    
    std::vector<unsigned char> vchData;
    CScript::const_iterator pc = script.begin() + 1;
    opcodetype opcodeRet;
    
    if (!script.GetOp(pc, opcodeRet, vchData) || vchData.size() < 2) {
        return false;
    }
    
    // Check for ReddID prefix ('R')
    if (vchData[0] != 'R') {
        return false;
    }
    
    // Extract operation code and data
    opCode = vchData[1];
    data = std::vector<unsigned char>(vchData.begin() + 2, vchData.end());
    
    return true;
}

bool IsReddIDOpCode(unsigned char opCode)
{
    return opCode == OP_NAMESPACE_AUCTION_CREATE ||
           opCode == OP_NAMESPACE_AUCTION_BID ||
           opCode == OP_NAMESPACE_AUCTION_FINALIZE ||
           opCode == OP_NAMESPACE_AUCTION_CANCEL ||
           opCode == OP_AUCTION_CREATE ||
           opCode == OP_AUCTION_BID ||
           opCode == OP_AUCTION_FINALIZE ||
           opCode == OP_AUCTION_CANCEL ||
           opCode == OP_REDDID_AUCTION_CREATE ||
           opCode == OP_REDDID_AUCTION_BID ||
           opCode == OP_REDDID_AUCTION_FINALIZE ||
           opCode == OP_REDDID_PROFILE_UPDATE ||
           opCode == OP_REDDID_CONNECTION;
}
