#!/bin/bash
# Reddcoin Regtest Launch Procedure
# This script launches regtest with proper PoW and PoS block generation including mock time handling

set -e

REDDCOIN_CLI="./src/reddcoin-cli -regtest"
REDDCOIND="./src/reddcoind -regtest"

echo "=== Reddcoin Regtest Launch Procedure ==="
echo ""

# Step 1: Stop any running regtest node
echo "Step 1: Stopping any running regtest node..."
$REDDCOIN_CLI stop 2>/dev/null || echo "No node running"
sleep 2

# Step 2: Start regtest daemon
echo ""
echo "Step 2: Starting regtest daemon..."
$REDDCOIND -daemon
sleep 3

# Step 3: Check node is running
echo ""
echo "Step 3: Verifying node is running..."
$REDDCOIN_CLI getblockchaininfo | grep -E '"chain"|"blocks"'

# Step 4: Create or load wallet
echo ""
echo "Step 4: Creating/loading wallet..."
WALLET_NAME="regtest_wallet"
if $REDDCOIN_CLI listwallets | grep -q "$WALLET_NAME"; then
    echo "Wallet '$WALLET_NAME' already loaded"
else
    # Try to load existing wallet, or create new one
    $REDDCOIN_CLI loadwallet "$WALLET_NAME" 2>/dev/null || \
    $REDDCOIN_CLI createwallet "$WALLET_NAME"
fi

# Step 5: Get mining address
echo ""
echo "Step 5: Getting mining address..."
MINING_ADDR=$($REDDCOIN_CLI -rpcwallet="$WALLET_NAME" getnewaddress "mining")
echo "Mining address: $MINING_ADDR"

# Step 6: Generate PoW blocks with proper time spacing
echo ""
echo "Step 6: Generating 89 PoW blocks with proper time spacing..."
CURRENT_HEIGHT=$($REDDCOIN_CLI getblockcount)
echo "Current height: $CURRENT_HEIGHT"

if [ $CURRENT_HEIGHT -eq 0 ]; then
    echo "Mining 89 PoW blocks (blocks 1-89)..."

    # Get genesis time
    GENESIS_TIME=$($REDDCOIN_CLI getblockheader $($REDDCOIN_CLI getblockhash 0) | jq -r '.time')
    echo "Genesis time: $GENESIS_TIME"

    # Mine 89 PoW blocks with 60-second spacing
    for i in {1..89}; do
        NEW_TIME=$((GENESIS_TIME + (i * 60)))
        $REDDCOIN_CLI setmocktime $NEW_TIME > /dev/null
        $REDDCOIN_CLI generatetoaddress 1 "$MINING_ADDR" > /dev/null

        if [ $((i % 10)) -eq 0 ]; then
            echo "  Mined block $i/89"
        fi
    done

    echo "Reached height: $($REDDCOIN_CLI getblockcount)"
fi

# Step 7: Check wallet balance
echo ""
echo "Step 7: Checking wallet balance..."
BALANCE=$($REDDCOIN_CLI -rpcwallet="$WALLET_NAME" getbalance)
echo "Wallet balance: $BALANCE RDD"

# Step 8: Advance mock time for stake modifier selection
echo ""
echo "Step 8: Advancing mock time for stake modifier selection..."
BLOCK89_TIME=$($REDDCOIN_CLI getblockheader $($REDDCOIN_CLI getblockhash 89) | jq -r '.time')
STAKE_TIME=$((BLOCK89_TIME + 2580))  # Add 43 minutes (2580 seconds)
$REDDCOIN_CLI setmocktime $STAKE_TIME
echo "Mock time advanced to: $STAKE_TIME (block 89 time + 2580 seconds)"

# Step 9: Enable staking for PoS blocks
echo ""
echo "Step 9: Enabling staking for PoS block generation..."
$REDDCOIN_CLI setstaking true
STAKING_ENABLED=$($REDDCOIN_CLI getstakinginfo | jq -r '.enabled')
echo "Staking enabled: $STAKING_ENABLED"

# Wait a moment and check if block 90 was automatically created
echo "Waiting for automatic block 90 generation..."
sleep 5

CURRENT_HEIGHT=$($REDDCOIN_CLI getblockcount)
if [ $CURRENT_HEIGHT -eq 90 ]; then
    BLOCK_HASH=$($REDDCOIN_CLI getblockhash 90)
    BLOCK_TYPE=$($REDDCOIN_CLI getblock "$BLOCK_HASH" 1 | jq -r '.type')
    echo "✓ Block 90 automatically staked ($BLOCK_TYPE)"
