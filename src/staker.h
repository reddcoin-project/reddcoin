// Copyright (c) 2014-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_STAKER_H
#define BITCOIN_STAKER_H

#include <sync.h>
#include <threadinterrupt.h>
#include <threadsafety.h>

#include <atomic>
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
    bool GetStakingActive() const { return fStakingActive; };
    void SetStakingActive(bool active);
    int GetStakingThreadCount()
    {
        LOCK(cs_threadStakeMinterGroup);
        return threadStakeMinterGroup.size();
    };
    void static ThreadStaker(CWallet* pwallet, ChainstateManager* chainman, CConnman* connman, CTxMemPool* mempool, std::thread::id thread_id, std::atomic<bool> &running);
    void StakeWalletAdd(const std::string& walletname);
    void StakeWalletRemove(const std::string& walletname);

    /**
     * This is signaled when staking activity should cease.
     */
    CThreadInterrupt interruptStake;

private:
    std::atomic<bool> fStakingActive{true};

    std::vector<std::thread> threadStakeMinterGroup GUARDED_BY(cs_threadStakeMinterGroup);
    mutable RecursiveMutex cs_threadStakeMinterGroup;

    typedef std::unordered_map<std::string, std::thread::id> ThreadMap;
    ThreadMap tm_;

    CClientUIInterface* clientInterface;
    ChainstateManager* chainManager;
    CConnman* connManager;
    CTxMemPool* memPool;
};

#endif // BITCOIN_STAKER_H
