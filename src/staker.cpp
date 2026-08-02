// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <staker.h>

#include <interfaces/staking.h>
#include <logging.h>
#include <miner.h>
#include <net_processing.h>
#include <node/ui_interface.h>
#include <txmempool.h>
#include <util/system.h>
#include <util/thread.h>
#include <util/translation.h>
#include <validation.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

// #include <boost/filesystem/path.hpp>

class CScheduler;

// reddcoin: stake manager
CStakeman::CStakeman(bool stake_active)
{
    // Options connOptions;
    LogPrintf("CStakeman::%s: %s\n", __func__, stake_active);
    Options stakeOptions;
    Init(stakeOptions);
    SetStakingActive(stake_active);
}

CStakeman::~CStakeman()
{
    LogPrintf("CStakeman::%s: \n", __func__);
    Interrupt();
    Stop();
}

void CStakeman::Init(const Options& stakeOptions)
{
    LogPrintf("CStakeman::%s\n", __func__);
    clientInterface = stakeOptions.uiInterface;
    chainManager = stakeOptions.chainman;
    connManager = stakeOptions.connman;
    memPool = stakeOptions.mempool;
    stakingSupport = stakeOptions.staking_support;
}

void CStakeman::InitWallets()
{
    LogPrintf("CStakeman::%s\n", __func__);

    if (!stakingSupport) {
        LogPrintf("CStakeman::%s: wallet support not compiled in, staking unavailable\n", __func__);
        return;
    }

    try {
        std::set<std::string> seen;
        for (const std::string& wallet_name : gArgs.GetArgs("-stake")) {
            if (!seen.insert(wallet_name).second) {
                continue;
            }

            std::unique_ptr<interfaces::StakingWallet> pwallet = stakingSupport->getStakingWallet(wallet_name);
            if (!pwallet) {
                return;
            }

            LogPrintf("CStakeman::[%s] Init for staking\n", wallet_name);

            std::string reason;
            if (!pwallet->canStake(reason)) {
                LogPrintf("CStakeman::[%s] error: %s.\n", wallet_name, reason);
                continue;
            }
            pwallet->setEnableStaking(true);
        }

        return;
    } catch (const std::runtime_error& e) {
        LogPrintf("CStakeman::%s\n", e.what());

        return;
    }
}

// Launch a staking thread for every loaded, staking-enabled, stake-capable
// wallet. Shared by both Start() overloads.
void CStakeman::LaunchStakingThreads()
{
    if (!stakingSupport) {
        LogPrintf("CStakeman::%s: wallet support not compiled in, no staking threads\n", __func__);
        return;
    }

    if (clientInterface) {
        clientInterface->InitMessage(_("Loading Staking wallets…").translated);
    }

    //
    // Start threads
    //
    uiInterface.InitMessage(_("Starting staking threads…").translated);
    interruptStake.reset();

    for (const auto& wallet : stakingSupport->getStakingWallets()) {
        if (!wallet->getEnableStaking()) {
            continue;
        }
        LogPrintf("CStakeman::%s launching staking thread for wallet...%s\n", __func__, wallet->getName());
        std::string reason;
        if (!wallet->canStake(reason)) {
            LogPrintf("CStakeman::%s.. skipping [%s]\n", reason, wallet->getName());
            continue;
        }
        StakeWalletAdd(wallet->getName());
        LogPrintf("CStakeman::%s Launching wallet..  [%s]\n", __func__, wallet->getName());
    }

    uiInterface.InitMessage(_("Staking threads started…").translated);
}

bool CStakeman::Start()
{
    if (!fStakingActive) {
        return false;
    }
    uiInterface.NotifyNodeStakingActiveChanged(fStakingActive);
    InitWallets();
    LogPrintf("CStakeman::%s\n", __func__);

    LaunchStakingThreads();

    return true;
}

bool CStakeman::Start(CScheduler& scheduler, const Options& stakeOptions)
{
    Init(stakeOptions);
    InitWallets();
    LogPrintf("CStakeman::%s\n", __func__);

    LaunchStakingThreads();

    return true;
}

