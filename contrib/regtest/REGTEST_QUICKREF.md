# Reddcoin Regtest Quick Reference

## Launch Sequence

```bash
# 1. Start node
./src/reddcoind -regtest -daemon

# 2. Create/load wallet
./src/reddcoin-cli -regtest createwallet "mytest"

# 3. Get address and genesis time
ADDR=$(./src/reddcoin-cli -regtest -rpcwallet=mytest getnewaddress)
GENESIS_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash 0) | jq -r '.time')

# 4. Generate PoW blocks (1-89) with proper time spacing
# Use 10-second intervals for faster chain building
for i in {1..89}; do
    NEW_TIME=$((GENESIS_TIME + (i * 10)))
    ./src/reddcoin-cli -regtest setmocktime $NEW_TIME
    ./src/reddcoin-cli -regtest generatetoaddress 1 "$ADDR"
done

# 5. Add time for stake modifier selection interval (43 minutes)
BLOCK89_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash 89) | jq -r '.time')
STAKE_TIME=$((BLOCK89_TIME + 2580))
./src/reddcoin-cli -regtest setmocktime $STAKE_TIME

# 6. Enable staking
./src/reddcoin-cli -regtest setstaking true

# 7. Generate PoS blocks (90+) - requires time advancement
# See "Generating PoS Blocks" section below
```

## Key Heights

| Height | Block Type | Notes |
|--------|------------|-------|
| 0      | Genesis    | Special genesis block |
| 1-89   | PoW        | Proof of Work mining |
| 90+    | PoS        | Proof of Stake (requires staking enabled + mock time advancement) |

## Generating PoS Blocks

**Important:** PoS block generation requires advancing mock time between blocks.

### Manual Method
```bash
# For each PoS block:
CURRENT_HEIGHT=$(./src/reddcoin-cli -regtest getblockcount)
CURRENT_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash $CURRENT_HEIGHT) | jq -r '.time')
NEW_TIME=$((CURRENT_TIME + 10))
./src/reddcoin-cli -regtest setmocktime $NEW_TIME

# Wait for staker to create block (typically 1-5 seconds)
sleep 5
```

### Automated Function
```bash
generate_pos_blocks() {
    local NUM_BLOCKS=$1
    local BLOCK_INTERVAL=10  # 10 seconds for fast regtest generation

    for i in $(seq 1 $NUM_BLOCKS); do
        CURRENT_HEIGHT=$(./src/reddcoin-cli -regtest getblockcount)
        CURRENT_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash $CURRENT_HEIGHT) | jq -r '.time')
        NEW_TIME=$((CURRENT_TIME + BLOCK_INTERVAL))
        ./src/reddcoin-cli -regtest setmocktime $NEW_TIME

        echo "Waiting for PoS block $i/$NUM_BLOCKS..."

        # Wait for block (typically 1-5 seconds in regtest)
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
            echo "✗ Timeout waiting for block"
            return 1
        fi
    done
}

# Generate 11 PoS blocks (90-100)
generate_pos_blocks 11
```

## Essential Commands

### Node Control
```bash
./src/reddcoind -regtest -daemon              # Start
./src/reddcoin-cli -regtest stop              # Stop
./src/reddcoin-cli -regtest getblockcount     # Check height
```

### Wallet
```bash
./src/reddcoin-cli -regtest createwallet "name"
./src/reddcoin-cli -regtest loadwallet "name"
./src/reddcoin-cli -regtest -rpcwallet=name getnewaddress
./src/reddcoin-cli -regtest -rpcwallet=name getbalance
```

### Mining & Staking
```bash
# Generate PoW blocks (heights 1-89)
./src/reddcoin-cli -regtest generatetoaddress <num> <address>

# Enable/disable staking (required for PoS blocks after height 89)
./src/reddcoin-cli -regtest setstaking true|false

# Check staking status
./src/reddcoin-cli -regtest getstakinginfo

# Advance mock time (required for PoS staking)
./src/reddcoin-cli -regtest setmocktime <unix_timestamp>
```

### Transactions
```bash
./src/reddcoin-cli -regtest -rpcwallet=name sendtoaddress <addr> <amount>
./src/reddcoin-cli -regtest getrawmempool
./src/reddcoin-cli -regtest -rpcwallet=name listtransactions
```

## Configuration

**File:** `src/chainparams.cpp` (CRegTestParams)

```cpp
consensus.nLastPowHeight = 89;          // PoW ends at block 89
consensus.nCoinbaseMaturity = 60;       // Maturity time
consensus.nStakeMinAge = 10;            // 10 seconds for PoS
consensus.nModifierInterval = 60;       // 60 seconds for stake modifier
fRequireStandard = false;               // Allow non-standard tx
m_is_test_chain = true;                 // Enable test features
m_is_mockable_chain = true;             // Allow mock time
```

