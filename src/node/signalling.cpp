// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/signalling.h>

#include <chain.h>
#include <consensus/params.h>
#include <deploymentinfo.h>
#include <deploymentstatus.h>

#include <algorithm>
#include <array>

SignallingInfo GetSignallingInfo(const CBlockIndex* tip, const Consensus::Params& params)
{
    SignallingInfo info;
    if (tip == nullptr) return info;

    info.height = tip->nHeight;
    info.hash = tip->GetBlockHash().GetHex();
    info.period = params.nMinerConfirmationWindow;
    info.threshold = params.nRuleChangeActivationThreshold;
    const auto [current, previous] = g_versionbitscache.Signalling(tip, params);
    info.elapsed = current.elapsed;
    info.previous_elapsed = previous.elapsed;
    info.period_end_height = tip->nHeight + (info.period - current.elapsed);
    info.period_end_time = tip->GetBlockTime() + int64_t{info.period_end_height - tip->nHeight} * params.nPowTargetSpacing;

    std::array<bool, VERSIONBITS_NUM_BITS> known{};
    for (int i = 0; i < Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++i) {
        const auto pos = static_cast<Consensus::DeploymentPos>(i);
        const Consensus::BIP9Deployment& dep = params.vDeployments[pos];
        known[dep.bit] = true;

        DeploymentSignalling d;
        d.name = DeploymentName(pos);
        d.bit = dep.bit;
        d.count = current.count[dep.bit];
        d.previous_count = previous.count[dep.bit];
        d.scheduled = DeploymentEnabled(params, pos);
        if (d.scheduled) {
            d.state = g_versionbitscache.State(tip, params, pos);
            d.start_time = dep.nStartTime;
            d.timeout = dep.nTimeout;
            d.min_activation_height = dep.min_activation_height;
            d.since = g_versionbitscache.StateSinceHeight(tip, params, pos);
            if (const CBlockIndex* since_block = tip->GetAncestor(d.since)) {
                d.since_time = since_block->GetBlockTime();
            }
            if (d.state == ThresholdState::ACTIVE) {
                d.activation_height = d.since;
            } else if (d.state == ThresholdState::LOCKED_IN) {
                // Activation happens at the first period boundary after the
                // locked-in period that is at or above min_activation_height.
                d.activation_height = d.since + info.period;
                if (d.activation_height < dep.min_activation_height) {
                    const int periods = (dep.min_activation_height - d.activation_height + info.period - 1) / info.period;
                    d.activation_height += periods * info.period;
                }
                d.activation_time = tip->GetBlockTime() + int64_t{d.activation_height - tip->nHeight} * params.nPowTargetSpacing;
            } else if (d.state == ThresholdState::STARTED) {
                d.possible = (info.period - info.threshold) >= (current.elapsed - d.count);
            }
        }
        info.deployments.push_back(std::move(d));
    }

    for (int bit = 0; bit < VERSIONBITS_NUM_BITS; ++bit) {
        if (known[bit] || (current.count[bit] == 0 && previous.count[bit] == 0)) continue;
        info.unknown_bits.push_back({bit, current.count[bit], previous.count[bit]});
    }
    return info;
}
