// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_HDACCOUNTMOVE_H
#define BITCOIN_QT_HDACCOUNTMOVE_H

#include <amount.h>
#include <interfaces/wallet.h>

#include <QString>

#include <cstdint>
#include <vector>

class WalletModel;

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

/** One HD account of a wallet whose funds move to a new wallet. */
struct HDAccountMove {
    //! The account of the wallet the funds leave.
    uint32_t from{0};
    std::string label;
    //! What it can spend now.
    CAmount amount{0};
    //! The account of the new wallet they go to.
    uint32_t to{0};
};

/** The accounts of a wallet that hold funds to move, in order, each to the
 *  account of the new wallet numbered next from 0: so the phrase of the new
 *  wallet restores every one of them, as BIP44's account rule has it. */
std::vector<HDAccountMove> PlanHDAccountMove(const std::vector<interfaces::WalletHDAccount>& accounts);

/** Whether the plan moves an account above 0: Move funds then moves each
 *  account in its own transaction, not the send page's account 0 alone. */
bool MovesHDAccounts(const std::vector<HDAccountMove>& plan);

/** The plan, a line for each account. */
QString DescribeHDAccountMove(const std::vector<HDAccountMove>& plan, int unit);

/** Create the accounts of the new wallet, then move each account of the plan
 *  in its own transaction to an address of its account in the new wallet,
 *  in order, after a confirmation that shows each payment and its fee.
 *  Stops at the first transaction that cannot be made or is not accepted.
 *  result: what was done, for the owner. Returns whether every account moved. */
bool MoveHDAccounts(WalletModel& from, WalletModel& to, const std::vector<HDAccountMove>& plan, QWidget* parent, QString& result);

#endif // BITCOIN_QT_HDACCOUNTMOVE_H