**Genesis Block:**
- Hash: `e817774b5fd9808e7e03a557a43ec2a37f35ea4cf38550a7ce4f414531e28ef6`
- Time: 1642570147
- Nonce: 36529

## Mock Time Requirements

**Why is mock time needed?**
- `nModifierInterval = 60` seconds creates a stake modifier selection interval of ~43 minutes (2580 seconds)
- The staker needs to look ahead from UTXO block by this interval to find valid stake modifiers
- Without advancing mock time, the blockchain doesn't have enough "time" for stake modifier calculations

**Key Times:**
1. **After mining 89 PoW blocks:** Add 2580 seconds (43 minutes) to enable staking
2. **Between PoS blocks:** Add 10 seconds per block for fast regtest generation
3. **Block generation time:** PoS blocks typically appear within 1-5 seconds after advancing mock time

## Quick Reset

```bash
./src/reddcoin-cli -regtest stop
rm -rf ~/.reddcoin/regtest
./src/reddcoind -regtest -daemon
```

## Common Issues

| Error | Cause | Fix |
|-------|-------|-----|
| `unable to determine stakemodifier` | Not enough blockchain time | Advance mock time by 2580 seconds after block 89 |
| `pow-ended` | Trying PoW after height 89 | Enable staking with `setstaking true` |
| `no valid coinstake found` | Coins need more age or mock time not advanced | Wait 10+ seconds, advance mock time |
| `acceptnonstdtxn not supported` | Wrong network | Use `-regtest` flag |
| PoS blocks not generating | Mock time not advancing | Use setmocktime before each expected block |

## Testing PoW→PoS Transition

Test the critical transition from block 89 (PoW) to block 90 (PoS):

```bash
# Start fresh
./src/reddcoin-cli -regtest stop
rm -rf ~/.reddcoin/regtest
./src/reddcoind -regtest -daemon
sleep 2

# Create wallet
./src/reddcoin-cli -regtest createwallet "transition_test"
ADDR=$(./src/reddcoin-cli -regtest -rpcwallet=transition_test getnewaddress)
GENESIS_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash 0) | jq -r '.time')

# Mine exactly to block 89 (10-second intervals)
for i in {1..89}; do
    NEW_TIME=$((GENESIS_TIME + (i * 10)))
    ./src/reddcoin-cli -regtest setmocktime $NEW_TIME > /dev/null
    ./src/reddcoin-cli -regtest generatetoaddress 1 "$ADDR" > /dev/null
done

echo "=== At block 89 (last PoW block) ==="
echo "Height: $(./src/reddcoin-cli -regtest getblockcount)"

# Verify block 89 is PoW
BLOCK89_HASH=$(./src/reddcoin-cli -regtest getblockhash 89)
BLOCK89_TYPE=$(./src/reddcoin-cli -regtest getblock "$BLOCK89_HASH" 1 | jq -r '.type')
echo "Block 89 type: $BLOCK89_TYPE"

# Advance mock time for stake modifier
BLOCK89_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash 89) | jq -r '.time')
STAKE_TIME=$((BLOCK89_TIME + 2580))
./src/reddcoin-cli -regtest setmocktime $STAKE_TIME
echo "Mock time advanced by 2580 seconds"

# Enable staking and wait for block 90
echo "Enabling staking..."
./src/reddcoin-cli -regtest setstaking true
sleep 5

# Check if block 90 was created
HEIGHT_AFTER=$(./src/reddcoin-cli -regtest getblockcount)
echo ""
echo "=== After enabling staking ==="
echo "Height: $HEIGHT_AFTER"

if [ $HEIGHT_AFTER -eq 90 ]; then
    BLOCK90_HASH=$(./src/reddcoin-cli -regtest getblockhash 90)
    BLOCK90_TYPE=$(./src/reddcoin-cli -regtest getblock "$BLOCK90_HASH" 1 | jq -r '.type')
    echo "✓ Block 90 auto-generated: $BLOCK90_TYPE"

    # Verify it's PoS
    if [ "$BLOCK90_TYPE" = "PoS" ]; then
        echo "✓ PoW→PoS transition successful!"
    else
        echo "✗ ERROR: Block 90 is not PoS!"
    fi
else
    echo "⚠ Block 90 not auto-generated (height still $HEIGHT_AFTER)"
    echo "Manually advancing time and waiting..."
    CURRENT_TIME=$(./src/reddcoin-cli -regtest getblockheader $(./src/reddcoin-cli -regtest getblockhash $HEIGHT_AFTER) | jq -r '.time')
    ./src/reddcoin-cli -regtest setmocktime $((CURRENT_TIME + 10))
    sleep 5
    echo "Height now: $(./src/reddcoin-cli -regtest getblockcount)"
fi
```

## Automated Script

```bash
./regtest_launch.sh
```

See `REGTEST_GUIDE.md` for full documentation.
