// Copyright (c) 2016-2019 The Bitcoin Core developers
// Copyright (c) 2016-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_VERSIONBITS_H
#define BITCOIN_VERSIONBITS_H

#include <chain.h>
#include <sync.h>

#include <array>
#include <map>

/** What block version to use for new blocks (pre versionbits) */
static const int32_t VERSIONBITS_LAST_OLD_BLOCK_VERSION = 5;
/** What bits to set in version for versionbits blocks */
static const int32_t VERSIONBITS_TOP_BITS = 0x20000000UL;
/** What bitmask determines whether versionbits is in use */
static const int32_t VERSIONBITS_TOP_MASK = 0xE0000000UL;
/** Total bits available for versionbits */
static const int32_t VERSIONBITS_NUM_BITS = 29;

/** BIP 9 defines a finite-state-machine to deploy a softfork in multiple stages.
 *  State transitions happen during retarget period if conditions are met
 *  In case of reorg, transitions can go backward. Without transition, state is
 *  inherited between periods. All blocks of a period share the same state.
 */
enum class ThresholdState {
    DEFINED,   // First state that each softfork starts out as. The genesis block is by definition in this state for each deployment.
    STARTED,   // For blocks past the starttime.
    LOCKED_IN, // For at least one retarget period after the first retarget period with STARTED blocks of which at least threshold have the associated bit set in nVersion, until min_activation_height is reached.
    ACTIVE,    // For all blocks after the LOCKED_IN retarget period (final state)
    FAILED,    // For all blocks once the first retarget period after the timeout time is hit, if LOCKED_IN wasn't already reached (final state)
};

// A map that gives the state for blocks whose height is a multiple of Period().
// The map is indexed by the block's parent, however, so all keys in the map
// will either be nullptr or a block with (height + 1) % Period() == 0.
typedef std::map<const CBlockIndex*, ThresholdState> ThresholdConditionCache;

/** Display status of an in-progress BIP9 softfork */
struct BIP9Stats {
    /** Length of blocks of the BIP9 signalling period */
    int period;
    /** Number of blocks with the version bit set required to activate the softfork */
    int threshold;
    /** Number of blocks elapsed since the beginning of the current period */
    int elapsed;
    /** Number of blocks with the version bit set since the beginning of the current period */
    int count;
    /** False if there are not enough blocks left in this period to pass activation threshold */
    bool possible;
};

/** How many blocks of one signalling period set each version bit. */
struct SignallingCounts {
    /** Blocks of the period counted: the whole period once it is over, fewer
     *  for the current one, 0 if the period doesn't exist. */
    int elapsed{0};
    /** Blocks using the versionbits format with each bit set. This is the
     *  condition the deployments count, with no regard to which bits this
     *  release knows. */
    std::array<int, VERSIONBITS_NUM_BITS> count{};
};

/**
 * Abstract class that implements BIP9-style threshold logic, and caches results.
 */
class AbstractThresholdConditionChecker {
protected:
    virtual bool Condition(const CBlockIndex* pindex, const Consensus::Params& params) const =0;
    virtual int64_t BeginTime(const Consensus::Params& params) const =0;
    virtual int64_t EndTime(const Consensus::Params& params) const =0;
    virtual int MinActivationHeight(const Consensus::Params& params) const { return 0; }
    virtual int Period(const Consensus::Params& params) const =0;
    virtual int Threshold(const Consensus::Params& params) const =0;

public:
    /** Returns the numerical statistics of an in-progress BIP9 softfork in the current period */
    BIP9Stats GetStateStatisticsFor(const CBlockIndex* pindex, const Consensus::Params& params) const;
    /** Returns the state for pindex A based on parent pindexPrev B. Applies any state transition if conditions are present.
     *  Caches state from first block of period. */
    ThresholdState GetStateFor(const CBlockIndex* pindexPrev, const Consensus::Params& params, ThresholdConditionCache& cache) const;
    /** Returns the height since when the ThresholdState has started for pindex A based on parent pindexPrev B, all blocks of a period share the same */
    int GetStateSinceHeightFor(const CBlockIndex* pindexPrev, const Consensus::Params& params, ThresholdConditionCache& cache) const;
};

/** BIP 9 allows multiple softforks to be deployed in parallel. We cache
 *  per-period state for every one of them. */
class VersionBitsCache
{
private:
    Mutex m_mutex;
    ThresholdConditionCache m_caches[Consensus::MAX_VERSION_BITS_DEPLOYMENTS] GUARDED_BY(m_mutex);

    /** Signalling counts for the period holding the block after
     *  m_signalling_tip, and for the period before it. */
    const CBlockIndex* m_signalling_tip GUARDED_BY(m_mutex){nullptr};
    SignallingCounts m_signalling_current GUARDED_BY(m_mutex);
    SignallingCounts m_signalling_previous GUARDED_BY(m_mutex);

public:
    /** Get the numerical statistics for a given deployment for the signalling period that includes the block after pindexPrev. */
    static BIP9Stats Statistics(const CBlockIndex* pindexPrev, const Consensus::Params& params, Consensus::DeploymentPos pos);

    static uint32_t Mask(const Consensus::Params& params, Consensus::DeploymentPos pos);

    /** Get the BIP9 state for a given deployment for the block after pindexPrev. */
    ThresholdState State(const CBlockIndex* pindexPrev, const Consensus::Params& params, Consensus::DeploymentPos pos);

    /** Get the block height at which the BIP9 deployment switched into the state for the block after pindexPrev. */
    int StateSinceHeight(const CBlockIndex* pindexPrev, const Consensus::Params& params, Consensus::DeploymentPos pos);

    /** Determine what nVersion a new block should use
     */
    int32_t ComputeBlockVersion(const CBlockIndex* pindexPrev, const Consensus::Params& params);

    /** Per-bit signalling counts for the period that includes the block after
     *  pindexPrev (its blocks up to pindexPrev), and for the period before it.
     *  The counts are cached: when pindexPrev extends the last tip asked for
     *  within the same period, only the new blocks are counted. Otherwise,
     *  after a reorg or at a new period, both periods are counted again. */
    std::pair<SignallingCounts, SignallingCounts> Signalling(const CBlockIndex* pindexPrev, const Consensus::Params& params);

    void Clear();
};

#endif // BITCOIN_VERSIONBITS_H