void CStakeman::Interrupt()
{
    LogPrintf("CStakeman::%s\n", __func__);
    interruptStake();
}

void CStakeman::StopThreads()
{
    LogPrintf("CStakeman::%s\n", __func__);
    {
        LOCK(cs_threadStakeMinterGroup);
        for (std::thread& t : threadStakeMinterGroup) {
            LogPrintf("CStakeman::%s Stopping thread %i!\n", __func__, t.get_id());
            if (t.joinable()) t.join();
        }
        threadStakeMinterGroup.clear();
        tm_.clear();
    }

    uiInterface.NotifyNodeStakingActiveChanged(false);
    LogPrintf("CStakeman::%s done!\n", __func__);
}

void CStakeman::SetStakingActive(bool active)
{
    LogPrintf("CStakeman::%s: %s\n", __func__, active);

    if (fStakingActive == active) {
        return;
    }

    fStakingActive = active;
    gArgs.ForceSetArg("-staking", active ? "1" : "0");
}

void CStakeman::StakeWalletAdd(const std::string& walletname)
{
    LogPrintf("CStakeman::%s\n", __func__);
    if (!fStakingActive || !stakingSupport) {
        return;
    }

    // shared_ptr, not unique_ptr, because TraceThread takes std::function, whose
    // callable must be copyable; the thread holds the wallet for its lifetime.
    std::shared_ptr<interfaces::StakingWallet> wallet = stakingSupport->getStakingWallet(walletname);
    if (!wallet || !wallet->getEnableStaking()) {
        return;
    }

    {
        LOCK(cs_threadStakeMinterGroup);
        threadStakeMinterGroup.push_back(
            std::thread(&util::TraceThread, "staker", [this, wallet, chainManager = chainManager, connManager = connManager, mempool = memPool]() {
                tm_[wallet->getName()] = std::this_thread::get_id();
                ThreadStaker(wallet, chainManager, connManager, mempool, std::this_thread::get_id(), fStakingActive);
            }));
    }

    LogPrintf("CStakeman::%s Launching wallet..  [%s]\n", __func__, walletname);
    wallet->notifyStakingStatusChanged();
}

void CStakeman::StakeWalletRemove(const std::string& walletname)
{
    ThreadMap::const_iterator it = tm_.find(walletname);
    if (it != tm_.end()) {
        {
            LOCK(cs_threadStakeMinterGroup);
            auto iter = std::find_if(threadStakeMinterGroup.begin(), threadStakeMinterGroup.end(), [=](std::thread& t) { return (t.get_id() == it->second); });
            if (iter != threadStakeMinterGroup.end()) {
                iter->join();
                threadStakeMinterGroup.erase(iter);
            }
        }

        tm_.erase(walletname);
        LogPrintf("CStakeman::%s Thread %s removed\n", __func__, walletname);
        uiInterface.NotifyWalletStakingActiveChanged(false);
    }
}

void CStakeman::ThreadStaker(std::shared_ptr<interfaces::StakingWallet> staking_wallet, ChainstateManager* chainman, CConnman* connman, CTxMemPool* mempool, std::thread::id thread_id, std::atomic<bool> &running)
{
    LogPrintf("CStakeman::%s\n", __func__);
    LogPrintf("CStakeman::%s Staking thread [%s] starting\n", __func__, thread_id);
    try {
        PoSMiner(*staking_wallet, chainman, connman, mempool, thread_id, running);
    } catch (std::exception& e) {
        PrintExceptionContinue(&e, "ThreadStakeMinter()");
    } catch (...) {
        PrintExceptionContinue(NULL, "ThreadStakeMinter()");
    }
    staking_wallet->setLastCoinStakeSearchInterval(0);
    LogPrintf("CStakeman::%s Staking thread [%s] stopped\n", __func__, thread_id);
    staking_wallet->notifyStakingStatusChanged();
}



