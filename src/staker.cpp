// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <staker.h>

#include <fs.h>
#include <logging.h>
#include <miner.h>
#include <net_processing.h>
#include <node/ui_interface.h>
#include <txmempool.h>
#include <util/system.h>
#include <util/thread.h>
#include <util/translation.h>
#include <validation.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>

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
}

void CStakeman::InitWallets()
{
    LogPrintf("CStakeman::%s\n", __func__);

    try {
        std::set<fs::path> wallet_paths;
        for (const std::string& wallet_name : gArgs.GetArgs("-stake")) {
            if (!wallet_paths.insert(wallet_name).second) {
                continue;
            }

            std::shared_ptr<CWallet> pwallet = GetWallet(wallet_name);
            if (!pwallet) {
                return;
            }

            LogPrintf("CStakeman::[%s] Init for staking\n", wallet_name);

            if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                LogPrintf("CStakeman::[%s] error: Disable private keys flag set.\n", wallet_name);
                continue;
            } else if (pwallet->IsWalletFlagSet(WALLET_FLAG_BLANK_WALLET)) {
                LogPrintf("CStakeman::[%s] error: Blank wallet flag set.\n", wallet_name);
                continue;
            } else {
                pwallet->SetEnableStaking(true);
            }
        }

        return;
    } catch (const std::runtime_error& e) {
        LogPrintf("CStakeman::%s\n", e.what());

        return;
    }
}

bool CStakeman::Start()
{
    if (!fStakingActive) {
        return false;
    }
    InitWallets();
    LogPrintf("CStakeman::%s\n", __func__);

    if (clientInterface) {
        clientInterface->InitMessage(_("Loading Staking wallets…").translated);
    }

    //
    // Start threads
    //
    uiInterface.InitMessage(_("Starting staking threads…").translated);
    interruptStake.reset();

    std::vector<std::shared_ptr<CWallet>> m_stake_wallets = GetWallets();
    for (const auto& wallet : m_stake_wallets) {
        if (wallet->GetEnableStaking()) {
            LogPrintf("CStakeman::%s launching staking thread for wallet...%s\n", __func__, wallet->GetName());
            if (wallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                LogPrintf("CStakeman::Disable private keys flag set.. skipping [%s]\n", wallet->GetName());
                continue;
            } else if (wallet->IsWalletFlagSet(WALLET_FLAG_BLANK_WALLET)) {
                LogPrintf("CStakeman::Blank wallet flag set.. skipping [%s]\n", wallet->GetName());
                continue;
            } else {
                StakeWalletAdd(wallet->GetName());
                LogPrintf("CStakeman::%s Launching wallet..  [%s]\n", __func__, wallet->GetName());
            }
        }
    }

    uiInterface.InitMessage(_("Staking threads started…").translated);

    return true;
}

bool CStakeman::Start(CScheduler& scheduler, const Options& stakeOptions)
{
    Init(stakeOptions);
    InitWallets();
    LogPrintf("CStakeman::%s\n", __func__);

    if (clientInterface) {
        clientInterface->InitMessage(_("Loading Staking wallets…").translated);
    }

    //
    // Start threads
    //
    uiInterface.InitMessage(_("Starting staking threads…").translated);
    interruptStake.reset();

    std::vector<std::shared_ptr<CWallet>> m_stake_wallets = GetWallets();
    for (const auto& wallet : m_stake_wallets) {
        if (wallet->GetEnableStaking()) {
            LogPrintf("CStakeman::%s launching staking thread for wallet...%s\n", __func__, wallet->GetName());
            if (wallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                LogPrintf("CStakeman::Disable private keys flag set.. skipping [%s]\n", wallet->GetName());
                continue;
            } else if (wallet->IsWalletFlagSet(WALLET_FLAG_BLANK_WALLET)) {
                LogPrintf("CStakeman::Blank wallet flag set.. skipping [%s]\n", wallet->GetName());
                continue;
            } else {
                StakeWalletAdd(wallet->GetName());
                LogPrintf("CStakeman::%s Launching wallet..  [%s]\n", __func__, wallet->GetName());
            }
        }
    }

    uiInterface.InitMessage(_("Staking threads started…").translated);

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

    uiInterface.NotifyStakingActiveChanged(false);
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

    uiInterface.NotifyStakingActiveChanged(fStakingActive);
}

void CStakeman::StakeWalletAdd(const std::string& walletname)
{
    LogPrintf("CStakeman::%s\n", __func__);
    if (!fStakingActive) {
        return;
    }
    std::vector<std::shared_ptr<CWallet>> m_stake_wallets = GetWallets();
    for (const auto& wallet : m_stake_wallets) {
        if (wallet->GetName() == walletname) {
            if (wallet->GetEnableStaking()) {
                {
                    LOCK(cs_threadStakeMinterGroup);
                    threadStakeMinterGroup.push_back(
                        std::thread(&util::TraceThread, "staker", [this, pwallet = wallet.get(), chainManager = chainManager, connManager = connManager, mempool = memPool]() {
                            tm_[pwallet->GetName()] = std::this_thread::get_id();
                            ThreadStaker(pwallet, chainManager, connManager, mempool, std::this_thread::get_id(), fStakingActive);
                        }));
                }

                LogPrintf("CStakeman::%s Launching wallet..  [%s]\n", __func__, wallet->GetName());
                uiInterface.NotifyStakingActiveChanged(true);
            }
        }
    }
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
        uiInterface.NotifyStakingActiveChanged(false);
    }
}

void CStakeman::ThreadStaker(CWallet* pwallet, ChainstateManager* chainman, CConnman* connman, CTxMemPool* mempool, std::thread::id thread_id, std::atomic<bool> &running)
{
    LogPrintf("CStakeman::%s\n", __func__);
    LogPrintf("CStakeman::%s Staking thread [%s] starting\n", __func__, thread_id);
    try {
        PoSMiner(pwallet, chainman, connman, mempool, thread_id, running);
    } catch (std::exception& e) {
        PrintExceptionContinue(&e, "ThreadStakeMinter()");
    } catch (...) {
        PrintExceptionContinue(NULL, "ThreadStakeMinter()");
    }
    pwallet->SetLastCoinStakeSearchInterval(0);
    LogPrintf("CStakeman::%s Staking thread [%s] stopped\n", __func__, thread_id);
    uiInterface.NotifyStakingActiveChanged(false);
}