elif [ $CURRENT_HEIGHT -eq 89 ]; then
    echo "⚠ Block 90 not generated automatically, will generate manually..."
fi

# Step 10: Generate remaining PoS blocks (91-100 or 90-100)
echo ""
echo "Step 10: Generating remaining PoS blocks to height 100..."
CURRENT_HEIGHT=$($REDDCOIN_CLI getblockcount)
TARGET_HEIGHT=100
BLOCKS_TO_GENERATE=$((TARGET_HEIGHT - CURRENT_HEIGHT))

if [ $BLOCKS_TO_GENERATE -gt 0 ]; then
    echo "Generating $BLOCKS_TO_GENERATE PoS blocks (height $((CURRENT_HEIGHT + 1)) to $TARGET_HEIGHT)..."

    # Advance mock time by 10 seconds before generating next block (faster for regtest)
    CURRENT_TIME=$($REDDCOIN_CLI getblockheader $($REDDCOIN_CLI getblockhash $CURRENT_HEIGHT) | jq -r '.time')
    NEXT_TIME=$((CURRENT_TIME + 10))
    $REDDCOIN_CLI setmocktime $NEXT_TIME > /dev/null
    echo "Mock time advanced to prepare for block $((CURRENT_HEIGHT + 1))"

    for i in $(seq 1 $BLOCKS_TO_GENERATE); do
        CURRENT_HEIGHT=$($REDDCOIN_CLI getblockcount)
        CURRENT_TIME=$($REDDCOIN_CLI getblockheader $($REDDCOIN_CLI getblockhash $CURRENT_HEIGHT) | jq -r '.time')
        NEW_TIME=$((CURRENT_TIME + 10))
        $REDDCOIN_CLI setmocktime $NEW_TIME > /dev/null

        echo "  Waiting for PoS block $i/$BLOCKS_TO_GENERATE (height $((CURRENT_HEIGHT + 1)))..."

        # Wait for new block (with timeout)
        TIMEOUT=120
        ELAPSED=0
        while [ $($REDDCOIN_CLI getblockcount) -eq $CURRENT_HEIGHT ] && [ $ELAPSED -lt $TIMEOUT ]; do
            sleep 1
            ELAPSED=$((ELAPSED + 1))
        done

        NEW_HEIGHT=$($REDDCOIN_CLI getblockcount)
        if [ $NEW_HEIGHT -gt $CURRENT_HEIGHT ]; then
            BLOCK_HASH=$($REDDCOIN_CLI getblockhash $NEW_HEIGHT)
            BLOCK_TYPE=$($REDDCOIN_CLI getblock "$BLOCK_HASH" 1 | jq -r '.type')
            echo "  ✓ Block $NEW_HEIGHT staked ($BLOCK_TYPE)"
        else
            echo "  ✗ Timeout waiting for block $i"
            break
        fi
    done
else
    echo "Already at target height $CURRENT_HEIGHT"
fi

FINAL_HEIGHT=$($REDDCOIN_CLI getblockcount)
echo ""
echo "Final height: $FINAL_HEIGHT"

# Step 11: Summary
echo ""
echo "=== Regtest Launch Complete ==="
echo "Chain: regtest"
echo "Current height: $($REDDCOIN_CLI getblockcount)"
echo "Mining address: $MINING_ADDR"
echo "Wallet balance: $($REDDCOIN_CLI -rpcwallet="$WALLET_NAME" getbalance) RDD"
echo "Staking enabled: $($REDDCOIN_CLI getstakinginfo | jq -r '.enabled')"
echo "Mock time: $($REDDCOIN_CLI getblockheader $($REDDCOIN_CLI getbestblockhash) | jq -r '.time')"
echo ""
echo "=== Useful Commands ===="
echo "Check blockchain info:  $REDDCOIN_CLI getblockchaininfo"
echo "Check staking info:     $REDDCOIN_CLI getstakinginfo"
echo "Generate more PoS blocks (requires time advancement):"
echo "  CURRENT_TIME=\$($REDDCOIN_CLI getblockheader \$($REDDCOIN_CLI getbestblockhash) | jq -r '.time')"
echo "  $REDDCOIN_CLI setmocktime \$((CURRENT_TIME + 60))"
echo "  # Wait 5-10 seconds for block to stake"
echo "Stop node:              $REDDCOIN_CLI stop"
echo ""
