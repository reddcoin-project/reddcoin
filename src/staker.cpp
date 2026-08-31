// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <staker.h>

#include <interfaces/handler.h>
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
    if (!fStakingActive || !stakingSupport) {
        return;
    }

    // shared_ptr, not unique_ptr, because TraceThread takes std::function, whose
    // callable must be copyable; the thread holds the wallet for its lifetime.
    std::shared_ptr<interfaces::StakingWallet> wallet = stakingSupport->getStakingWallet(walletname);
    if (!wallet || !wallet->getEnableStaking()) {
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
        staker.thread = std::thread(&util::TraceThread, "staker", [wallet, interrupt, finished, this, chainManager = chainManager, connManager = connManager, mempool = memPool]() {
            ThreadStaker(wallet, chainManager, connManager, mempool, std::this_thread::get_id(), fStakingActive, *interrupt);
            *finished = true;
        });
        m_stakers.emplace(walletname, std::move(staker));
    }

    LogPrintf("CStakeman::%s Launching wallet..  [%s]\n", __func__, walletname);
    wallet->notifyStakingStatusChanged();
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

void CStakeman::ThreadStaker(std::shared_ptr<interfaces::StakingWallet> staking_wallet, ChainstateManager* chainman, CConnman* connman, CTxMemPool* mempool, std::thread::id thread_id, std::atomic<bool> &running, CThreadInterrupt& interrupt)
{
    LogPrintf("CStakeman::%s\n", __func__);
    LogPrintf("CStakeman::%s Staking thread [%s] starting\n", __func__, thread_id);

    // Stop when the wallet is unloaded. This thread holds the wallet alive for
    // as long as it runs, so an unload cannot pull it away mid-pass, but it
    // also cannot finish until this thread lets go: UnloadWallet() waits for
    // the last reference to be released. Interrupting is enough, since the loop
    // returns out of its next sleep.
    //
    // The handler is deliberately a local of this function. It has to be
    // disconnected before the wallet's last reference goes, because ~CWallet
    // asserts that nothing is still subscribed, and a local is destroyed ahead
    // of the staking_wallet parameter it was registered on.
    std::unique_ptr<interfaces::Handler> unload_handler = staking_wallet->handleUnload([&interrupt, thread_id]() {
        LogPrintf("CStakeman::ThreadStaker Staking thread [%s] stopping, wallet unloaded\n", thread_id);
        interrupt();
    });

    try {
        PoSMiner(*staking_wallet, chainman, connman, mempool, thread_id, running, interrupt);
    } catch (std::exception& e) {
        PrintExceptionContinue(&e, "ThreadStakeMinter()");
    } catch (...) {
        PrintExceptionContinue(NULL, "ThreadStakeMinter()");
    }
    staking_wallet->setLastCoinStakeSearchInterval(0);
    LogPrintf("CStakeman::%s Staking thread [%s] stopped\n", __func__, thread_id);
    staking_wallet->notifyStakingStatusChanged();
}



