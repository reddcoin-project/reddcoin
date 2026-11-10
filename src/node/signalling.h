// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_SIGNALLING_H
#define BITCOIN_NODE_SIGNALLING_H

#include <versionbits.h>

#include <cstdint>
#include <string>
#include <vector>

class CBlockIndex;
namespace Consensus {
struct Params;
}

/** Signalling state of one versionbits deployment this release knows. */
struct DeploymentSignalling {
    std::string name;
    int bit{0};
    /** False for a deployment set to NEVER_ACTIVE: nothing else below applies. */
    bool scheduled{false};
    ThresholdState state{ThresholdState::DEFINED};
    int64_t start_time{0};
    int64_t timeout{0};
    int min_activation_height{0};
    /** Height of the first block to which the state applies, and that block's
     *  time if it is already in the chain (0 otherwise). */
    int since{0};
    int64_t since_time{0};
    /** Height from which the rules apply, once LOCKED_IN or ACTIVE; -1 otherwise. */
    int activation_height{-1};
    /** Estimated time of activation_height while LOCKED_IN; 0 otherwise. */
    int64_t activation_time{0};
    /** Blocks setting the bit in the current and the previous period. */
    int count{0};
    int previous_count{0};
    /** While STARTED: whether the threshold can still be reached this period. */
    bool possible{false};
};

/** A bit that no deployment of this release uses, seen set in recent blocks. */
struct UnknownBitSignalling {
    int bit{0};
    int count{0};
    int previous_count{0};
};

/** Versionbits signalling for the period that includes the block after the tip. */
struct SignallingInfo {
    int height{-1};
    std::string hash;
    int period{0};
    int threshold{0};
    /** Blocks of the current period so far, and of the previous one. */
    int elapsed{0};
    int previous_elapsed{0};
    /** Height of the last block of the current period, and its estimated time. */
    int period_end_height{0};
    int64_t period_end_time{0};
    std::vector<DeploymentSignalling> deployments;
    /** Only bits set in at least one block of the two periods. */
    std::vector<UnknownBitSignalling> unknown_bits;
};

/** Collect the signalling state for the block after tip. Requires cs_main. */
SignallingInfo GetSignallingInfo(const CBlockIndex* tip, const Consensus::Params& params);

#endif // BITCOIN_NODE_SIGNALLING_H
