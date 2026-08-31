// Copyright (c) 2014-2024 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <staker.h>

#include <fs.h>
#include <interfaces/wallet.h>
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
    uiInterface.NotifyNodeStakingActiveChanged(fStakingActive);
    InitWallets();
    LogPrintf("CStakeman::%s\n", __func__);

    if (clientInterface) {
        clientInterface->InitMessage(_("Loading Staking wallets…").translated);
    }

    //
    // Start threads
    //
    uiInterface.InitMessage(_("Starting staking threads…").translated);

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
    LOCK(cs_threadStakeMinterGroup);
    for (const auto& staker : m_stakers) {
        (*staker.second.interrupt)();
    }
}

void CStakeman::StopThreads()
{
    LogPrintf("CStakeman::%s\n", __func__);

    // Take the threads out of the map first so that the joins below happen with
    // the lock released: a staking pass can take a while to notice it has been
    // interrupted, and GetStakingThreadCount() should not block behind it.
    std::unordered_map<std::string, StakerThread> stakers;
    {
        LOCK(cs_threadStakeMinterGroup);
        stakers.swap(m_stakers);
    }

    // Signal every thread before joining any of them, so they wind down in
    // parallel rather than one sleep at a time.
    for (const auto& staker : stakers) {
        (*staker.second.interrupt)();
    }
    for (auto& staker : stakers) {
        LogPrintf("CStakeman::%s Stopping thread %i!\n", __func__, staker.second.thread.get_id());
        if (staker.second.thread.joinable()) staker.second.thread.join();
    }

    uiInterface.NotifyNodeStakingActiveChanged(false);
    LogPrintf("CStakeman::%s done!\n", __func__);
}

int CStakeman::GetStakingThreadCount()
{
    LOCK(cs_threadStakeMinterGroup);
    int count = 0;
    for (const auto& staker : m_stakers) {
        if (!staker.second.finished->load()) ++count;
    }
    return count;
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
    if (!fStakingActive) {
        return;
    }

    std::shared_ptr<CWallet> wallet = GetWallet(walletname);
    if (!wallet || !wallet->GetEnableStaking()) {
        return;
    }

    auto interrupt = std::make_shared<CThreadInterrupt>();
    auto finished = std::make_shared<std::atomic<bool>>(false);

    {
        LOCK(cs_threadStakeMinterGroup);

        auto it = m_stakers.find(walletname);
        if (it != m_stakers.end()) {
            if (!it->second.finished->load()) {
                // Already staking. A second thread for one wallet would search
                // the same coins twice, and only one of the two would ever be
                // reachable again for stopping.
                LogPrintf("CStakeman::%s [%s] is already staking\n", __func__, walletname);
                return;
            }
            // The previous thread returned on its own (an exhausted keypool, a
            // runtime error). Reap it before it is replaced; it has finished, so
            // this join does not wait.
            if (it->second.thread.joinable()) it->second.thread.join();
            m_stakers.erase(it);
        }

        StakerThread staker;
        staker.interrupt = interrupt;
        staker.finished = finished;
        staker.thread = std::thread(&util::TraceThread, "staker", [this, pwallet = wallet.get(), interrupt, finished, chainManager = chainManager, connManager = connManager, mempool = memPool]() {
            ThreadStaker(pwallet, chainManager, connManager, mempool, std::this_thread::get_id(), fStakingActive, *interrupt);
            *finished = true;
        });
        m_stakers.emplace(walletname, std::move(staker));
    }

    LogPrintf("CStakeman::%s Launching wallet..  [%s]\n", __func__, walletname);
    wallet->NotifyWalletStakingStatusChanged();
}

void CStakeman::StakeWalletRemove(const std::string& walletname)
{
    StakerThread staker;
    {
        LOCK(cs_threadStakeMinterGroup);
        auto it = m_stakers.find(walletname);
        if (it == m_stakers.end()) {
            return;
        }
        staker = std::move(it->second);
        m_stakers.erase(it);
    }

    // Wake the thread before joining it: it spends nearly all its time asleep,
    // and an uninterrupted sleep runs for a minute after a block is found. The
    // join is outside the lock so that the `staking` RPC stays responsive while
    // this thread winds down.
    (*staker.interrupt)();
    if (staker.thread.joinable()) staker.thread.join();

    LogPrintf("CStakeman::%s Thread %s removed\n", __func__, walletname);
    uiInterface.NotifyWalletStakingActiveChanged(false);
}

void CStakeman::ThreadStaker(CWallet* pwallet, ChainstateManager* chainman, CConnman* connman, CTxMemPool* mempool, std::thread::id thread_id, std::atomic<bool> &running, CThreadInterrupt& interrupt)
{
    LogPrintf("CStakeman::%s\n", __func__);
    LogPrintf("CStakeman::%s Staking thread [%s] starting\n", __func__, thread_id);
    try {
        PoSMiner(pwallet, chainman, connman, mempool, thread_id, running, interrupt);
    } catch (std::exception& e) {
        PrintExceptionContinue(&e, "ThreadStakeMinter()");
    } catch (...) {
        PrintExceptionContinue(NULL, "ThreadStakeMinter()");
    }
    pwallet->SetLastCoinStakeSearchInterval(0);
    LogPrintf("CStakeman::%s Staking thread [%s] stopped\n", __func__, thread_id);
    pwallet->NotifyWalletStakingStatusChanged();
}



