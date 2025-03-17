// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_REDDID_H
#define BITCOIN_RPC_REDDID_H

#include <univalue.h>

/**
 * RPC commands for ReddID system
 */

// Declare RPC methods
UniValue getnamespacelist(const UniValue& params, bool fHelp);
UniValue getnamespaceinfo(const UniValue& params, bool fHelp);
UniValue getnamespaceauctionlist(const UniValue& params, bool fHelp);
UniValue getnamespaceauctioninfo(const UniValue& params, bool fHelp);
UniValue createnamespacesauction(const UniValue& params, bool fHelp);
UniValue bidonnamespacesauction(const UniValue& params, bool fHelp);
UniValue finalizenamespacesauction(const UniValue& params, bool fHelp);
UniValue cancelnamespacesauction(const UniValue& params, bool fHelp);
UniValue configurenamespacess(const UniValue& params, bool fHelp);
UniValue getnamespacesauctions(const UniValue& params, bool fHelp);
UniValue getnamespacesauctionbids(const UniValue& params, bool fHelp);
UniValue renewnamespace(const UniValue& params, bool fHelp);
UniValue transfernamespace(const UniValue& params, bool fHelp);

UniValue getuseridlist(const UniValue& params, bool fHelp);
UniValue getuseridinfo(const UniValue& params, bool fHelp);
UniValue getuserauctionlist(const UniValue& params, bool fHelp);
UniValue getuserauctioninfo(const UniValue& params, bool fHelp);
UniValue createuseridauction(const UniValue& params, bool fHelp);
UniValue bidonuseridauction(const UniValue& params, bool fHelp);
UniValue finalizeuseridauction(const UniValue& params, bool fHelp);
UniValue canceluseridauction(const UniValue& params, bool fHelp);
UniValue getuseridauctions(const UniValue& params, bool fHelp);
UniValue getuseridauctionbids(const UniValue& params, bool fHelp);
UniValue renewuserid(const UniValue& params, bool fHelp);
UniValue transferuserid(const UniValue& params, bool fHelp);

UniValue getuserconnections(const UniValue& params, bool fHelp);
UniValue createuserconnection(const UniValue& params, bool fHelp);
UniValue updateuserconnection(const UniValue& params, bool fHelp);
UniValue removeuserconnection(const UniValue& params, bool fHelp);

UniValue getuserreputation(const UniValue& params, bool fHelp);
UniValue updateuserreputation(const UniValue& params, bool fHelp);
UniValue calculateuserreputation(const UniValue& params, bool fHelp);
UniValue getuserreputationhistory(const UniValue& params, bool fHelp);

// Utility functions
void RegisterReddIDRPCCommands();

#endif // BITCOIN_RPC_REDDID_H
