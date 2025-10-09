# Reddcoin Regtest Guide

This guide covers launching and testing Reddcoin in regtest mode with both PoW (Proof of Work) and PoS (Proof of Stake) block generation.

## What's New

**Fast PoS Generation**: Regtest PoS blocks generate quickly (1-5 seconds) after advancing mock time. Use 10-second time intervals between blocks for optimal performance.

**Mock Time Management**: Regtest PoS staking requires careful mock time management due to the stake modifier selection interval. This guide shows you how to properly advance time for smooth PoS block generation.

**Updated Heights**: Regtest now uses `nLastPowHeight = 89`, making the PoW/PoS transition happen earlier for faster testing with TestChain100Setup compatibility.

## Quick Start

Run the automated launch script:
```bash
./regtest_launch.sh
```

This script will:
1. Start regtest node
2. Create/load wallet
3. Mine 89 PoW blocks with proper time spacing
4. Advance mock time by 43 minutes for stake modifier selection
5. Enable staking
6. Generate 11 PoS blocks (90-100) with automatic time advancement

## Manual Procedure

### 1. Start Regtest Node

```bash
./src/reddcoind -regtest -daemon
```

Wait a few seconds for the node to start, then verify:
```bash
./src/reddcoin-cli -regtest getblockchaininfo
```

### 2. Create/Load Wallet

Create a new wallet:
```bash
./src/reddcoin-cli -regtest createwallet "regtest_wallet"
```

Or load existing wallet:
```bash
./src/reddcoin-cli -regtest loadwallet "regtest_wallet"
```

### 3. Generate Mining Address

```bash
MINING_ADDR=$(./src/reddcoin-cli -regtest -rpcwallet=regtest_wallet getnewaddress "mining")
echo $MINING_ADDR
```

Save this address for mining blocks.

### 4. Generate PoW Blocks (Height 1-89) with Time Spacing

**Important:** PoW blocks must be generated with proper time spacing for stake modifier calculations to work later.

```bash
# Get genesis time
GENESIS_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash 0) | jq -r '.time')

# Generate 89 PoW blocks with 10-second spacing (faster for regtest)
for i in {1..89}; do
    NEW_TIME=$((GENESIS_TIME + (i * 10)))
    ./src/reddcoin-cli -regtest setmocktime $NEW_TIME
    ./src/reddcoin-cli -regtest generatetoaddress 1 "$MINING_ADDR"
done
```

Check current height:
```bash
./src/reddcoin-cli -regtest getblockcount
```

### 5. Advance Mock Time for Stake Modifier Selection

**Critical Step:** Before enabling staking, advance mock time by the stake modifier selection interval (approximately 43 minutes).

```bash
# Get block 89 timestamp
BLOCK89_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash 89) | jq -r '.time')

# Add 2580 seconds (43 minutes) for stake modifier selection interval
STAKE_TIME=$((BLOCK89_TIME + 2580))
./src/reddcoin-cli -regtest setmocktime $STAKE_TIME

echo "Mock time advanced to: $STAKE_TIME"
```

**Why 2580 seconds?**
- `nModifierInterval = 60` seconds
- Stake modifier selection algorithm requires ~64 * modifier interval * ratio
- This creates a selection interval of approximately 2580 seconds (43 minutes)
- The staker needs to look ahead from UTXO blocks by this interval

### 6. Enable Staking

Before generating PoS blocks, enable staking:
```bash
./src/reddcoin-cli -regtest setstaking true
```

Verify staking is enabled:
```bash
./src/reddcoin-cli -regtest getstakinginfo
```

### 7. Generate PoS Blocks (Height 90+)

After height 89, blocks are Proof of Stake. **PoS blocks require advancing mock time** between each block.

