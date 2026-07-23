// Copyright (c) 2014-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_STAKING_H
#define BITCOIN_INTERFACES_STAKING_H

#include <amount.h>          // For CAmount
#include <primitives/transaction.h> // For COutPoint, CMutableTransaction
#include <script/standard.h> // For CTxDestination

#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

class CBlock;
class CChainState;
class JSONRPCRequest;
namespace Consensus {
struct Params;
}

/** Default for -staketimio, the proof-of-stake kernel search timeout in ms.
 *  Declared here rather than in the wallet because the staking loop that uses
 *  it lives in libbitcoin_server, while the argument is registered by the
 *  wallet. */
static const unsigned int DEFAULT_STAKETIMIO = 500;

namespace interfaces {

//! Wallet-free description of a coin that can be staked.
//!
//! Used at the node/wallet boundary in place of CInputCoin, which is defined in
//! wallet/coinselection.h and so cannot be named by libbitcoin_server.
struct StakeCoin
{
    COutPoint outpoint;
    CAmount value{0};
};

//! Interface giving the node access to one wallet's staking capability.
//!
//! Declared here so libbitcoin_server (miner.cpp, staker.cpp, rpc/mining.cpp)
//! can drive staking without naming CWallet. The only implementation lives in
//! libbitcoin_wallet, so a --disable-wallet build simply has none.
//!
//! Note on direction: libbitcoin_server is linked before libbitcoin_wallet in
//! reddcoind but after it in test_test_reddcoin. Calls must therefore only ever
//! go from server to wallet through this interface, never the reverse.
class StakingWallet
{
public:
    virtual ~StakingWallet() {}

    //! Handle holding the wallet's cs_wallet for as long as it is alive.
    //!
    //! The node holds cs_wallet across whole operations that call back into the
    //! wallet several times (most importantly BlockAssembler::CreateNewBlock,
    //! which invokes abandonOrphanedCoinstakes, createCoinStake,
    //! setLastCoinStakeSearchInterval and finalizeCoinStakeReward in turn), so
    //! per-method locking inside the implementation cannot reproduce the span.
    //! Callers keep the original scope by holding one of these.
    //!
    //! Lock order: cs_wallet is acquired *before* cs_main on the staking path.
    class Lock
    {
    public:
        virtual ~Lock() {}
    };

    //! Acquire the wallet lock. See Lock.
    virtual std::unique_ptr<Lock> lock() = 0;

    //! Get wallet name.
    virtual std::string getName() const = 0;

    //! Return whether the wallet is locked (encrypted and not unlocked).
    virtual bool isLocked() const = 0;

    //! Return whether staking is enabled for this wallet.
    virtual bool getEnableStaking() const = 0;

    //! Enable or disable staking for this wallet. Persists on the underlying
    //! wallet, so it is visible through any StakingWallet wrapping it.
    virtual void setEnableStaking(bool enable) = 0;

    //! Whether this wallet is capable of staking at all. Returns false, and
    //! fills reason for logging, when the wallet has no private keys or is
    //! blank; such wallets are skipped rather than given a staking thread.
    virtual bool canStake(std::string& reason) = 0;

    //! Emit the wallet's staking-status-changed notification.
    virtual void notifyStakingStatusChanged() = 0;

    //! Number of spendable coins, used to scale the stake search timeout.
    virtual size_t getAvailableCoinCount() = 0;

    //! Block until the wallet has processed the current chain tip, so a staking
    //! readout reflects blocks the caller could already have seen.
    virtual void blockUntilSyncedToCurrentChain() = 0;

    //! How far the last kernel search advanced, for getstakinginfo.
    virtual int64_t getLastCoinStakeSearchInterval() = 0;

    //! Average and total coin-age weight of this wallet's stakeable coins.
    virtual bool getStakeWeight(uint64_t& average_weight, uint64_t& total_weight) = 0;

    //! Reserve a destination to receive the coinstake, held until
    //! keepDestination() is called or this object is destroyed.
    virtual bool reserveDestination(CTxDestination& dest, std::string& error) = 0;

    //! Keep the destination reserved by reserveDestination().
    virtual void keepDestination() = 0;

    //! Abandon coinstakes left behind by a reorg.
    virtual void abandonOrphanedCoinstakes() = 0;

    //! Record how far the last kernel search advanced, for getstakinginfo.
    virtual void setLastCoinStakeSearchInterval(int64_t interval) = 0;

    //! Search for a kernel and build the coinstake transaction.
    //! Requires the caller to hold lock().
    virtual bool createCoinStake(CChainState& chainstate,
        unsigned int nBits,
        int64_t nSearchInterval,
        CMutableTransaction& tx_new,
        const Consensus::Params& consensus_params) = 0;

    //! Fold the block's transaction fees into the coinstake reward and re-sign.
    //! createCoinStake() runs before the fees are known, so the block assembler
    //! calls this afterwards. Only output amounts and signatures change.
    //! Requires the caller to hold lock().
    virtual bool finalizeCoinStakeReward(CChainState& chainstate,
        CMutableTransaction& tx_coinstake,
        const CAmount& fees,
        const Consensus::Params& consensus_params) = 0;

    //! Sign the block with the key behind the coinstake output.
    //! Requires the caller to hold lock().
    virtual bool signBlock(CBlock& block) = 0;

    //! Collect the coins this wallet would stake, for weight reporting.
    virtual bool getStakeCoins(std::vector<StakeCoin>& coins) = 0;
};

//! Interface letting the node find staking wallets without naming CWallet.
//!
//! Registered on NodeContext::staking_support by WalletInit::Construct(), which
//! is only compiled when ENABLE_WALLET. It stays null in a --disable-wallet
//! build, and every staking entry point degrades to a clear error.
class StakingSupport
{
public:
    virtual ~StakingSupport() {}

    //! All loaded wallets, for the staker to pick the staking-enabled ones.
    virtual std::vector<std::unique_ptr<StakingWallet>> getStakingWallets() = 0;

    //! Look up a loaded wallet by name. Null if not loaded.
    virtual std::unique_ptr<StakingWallet> getStakingWallet(const std::string& name) = 0;

    //! Wallet selected by an RPC request's endpoint. Null if none.
    virtual std::unique_ptr<StakingWallet> getStakingWalletForRequest(const JSONRPCRequest& request) = 0;
};

} // namespace interfaces

#endif // BITCOIN_INTERFACES_STAKING_H
