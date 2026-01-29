# wallet_descriptor_staking.py - Descriptor Wallet PoS Staking

This document describes the `test/functional/wallet_descriptor_staking.py` test, which validates that descriptor wallets can stake Proof-of-Stake blocks in ReddCoin.

## Overview

The test verifies that `DescriptorScriptPubKeyMan` correctly retrieves private keys needed for:
1. Signing coinstake transactions (P2PKH and P2PK inputs)
2. Signing PoS block headers

This is a single-node test that generates PoW blocks to establish coin supply, then stakes PoS blocks using a descriptor wallet with a `legacy` (P2PKH) address.

## Key Technical Context

### P2PK Coinstake Output Problem

When `CreateCoinStake` builds a coinstake transaction, it converts P2PKH outputs to P2PK outputs (consensus requirement). A `pkh()` descriptor only caches P2PKH scripts in `m_map_script_pub_keys`, not the resulting P2PK scripts.

Without the P2PK fallback in `DescriptorScriptPubKeyMan`, recycled coinstake outputs (which are P2PK) would not be recognized as spendable, causing UTXO exhaustion after all original coinbase UTXOs are consumed.

### Descriptor Wallet SPKMs

A descriptor wallet creates approximately 6 `ScriptPubKeyMan` instances: `pkh` (external/internal), `wpkh` (external/internal), `sh(wpkh)` (external/internal). Each manages different derivation paths. When signing, `CWallet::SignTransaction` iterates all SPKMs until one succeeds.

### Methods Exercised

- **`DescriptorScriptPubKeyMan::GetKey`**: Retrieves the private key for block signing via the coinstake output's public key.
- **`DescriptorScriptPubKeyMan::GetSigningProvider`**: Returns a `FlatSigningProvider` for the coinstake input script. The P2PK fallback uses `m_map_pubkeys` to find the correct descriptor index when the script is not in `m_map_script_pub_keys`.
- **`DescriptorScriptPubKeyMan::IsMine`**: Recognizes P2PK coinstake outputs as spendable via the P2PK fallback.
- **`DescriptorScriptPubKeyMan::MarkUnusedAddresses`**: Uses `find()` (not `operator[]`) to avoid corrupting `m_map_script_pub_keys` with default index values for P2PK scripts.

## Test Structure

### Parameters

```python
self.num_nodes = 1
self.setup_clean_chain = True    # Start from genesis
self.extra_args = [['-keypool=100']]
self.wallet_names = []           # No default wallet
```

### Prerequisites

- `wallet` module (requires wallet support compiled in)
- `sqlite` module (descriptor wallets use SQLite storage)

### Flow

1. **Wallet creation**: Creates a descriptor wallet (`desc_staking`) and verifies SQLite format.

2. **Address generation**: Gets a single `legacy` (P2PKH) address for staking. All coinbase rewards and coinstake outputs go to this address.

3. **PoW phase (blocks 1-89)**: Generates 89 PoW blocks to reach the end of the PoW period (`nLastPowHeight = 89` on regtest). Each block advances mocktime by 60 seconds.

4. **Time advancement**: Advances mocktime by 600 seconds to build coin age for PoS kernel eligibility.

5. **PoS maturity phase (70 blocks)**: Generates `COINBASE_MATURITY + 10 = 70` PoS blocks with retry logic. After 60 blocks, the first coinbase output matures, providing spendable coins. The extra 10 blocks ensure multiple mature coinbases are available.

6. **Balance verification**: Asserts that the mature balance is greater than zero.

7. **Block type verification**: Checks the last 3 blocks for PoS `flags` field to confirm they are PoS blocks.

8. **Final staking verification**: Generates 5 more PoS blocks after another 600-second time advance, confirming continued staking ability.

### Retry Logic

PoS block generation is probabilistic. The test retries up to 20 times per block, advancing mocktime by 60 seconds on each failed attempt:

```python
for attempt in range(max_attempts):
    try:
        self.generate_block(desc_wallet, staking_addr, node)
        success = True
        break
    except Exception as e:
        if "no valid coinstake found" in str(e):
            if attempt < max_attempts - 1:
                node.mocktime += 60
                node.setmocktime(node.mocktime)
        else:
            raise
```

### Mocktime Management

The test uses a `generate_block` helper that advances mocktime based on the current chain tip time:

```python
def generate_block(self, wallet_rpc, address, node):
    block_time = node.getblockheader(node.getbestblockhash())['time']
    new_time = max(getattr(node, 'mocktime', 0), block_time) + POS_BLOCK_SPACING
    node.setmocktime(new_time)
    node.mocktime = new_time
    return wallet_rpc.generatetoaddress(1, address)[0]
```

This ensures each block's timestamp is at least 60 seconds after the previous block, satisfying the PoS coinage requirement (`nStakeMinAge = 10` seconds on regtest).

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `REDDCOIN_COINBASE_MATURITY` | 60 | Confirmations required before coinbase outputs are spendable |
| `POS_BLOCK_SPACING` | 60 | Seconds between blocks for PoS coinage |

## Running the Test

```bash
python3 test/functional/wallet_descriptor_staking.py

# With verbose output
python3 test/functional/wallet_descriptor_staking.py --loglevel=DEBUG
```

## Related Files

- `src/wallet/scriptpubkeyman.cpp`: `DescriptorScriptPubKeyMan` with P2PK fallback in `IsMine`, `GetSigningProvider`, and `MarkUnusedAddresses`
- `src/wallet/scriptpubkeyman.h`: `GetKey` method declaration and `m_map_pubkeys` member
- `src/wallet/wallet.cpp`: `CWallet::SignTransaction` iterates SPKMs
- `src/miner.cpp`: `CreateCoinStake` converts P2PKH to P2PK outputs
- `test/functional/feature_descriptor_legacy_staking.py`: Multi-node interop test with UTXO exhaustion validation