#### Manual Method (One block at a time)
```bash
# For each PoS block:
CURRENT_HEIGHT=$(./src/reddcoin-cli -regtest getblockcount)
CURRENT_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash $CURRENT_HEIGHT) | jq -r '.time')

# Advance time by 10 seconds
NEW_TIME=$((CURRENT_TIME + 10))
./src/reddcoin-cli -regtest setmocktime $NEW_TIME

# Wait for background staker to create block (typically 1-5 seconds)
sleep 5

# Verify new block
./src/reddcoin-cli -regtest getblockcount
```

#### Automated Method (Multiple blocks)
```bash
generate_pos_blocks() {
    local NUM_BLOCKS=$1
    local BLOCK_INTERVAL=10  # 10 seconds for fast regtest generation

    for i in $(seq 1 $NUM_BLOCKS); do
        CURRENT_HEIGHT=$(./src/reddcoin-cli -regtest getblockcount)
        CURRENT_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash $CURRENT_HEIGHT) | jq -r '.time')
        NEW_TIME=$((CURRENT_TIME + BLOCK_INTERVAL))
        ./src/reddcoin-cli -regtest setmocktime $NEW_TIME

        echo "Waiting for PoS block $i/$NUM_BLOCKS (height $((CURRENT_HEIGHT + 1)))..."

        # Wait for new block (typically 1-5 seconds in regtest)
        TIMEOUT=30
        ELAPSED=0
        START_HEIGHT=$CURRENT_HEIGHT
        while [ $(./src/reddcoin-cli -regtest getblockcount) -eq $START_HEIGHT ] && [ $ELAPSED -lt $TIMEOUT ]; do
            sleep 1
            ELAPSED=$((ELAPSED + 1))
        done

        NEW_HEIGHT=$(./src/reddcoin-cli -regtest getblockcount)
        if [ $NEW_HEIGHT -gt $CURRENT_HEIGHT ]; then
            echo "✓ PoS block staked at height $NEW_HEIGHT (${ELAPSED}s)"
        else
            echo "✗ Timeout waiting for block $i"
            return 1
        fi
    done
}

# Generate 11 PoS blocks (90-100)
generate_pos_blocks 11
```

### 8. Verify Block Types

Check if a specific block is PoW or PoS:
```bash
# Get block hash
BLOCK_HASH=$(./src/reddcoin-cli -regtest getblockhash 90)

# Get block info
./src/reddcoin-cli -regtest getblock "$BLOCK_HASH" 1 | grep '"type"'
```

Output will show:
- PoW: `"type": "PoW",`
- PoS: `"type": "PoS",`

You can also check multiple blocks at once:
```bash
# Check blocks 88-92 (around the PoW/PoS transition)
for height in 88 89 90 91 92; do
  HASH=$(./src/reddcoin-cli -regtest getblockhash $height)
  TYPE=$(./src/reddcoin-cli -regtest getblock "$HASH" 1 | jq -r '.type')
  echo "Block $height: $TYPE"
done
```

## Configuration Details

### Regtest Chainparams (src/chainparams.cpp)

Key regtest configuration:
```cpp
consensus.nLastPowHeight = 89;         // Last PoW block height
consensus.nCoinbaseMaturity = 60;      // Blocks before coinbase spendable
consensus.nStakeMinAge = 10;           // 10 seconds (fast for testing)
consensus.nModifierInterval = 60;      // 60 seconds for stake modifier
consensus.fRequireStandard = false;    // Allow non-standard transactions
m_is_test_chain = true;                // Enable test features
m_is_mockable_chain = true;            // Allow time mocking
```

### Network Message Bytes

Regtest uses unique message start bytes to prevent cross-network communication:
```cpp
pchMessageStart[0] = 0xfa;
pchMessageStart[1] = 0xbf;
pchMessageStart[2] = 0xb5;
pchMessageStart[3] = 0xda;
```

### Genesis Block

Regtest uses a different genesis block than mainnet:
- Genesis hash: `e817774b5fd9808e7e03a557a43ec2a37f35ea4cf38550a7ce4f414531e28ef6`
- Genesis time: 1642570147 (Unix timestamp)
- Genesis nonce: 36529
- Difficulty bits: 0x207fffff

