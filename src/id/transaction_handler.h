// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ID_TRANSACTION_HANDLER_H
#define BITCOIN_ID_TRANSACTION_HANDLER_H

#include <amount.h>
#include <id/reddid.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/script.h>
#include <uint256.h>

#include <cstdint>
#include <string>
#include <vector>

class ReddIDManager;

/**
 * Process a transaction for ReddID operations
 *
 * @param tx The transaction to process
 * @param nHeight The block height
 * @param reddid The ReddID manager instance
 * @return True if the transaction was processed, false otherwise
 */
bool ProcessReddIDTransaction(const CTransaction& tx, int nHeight, ReddIDManager* reddid);

/**
 * Parse an OP_RETURN script for ReddID operations
 *
 * @param script The script to parse
 * @param opCode The operation code found in the script
 * @param data The data found in the script
 * @return True if the script was parsed successfully, false otherwise
 */
bool ParseReddIDScript(const CScript& script, unsigned char& opCode, std::vector<unsigned char>& data);

/**
 * Check if an operation code is a valid ReddID operation
 *
 * @param opCode The operation code to check
 * @return True if the operation code is a valid ReddID operation, false otherwise
 */
bool IsReddIDOpCode(unsigned char opCode);

/**
 * OP_RETURN helpers
 *
 * The parser splits the first data push after OP_RETURN into the operation
 * code and the bytes that follow the two-byte 'R' + opcode header. The
 * builders return an empty script when the payload, header included, would
 * exceed REDDID_OP_RETURN_MAX_PAYLOAD bytes.
 */
bool ParseReddIDOpReturn(const CScript& script, unsigned char& opCode,
                         std::vector<unsigned char>& data);
CScript CreateNamespaceAuctionOpReturn(const std::string& namespaceId);
CScript CreateUserIDAuctionOpReturn(const std::string& name,
                                    const std::string& namespaceId);
CScript CreateUserIDBidOpReturn(const uint256& auctionId, CAmount bidAmount);
CScript CreateProfileUpdateOpReturn(const std::string& reddId, const uint256& profileHash);
CScript CreateConnectionOpReturn(const std::string& fromReddId,
                                 const std::string& toReddId,
                                 SocialConnectionType type);
CScript CreateBidDepositOpReturn(const uint256& auctionId, CAmount bidAmount,
                                 uint8_t depositVout, const CKeyID& bidderPKH);
CScript CreateAuctionSettleOpReturn(const uint256& auctionId, const uint256& bidId);

#endif // BITCOIN_ID_TRANSACTION_HANDLER_H
