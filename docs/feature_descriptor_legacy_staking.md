# feature_descriptor_legacy_staking.py - Descriptor/Legacy Wallet Staking Interop

This document describes the `test/functional/feature_descriptor_legacy_staking.py` test, which validates that descriptor and legacy wallets can both stake PoS blocks and accept each other's blocks.

## Overview

The test verifies:
1. A descriptor wallet node can stake PoS blocks
2. A legacy wallet node can stake PoS blocks
3. Blocks staked by descriptor wallets are validated by legacy wallet nodes
4. Blocks staked by legacy wallets are validated by descriptor wallet nodes
5. Both nodes stay in sync throughout
6. Descriptor wallets recycle P2PK coinstake outputs without UTXO exhaustion

This is the primary integration test for the descriptor wallet staking feature. It exercises the full P2PK fallback code path by staking far more blocks than the wallet has original UTXOs.

## Key Technical Context

### UTXO Exhaustion Problem

Each coinstake transaction consumes a P2PKH UTXO and produces a P2PK output (consensus requirement). Without the P2PK fallback in `DescriptorScriptPubKeyMan`, the wallet cannot spend recycled P2PK outputs because `pkh()` descriptors only cache P2PKH scripts in `m_map_script_pub_keys`.

Node 0 starts with 89 coinbase UTXOs from the PoW phase. The test stakes 183 total PoS blocks from Node 0 (11 initial + 1 confirm + 70 maturity + 1 split-confirm + 100 exhaustion test), proving that P2PK outputs are recognized and recycled.

### Coinstake Maturity Gap

Coinstake outputs require `COINBASE_MATURITY + 1` (61) confirmations to mature. When staking continuously, the wallet can exhaust all spendable UTXOs before recycled outputs become spendable. The test bridges this gap by splitting the balance into 40 additional UTXOs before the exhaustion test.

### Separate Legacy Addresses

Node 1 receives coins via `sendmany` to 10 separate addresses. This gives each UTXO a distinct `scriptPubKey`, preventing `CreateCoinStake` from combining them into a single coinstake (which would reduce the UTXO count faster).

## Test Structure

### Parameters

```python
self.num_nodes = 2
self.setup_clean_chain = True
self.extra_args = [
    ['-keypool=100', '-whitelist=127.0.0.1', '-peertimeout=999999999'],  # Node 0: descriptor
    ['-keypool=100', '-whitelist=127.0.0.1', '-peertimeout=999999999'],  # Node 1: legacy
]
self.wallet_names = []
```

- `-whitelist=127.0.0.1`: Prevents P2P disconnections from the mocktime/realtime mismatch bug in `net.cpp`
- `-peertimeout=999999999`: Additional protection against peer timeouts during long staking sequences

### Prerequisites

- `wallet` module (wallet support compiled in)
- `sqlite` module (descriptor wallets use SQLite storage)

### Flow

#### Phase 1: Setup (blocks 0-89)

1. **Connect nodes before mocktime**: Nodes are connected first so the P2P handshake uses real time. Setting mocktime before connecting causes `nTimeConnected` (real time) to diverge from `GetTime()` (mocktime), triggering immediate disconnection via `InactivityCheck`.

2. **Create wallets**: Node 0 gets a descriptor wallet (SQLite format), Node 1 gets a legacy wallet (BDB format). Format is verified via `getwalletinfo`.

3. **Generate PoW blocks**: Node 0 generates 89 PoW blocks to a single legacy address, establishing the coin supply.

#### Phase 2: Initial PoS and Coin Distribution (blocks 90-101)

4. **Initial PoS blocks**: Node 0 generates 11 PoS blocks to reach height 100.

5. **Send coins to Node 1**: Uses `sendmany` to send 1,000,000 RDD to each of Node 1's 10 addresses in a single transaction.

6. **Confirm transaction**: One PoS block confirms the send.

#### Phase 3: Coin Maturation (blocks 102-171)