### Mining Difficulty

Regtest uses minimum difficulty for fast block generation:
```cpp
consensus.powLimit = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
consensus.fPowNoRetargeting = true;  // No difficulty adjustment
```

## Testing Scenarios

### Test 1: Basic PoW Mining
```bash
# Start fresh
./src/reddcoin-cli -regtest stop
rm -rf ~/.reddcoin/regtest
./src/reddcoind -regtest -daemon
sleep 2

# Create wallet and mine
./src/reddcoin-cli -regtest createwallet "test"
ADDR=$(./src/reddcoin-cli -regtest -rpcwallet=test getnewaddress)

# Mine with time spacing
GENESIS_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash 0) | jq -r '.time')
for i in {1..50}; do
    NEW_TIME=$((GENESIS_TIME + (i * 60)))
    ./src/reddcoin-cli -regtest setmocktime $NEW_TIME
    ./src/reddcoin-cli -regtest generatetoaddress 1 "$ADDR"
done
```

### Test 2: PoW to PoS Transition
```bash
# Mine to height 89 (PoW)
ADDR=$(./src/reddcoin-cli -regtest -rpcwallet=test getnewaddress)
GENESIS_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash 0) | jq -r '.time')

for i in {1..89}; do
    NEW_TIME=$((GENESIS_TIME + (i * 60)))
    ./src/reddcoin-cli -regtest setmocktime $NEW_TIME
    ./src/reddcoin-cli -regtest generatetoaddress 1 "$ADDR"
done

# Advance time for stake modifier selection
BLOCK89_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash 89) | jq -r '.time')
STAKE_TIME=$((BLOCK89_TIME + 2580))
./src/reddcoin-cli -regtest setmocktime $STAKE_TIME

# Enable staking
./src/reddcoin-cli -regtest setstaking true

# Generate PoS blocks (use the generate_pos_blocks function from above)
generate_pos_blocks 10

# Verify blocks 90+ are PoS
for h in {90..99}; do
  HASH=$(./src/reddcoin-cli -regtest getblockhash $h)
  TYPE=$(./src/reddcoin-cli -regtest getblock "$HASH" 1 | jq -r '.type')
  echo "Block $h: $TYPE"
done
```

### Test 3: Transaction Testing
```bash
# Create two wallets
./src/reddcoin-cli -regtest createwallet "wallet1"
./src/reddcoin-cli -regtest createwallet "wallet2"

# Mine blocks to wallet1
ADDR1=$(./src/reddcoin-cli -regtest -rpcwallet=wallet1 getnewaddress)
GENESIS_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash 0) | jq -r '.time')

for i in {1..101}; do
    NEW_TIME=$((GENESIS_TIME + (i * 60)))
    ./src/reddcoin-cli -regtest setmocktime $NEW_TIME
    ./src/reddcoin-cli -regtest generatetoaddress 1 "$ADDR1"
done

# Check balance
./src/reddcoin-cli -regtest -rpcwallet=wallet1 getbalance

# Send to wallet2
ADDR2=$(./src/reddcoin-cli -regtest -rpcwallet=wallet2 getnewaddress)
./src/reddcoin-cli -regtest -rpcwallet=wallet1 sendtoaddress "$ADDR2" 1000

# Mine block to confirm
CURRENT_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getbestblockhash) | jq -r '.time')
NEW_TIME=$((CURRENT_TIME + 60))
./src/reddcoin-cli -regtest setmocktime $NEW_TIME
./src/reddcoin-cli -regtest generatetoaddress 1 "$ADDR1"

# Verify receipt
./src/reddcoin-cli -regtest -rpcwallet=wallet2 getbalance
```

## Useful Commands

### Blockchain Info
```bash
# Chain overview
./src/reddcoin-cli -regtest getblockchaininfo

# Current height
./src/reddcoin-cli -regtest getblockcount

# Best block hash
./src/reddcoin-cli -regtest getbestblockhash

# Get block by height
./src/reddcoin-cli -regtest getblockhash <height>

# Get block details
./src/reddcoin-cli -regtest getblock <hash> 1
```

