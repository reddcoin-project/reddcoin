// Copyright (c) 2014-2024 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_STAKER_H
#define BITCOIN_STAKER_H

#include <sync.h>
#include <threadinterrupt.h>
#include <threadsafety.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class CConnman;
class ChainstateManager;
class CTxMemPool;
class CWallet;

class CClientUIInterface;
class CChainParams;
class CScheduler;

// logging defaults
static const bool DEFAULT_PRINTFEE = false;
static const bool DEFAULT_PRINTCREATION = false;

class CStakeman
{
public:
    struct Options {
        CClientUIInterface* uiInterface = nullptr;
        ChainstateManager* chainman = nullptr;
        CConnman* connman = nullptr;
        CTxMemPool* mempool = nullptr;
    };

    CStakeman(bool stake_active = true);
    ~CStakeman();

    void Init(const Options& connOptions);
    void InitWallets();
    bool Start();
    bool Start(CScheduler& scheduler, const Options& options);
    void Interrupt();
    void StopThreads();
    void Stop()
    {
        StopThreads();
    };
    bool GetNodeStakingActive() const { return fStakingActive; };
    void SetStakingActive(bool active);
    //! Number of staking threads that are still running. A thread that returned
    //! on its own is not counted, even though it has not been joined yet.
    int GetStakingThreadCount();
    void static ThreadStaker(CWallet* pwallet, ChainstateManager* chainman, CConnman* connman, CTxMemPool* mempool, std::thread::id thread_id, std::atomic<bool> &running, CThreadInterrupt& interrupt);
    void StakeWalletAdd(const std::string& walletname);
    void StakeWalletRemove(const std::string& walletname);

private:
    std::atomic<bool> fStakingActive{true};

    //! One staking thread per wallet. Keyed by wallet name so that the entry is
    //! created by whoever launches the thread, rather than by the thread itself
    //! once it has started running: a stop that arrives in between would
    //! otherwise find nothing to stop and leave the thread behind.
    struct StakerThread {
        std::thread thread;
        //! Signalled to wake this thread out of a sleep so it can stop. Held by
        //! shared_ptr because the thread's callable outlives this entry.
        std::shared_ptr<CThreadInterrupt> interrupt;
        //! Set by the thread as it returns, so a thread that stopped on its own
        //! is not reported as running and is reaped before it is replaced.
        std::shared_ptr<std::atomic<bool>> finished;
    };

    std::unordered_map<std::string, StakerThread> m_stakers GUARDED_BY(cs_threadStakeMinterGroup);
    mutable RecursiveMutex cs_threadStakeMinterGroup;

    CClientUIInterface* clientInterface;
    ChainstateManager* chainManager;
    CConnman* connManager;
    CTxMemPool* memPool;
};

#endif // BITCOIN_STAKER_H
