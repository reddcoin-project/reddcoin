// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ID_TRANSACTION_HANDLER_H
#define BITCOIN_ID_TRANSACTION_HANDLER_H

#include <primitives/transaction.h>
#include <script/script.h>

#include <vector>

/**
 * Process a transaction for ReddID operations
 * 
 * @param tx The transaction to process
 * @param nHeight The block height
 * @return True if the transaction was processed, false otherwise
 */
bool ProcessReddIDTransaction(const CTransaction& tx, int nHeight);

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

#endif // BITCOIN_ID_TRANSACTION_HANDLER_H