### Staking Info
```bash
# Staking status
./src/reddcoin-cli -regtest getstakinginfo

# Enable staking
./src/reddcoin-cli -regtest setstaking true

# Disable staking
./src/reddcoin-cli -regtest setstaking false
```

### Wallet Operations
```bash
# List wallets
./src/reddcoin-cli -regtest listwallets

# Get balance
./src/reddcoin-cli -regtest -rpcwallet=<name> getbalance

# Get new address
./src/reddcoin-cli -regtest -rpcwallet=<name> getnewaddress

# List transactions
./src/reddcoin-cli -regtest -rpcwallet=<name> listtransactions
```

### Mock Time Operations
```bash
# Get current mock time
./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getbestblockhash) | jq -r '.time'

# Set mock time (Unix timestamp)
./src/reddcoin-cli -regtest setmocktime <timestamp>

# Reset to system time
./src/reddcoin-cli -regtest setmocktime 0
```

### Mempool Operations
```bash
# View mempool
./src/reddcoin-cli -regtest getrawmempool

# Get mempool info
./src/reddcoin-cli -regtest getmempoolinfo
```

## Troubleshooting

### Issue: "unable to determine stakemodifier" Error
**Problem:** Stake modifier calculation fails
```
ERROR: CheckStakeKernelHash() : unable to determine stakemodifier nStakeModifier=0
```

**Cause:** Not enough blockchain time for stake modifier selection algorithm

**Solution:**
```bash
# Get current tip time
CURRENT_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getbestblockhash) | jq -r '.time')

# Add 2580 seconds (43 minutes)
./src/reddcoin-cli -regtest setmocktime $((CURRENT_TIME + 2580))

# Enable staking
./src/reddcoin-cli -regtest setstaking true
```

### Issue: "pow-ended" Error
**Problem:** Trying to generate PoW blocks after height 89
```
ERROR: ConnectTip: ConnectBlock ... failed, pow-ended
```

**Solution:** Enable staking for PoS blocks:
```bash
# Enable staking
./src/reddcoin-cli -regtest setstaking true

# Advance mock time
CURRENT_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getbestblockhash) | jq -r '.time')
./src/reddcoin-cli -regtest setmocktime $((CURRENT_TIME + 60))

# Wait for PoS block (5-10 seconds)
```

### Issue: PoS Blocks Not Generating
**Problem:** Staking enabled but no blocks being created

**Causes:**
1. Mock time not advanced
2. Coins haven't aged enough (need > 10 seconds)
3. Insufficient wallet balance
4. Stake modifier selection interval not satisfied

**Solution:**
```bash
# Check staking is enabled
./src/reddcoin-cli -regtest getstakinginfo

# Check wallet balance
./src/reddcoin-cli -regtest -rpcwallet=<wallet_name> getbalance

# Advance mock time
CURRENT_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getbestblockhash) | jq -r '.time')
./src/reddcoin-cli -regtest setmocktime $((CURRENT_TIME + 60))

# Wait 5-10 seconds
sleep 10

# Check for new block
./src/reddcoin-cli -regtest getblockcount
```

### Issue: "acceptnonstdtxn is not currently supported for main chain"
**Problem:** Node starting with mainnet config instead of regtest

**Solution:** Ensure you're using `-regtest` flag:
```bash
./src/reddcoind -regtest -daemon
./src/reddcoin-cli -regtest <command>
```

### Issue: Wallet Not Loaded
**Problem:** "Requested wallet does not exist or is not loaded"

**Solution:**
```bash
./src/reddcoin-cli -regtest loadwallet <wallet_name>
```

Or create new wallet:
```bash
./src/reddcoin-cli -regtest createwallet <wallet_name>
```

## Data Directories

