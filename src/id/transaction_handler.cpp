// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <id/transaction_handler.h>

#include <crypto/common.h>
#include <id/reddid.h>
#include <logging.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>

#include <vector>

bool ProcessReddIDTransaction(const CTransaction& tx, int nHeight, ReddIDManager* reddid)
{
    // Skip coinbase and coinstake transactions
    if (tx.IsCoinBase() || tx.IsCoinStake()) {
        return true;
    }

    if (!reddid) {
        return false;
    }

    return reddid->ProcessTransaction(tx, nHeight);
}

bool ParseReddIDScript(const CScript& script, unsigned char& opCode, std::vector<unsigned char>& data) {
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

bool IsReddIDOpCode(unsigned char opCode) {
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
           opCode == OP_REDDID_CONNECTION ||
           opCode == OP_BID_DEPOSIT ||
           opCode == OP_AUCTION_SETTLE ||
           opCode == OP_AUCTION_DEFAULT ||
           opCode == OP_DEPOSIT_FORFEIT;
}

bool ParseReddIDOpReturn(const CScript& script, unsigned char& opCode, std::vector<unsigned char>& data) {
    // Check if this is an OP_RETURN script
    if (script.size() < 2 || script[0] != OP_RETURN) {
        return false;
    }

    // Extract the data from OP_RETURN
    std::vector<unsigned char> vchData;
    CScript::const_iterator pc = script.begin() + 1;
    opcodetype opcodeRet;
    if (!script.GetOp(pc, opcodeRet, vchData) || vchData.size() < 2) {
        return false;
    }

    // Check if this is a ReddID operation (first byte should be 'R' and second byte an op code)
    if (vchData.size() < 2 || vchData[0] != 'R') {
        return false;
    }

    // Extract operation code and data
    opCode = vchData[1];
    data = std::vector<unsigned char>(vchData.begin() + 2, vchData.end());

    return true;
}

CScript CreateNamespaceAuctionOpReturn(const std::string& namespaceId) {
    // Format: 'R' + opcode + len(1) + namespace
    std::vector<unsigned char> data;
    data.push_back('R');
    data.push_back(OP_NAMESPACE_AUCTION_CREATE);

    // Length-prefixed namespace ID
    data.push_back(static_cast<unsigned char>(namespaceId.size()));
    data.insert(data.end(), namespaceId.begin(), namespaceId.end());

    if (data.size() > REDDID_OP_RETURN_MAX_PAYLOAD) {
        LogPrintf("ERROR: CreateNamespaceAuctionOpReturn: payload %d exceeds max %d bytes\n",
                  data.size(), REDDID_OP_RETURN_MAX_PAYLOAD);
        return CScript();
    }

    CScript script;
    script << OP_RETURN << data;
    return script;
}

CScript CreateUserIDAuctionOpReturn(const std::string& name, const std::string& namespaceId) {
    // Format: 'R' + opcode + len(1) + name + len(1) + namespace
    std::vector<unsigned char> data;
    data.push_back('R');
    data.push_back(OP_AUCTION_CREATE);

    // Length-prefixed name
    data.push_back(static_cast<unsigned char>(name.size()));
    data.insert(data.end(), name.begin(), name.end());

    // Length-prefixed namespace ID
    data.push_back(static_cast<unsigned char>(namespaceId.size()));
    data.insert(data.end(), namespaceId.begin(), namespaceId.end());

    if (data.size() > REDDID_OP_RETURN_MAX_PAYLOAD) {
        LogPrintf("ERROR: CreateUserIDAuctionOpReturn: payload %d exceeds max %d bytes\n",
                  data.size(), REDDID_OP_RETURN_MAX_PAYLOAD);
        return CScript();
    }

    CScript script;
    script << OP_RETURN << data;
    return script;
}

CScript CreateUserIDBidOpReturn(const uint256& auctionId, CAmount bidAmount) {
    // Format: 'R' + opcode + auctionId(32) + amount(8)
    std::vector<unsigned char> data;
    data.push_back('R');
    data.push_back(OP_AUCTION_BID);

    // Add auction ID (fixed 32 bytes)
    data.insert(data.end(), auctionId.begin(), auctionId.end());

    // Add bid amount (fixed 8 bytes, little-endian)
    std::vector<unsigned char> amountData(8);
    WriteLE64(amountData.data(), static_cast<uint64_t>(bidAmount));
    data.insert(data.end(), amountData.begin(), amountData.end());

    if (data.size() > REDDID_OP_RETURN_MAX_PAYLOAD) {
        LogPrintf("ERROR: CreateUserIDBidOpReturn: payload %d exceeds max %d bytes\n",
                  data.size(), REDDID_OP_RETURN_MAX_PAYLOAD);
        return CScript();
    }

    CScript script;
    script << OP_RETURN << data;
    return script;
}

CScript CreateProfileUpdateOpReturn(const std::string& reddId, const uint256& profileHash) {
    // Format: 'R' + opcode + len(1) + reddId + profileHash(32)
    std::vector<unsigned char> data;
    data.push_back('R');
    data.push_back(OP_REDDID_PROFILE_UPDATE);

    // Length-prefixed ReddID
    data.push_back(static_cast<unsigned char>(reddId.size()));
    data.insert(data.end(), reddId.begin(), reddId.end());

    // Add profile hash (fixed 32 bytes)
    data.insert(data.end(), profileHash.begin(), profileHash.end());

    if (data.size() > REDDID_OP_RETURN_MAX_PAYLOAD) {
        LogPrintf("ERROR: CreateProfileUpdateOpReturn: payload %d exceeds max %d bytes\n",
                  data.size(), REDDID_OP_RETURN_MAX_PAYLOAD);
        return CScript();
    }

    CScript script;
    script << OP_RETURN << data;
    return script;
}

CScript CreateConnectionOpReturn(const std::string& fromReddId, const std::string& toReddId,
                                 SocialConnectionType type) {
    // Format: 'R' + opcode + len(1) + fromReddId + len(1) + toReddId + type(1)
    std::vector<unsigned char> data;
    data.push_back('R');
    data.push_back(OP_REDDID_CONNECTION);

    // Length-prefixed source ReddID
    data.push_back(static_cast<unsigned char>(fromReddId.size()));
    data.insert(data.end(), fromReddId.begin(), fromReddId.end());

    // Length-prefixed target ReddID
    data.push_back(static_cast<unsigned char>(toReddId.size()));
    data.insert(data.end(), toReddId.begin(), toReddId.end());

    // Connection type (1 byte)
    data.push_back(static_cast<unsigned char>(type));

    if (data.size() > REDDID_OP_RETURN_MAX_PAYLOAD) {
        LogPrintf("ERROR: CreateConnectionOpReturn: payload %d exceeds max %d bytes\n",
                  data.size(), REDDID_OP_RETURN_MAX_PAYLOAD);
        return CScript();
    }

    CScript script;
    script << OP_RETURN << data;
    return script;
}

CScript CreateBidDepositOpReturn(const uint256& auctionId, CAmount bidAmount,
                                 uint8_t depositVout, const CKeyID& bidderPKH) {
    // Format: 'R'+'D'+auctionId(32)+bidAmt(8)+vout(1)+bidderPKH(20) = 64 bytes
    std::vector<unsigned char> data;
    data.push_back('R');
    data.push_back(OP_BID_DEPOSIT);

    // Add auction ID (32 bytes)
    data.insert(data.end(), auctionId.begin(), auctionId.end());

    // Add bid amount (8 bytes, little-endian)
    std::vector<unsigned char> amountData(8);
    WriteLE64(amountData.data(), static_cast<uint64_t>(bidAmount));
    data.insert(data.end(), amountData.begin(), amountData.end());

    // Add deposit vout index (1 byte)
    data.push_back(depositVout);

    // Add bidder public key hash (20 bytes)
    data.insert(data.end(), bidderPKH.begin(), bidderPKH.end());

    if (data.size() > REDDID_OP_RETURN_MAX_PAYLOAD) {
        LogPrintf("ERROR: CreateBidDepositOpReturn: payload %d exceeds max %d bytes\n",
                  data.size(), REDDID_OP_RETURN_MAX_PAYLOAD);
        return CScript();
    }

    CScript script;
    script << OP_RETURN << data;
    return script;
}

CScript CreateAuctionSettleOpReturn(const uint256& auctionId, const uint256& bidId) {
    // Format: 'R'+'S'+auctionId(32)+bidId(32) = 66 bytes payload
    std::vector<unsigned char> data;
    data.push_back('R');
    data.push_back(OP_AUCTION_SETTLE);

    data.insert(data.end(), auctionId.begin(), auctionId.end());
    data.insert(data.end(), bidId.begin(), bidId.end());

    if (data.size() > REDDID_OP_RETURN_MAX_PAYLOAD) {
        LogPrintf("ERROR: CreateAuctionSettleOpReturn: payload %d exceeds max %d bytes\n",
                  data.size(), REDDID_OP_RETURN_MAX_PAYLOAD);
        return CScript();
    }

    CScript script;
    script << OP_RETURN << data;
    return script;
}

// ReddIDManager's OP_RETURN helpers forward to the free functions above. They
// are defined here so that id/reddid.cpp does not depend on this file. The
// leading :: is required: without it each member would call itself.

bool ReddIDManager::ParseReddIDOpReturn(const CScript& script, unsigned char& opCode, std::vector<unsigned char>& data) {
    return ::ParseReddIDOpReturn(script, opCode, data);
}

CScript ReddIDManager::CreateNamespaceAuctionOpReturn(const std::string& namespaceId) {
    return ::CreateNamespaceAuctionOpReturn(namespaceId);
}

CScript ReddIDManager::CreateUserIDAuctionOpReturn(const std::string& name, const std::string& namespaceId) {
    return ::CreateUserIDAuctionOpReturn(name, namespaceId);
}

CScript ReddIDManager::CreateUserIDBidOpReturn(const uint256& auctionId, CAmount bidAmount) {
    return ::CreateUserIDBidOpReturn(auctionId, bidAmount);
}

CScript ReddIDManager::CreateProfileUpdateOpReturn(const std::string& reddId, const uint256& profileHash) {
    return ::CreateProfileUpdateOpReturn(reddId, profileHash);
}

CScript ReddIDManager::CreateConnectionOpReturn(const std::string& fromReddId, const std::string& toReddId,
                                               SocialConnectionType type) {
    return ::CreateConnectionOpReturn(fromReddId, toReddId, type);
}

CScript ReddIDManager::CreateBidDepositOpReturn(const uint256& auctionId, CAmount bidAmount,
                                               uint8_t depositVout, const CKeyID& bidderPKH) {
    return ::CreateBidDepositOpReturn(auctionId, bidAmount, depositVout, bidderPKH);
}

CScript ReddIDManager::CreateAuctionSettleOpReturn(const uint256& auctionId, const uint256& bidId) {
    return ::CreateAuctionSettleOpReturn(auctionId, bidId);
}