7. **Age Node 1's coins**: Node 0 generates 70 PoS blocks. Node 1's UTXOs are regular (non-coinbase) outputs, so they only need `nStakeMinAge` (10 seconds), but the additional blocks build sufficient coin age weight for reliable kernel finding. Retries up to 10 times per block.

8. **Sync verification**: Both nodes must be at the same height and best block hash.

#### Phase 4: UTXO Exhaustion Test (blocks 172-272)

9. **Split UTXOs**: Creates 40 additional UTXOs of 10,000 RDD each to bridge the 61-block maturity gap.

10. **Descriptor staking (100 blocks)**: Node 0 stakes 100 consecutive PoS blocks with up to 100 attempts per block. Recycled P2PK coinstake outputs have lower coin age weight, requiring more attempts to find a valid kernel. Progress is logged every 10 blocks with UTXO count and balance.

11. **Cross-validation**: Every block hash from Node 0 is verified on Node 1 (positive confirmations).

#### Phase 5: Legacy Wallet Staking (blocks 273-282)

12. **Time advance**: 600 seconds to build coin age for Node 1's UTXOs.

13. **Legacy staking (10 blocks)**: Node 1 stakes 10 blocks, cycling through its 10 addresses. Limited to 10 blocks because each coinstake consumes a UTXO and the outputs need 61 confirmations to mature.

14. **Cross-validation**: Every block hash from Node 1 is verified on Node 0.

#### Phase 6: Final Verification

15. **Sync check**: Both nodes at the same height and best block hash.

16. **UTXO exhaustion assertion**: Verifies that the descriptor wallet's total PoS block count (183) exceeds the original 89 UTXO count.

### Retry Logic

The `generate_pos_block` helper retries with time advancement:

```python
def generate_pos_block(self, wallet_rpc, address, max_attempts=20):
    for attempt in range(max_attempts):
        try:
            advance_time_for_pos(self.nodes, seconds=60)
            return wallet_rpc.generatetoaddress(1, address)[0]
        except Exception as e:
            if "no valid coinstake found" in str(e):
                if attempt < max_attempts - 1:
                    continue
            raise
    raise AssertionError(f"Failed to generate PoS block after {max_attempts} attempts")
```

The exhaustion test uses `max_attempts=100` because recycled P2PK outputs have lower coin age, reducing the probability of finding a valid kernel per attempt.

### Mocktime Management

Time is advanced on all nodes simultaneously via `advance_time_for_pos(self.nodes, seconds=60)`. This keeps both nodes' clocks in sync, preventing P2P timeout issues and ensuring both nodes accept blocks within the allowed timestamp window.

## Block Budget

| Phase | Blocks | Cumulative Height | Staker |
|-------|--------|--------------------|--------|
| PoW | 89 | 89 | Node 0 |
| Initial PoS | 11 | 100 | Node 0 |
| Confirm send | 1 | 101 | Node 0 |
| Maturity | 70 | 171 | Node 0 |
| Confirm split | 1 | 172 | Node 0 |
| Exhaustion test | 100 | 272 | Node 0 |
| Legacy staking | 10 | 282 | Node 1 |

Node 0 total PoS blocks: 183 (from 89 original UTXOs + 40 split UTXOs).

## Running the Test

```bash
python3 test/functional/feature_descriptor_legacy_staking.py

# With verbose output
python3 test/functional/feature_descriptor_legacy_staking.py --loglevel=DEBUG
```

This test takes longer than most functional tests due to the 100-block exhaustion sequence with high retry counts.

## Related Files

- `src/wallet/scriptpubkeyman.cpp`: P2PK fallback in `IsMine`, `GetSigningProvider`, and `MarkUnusedAddresses`
- `src/wallet/scriptpubkeyman.h`: `m_map_pubkeys` member used by P2PK fallback
- `src/miner.cpp`: `CreateCoinStake` converts P2PKH to P2PK outputs
- `test/functional/wallet_descriptor_staking.py`: Single-node descriptor staking test
- `test/functional/test_framework/util.py`: `advance_time_for_pos()` helper