Regtest data is stored separately from mainnet:
```
~/.reddcoin/regtest/
├── blocks/           # Block data
├── chainstate/       # UTXO database
├── wallets/          # Wallet files
├── debug.log         # Debug log
├── peers.dat         # Peer information
└── settings.json     # Runtime settings
```

To reset regtest:
```bash
./src/reddcoin-cli -regtest stop
rm -rf ~/.reddcoin/regtest
```

## Advanced Usage

### Mock Time and Stake Modifiers

**Understanding Stake Modifier Selection:**
- Stake modifiers are generated every `nModifierInterval` (60 seconds in regtest)
- The selection interval is approximately 64 × modifier interval × ratio ≈ 2580 seconds
- When staking, the algorithm looks ahead from the UTXO block by this selection interval
- Without sufficient blockchain time, the algorithm can't find valid modifiers

**Best Practices:**
1. Mine PoW blocks with 10-second time spacing for faster chain building
2. After mining to height 89, add 2580 seconds before enabling staking
3. Advance time by 10 seconds between each PoS block
4. PoS blocks typically appear within 1-5 seconds after advancing mock time
5. Use the automated `generate_pos_blocks` function for consistency

### Running 100 Blocks Example

Complete example to generate 100 blocks:
```bash
#!/bin/bash
# Generate 100 blocks in regtest (89 PoW + 11 PoS)

# Start node
./src/reddcoind -regtest -daemon
sleep 2

# Create wallet
./src/reddcoin-cli -regtest createwallet "test100"
ADDR=$(./src/reddcoin-cli -regtest -rpcwallet=test100 getnewaddress)

# Get genesis time
GENESIS_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash 0) | jq -r '.time')

# Generate 89 PoW blocks with 10-second spacing
echo "Generating 89 PoW blocks..."
for i in {1..89}; do
    NEW_TIME=$((GENESIS_TIME + (i * 10)))
    ./src/reddcoin-cli -regtest setmocktime $NEW_TIME > /dev/null
    ./src/reddcoin-cli -regtest generatetoaddress 1 "$ADDR" > /dev/null
done

# Advance time for stake modifier selection
BLOCK89_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash 89) | jq -r '.time')
STAKE_TIME=$((BLOCK89_TIME + 2580))
./src/reddcoin-cli -regtest setmocktime $STAKE_TIME

# Enable staking
echo "Enabling staking..."
./src/reddcoin-cli -regtest setstaking true

# Wait for block 90 to auto-generate
sleep 5

# Generate remaining PoS blocks (91-100)
echo "Generating PoS blocks to height 100..."
CURRENT_HEIGHT=$(./src/reddcoin-cli -regtest getblockcount)
TARGET_HEIGHT=100

while [ $CURRENT_HEIGHT -lt $TARGET_HEIGHT ]; do
    CURRENT_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash $CURRENT_HEIGHT) | jq -r '.time')
    NEW_TIME=$((CURRENT_TIME + 10))
    ./src/reddcoin-cli -regtest setmocktime $NEW_TIME > /dev/null

    sleep 5  # Wait for staker (typically 1-5 seconds)

    NEW_HEIGHT=$(./src/reddcoin-cli -regtest getblockcount)
    if [ $NEW_HEIGHT -gt $CURRENT_HEIGHT ]; then
        echo "  ✓ Block $NEW_HEIGHT staked"
        CURRENT_HEIGHT=$NEW_HEIGHT
    fi
done

# Verify
echo "Final height: $(./src/reddcoin-cli -regtest getblockcount)"
echo "Balance: $(./src/reddcoin-cli -regtest -rpcwallet=test100 getbalance)"
```

## See Also

- [Bitcoin Regtest Documentation](https://developer.bitcoin.org/examples/testing.html#regtest-mode)
- [Reddcoin Source Code](https://github.com/reddcoin-project/reddcoin)
- Quick Reference: `REGTEST_QUICKREF.md`
- Main Documentation: `doc/` directory
