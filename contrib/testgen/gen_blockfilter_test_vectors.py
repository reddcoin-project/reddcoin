#!/usr/bin/env python3
# Copyright (c) 2020-2023 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
'''
Generate block filter test vectors for Reddcoin.

This script fetches block data from a running Reddcoin node and creates test
vectors for the blockfilter tests. The vectors include the block data, previous
output scripts, and expected filter values.

Usage:
    # Generate vectors for specific blocks
    ./gen_blockfilter_test_vectors.py 100 1000 5000 > ../../src/test/data/blockfilters.json

    # Use default interesting blocks
    ./gen_blockfilter_test_vectors.py > ../../src/test/data/blockfilters.json

Requirements:
    - Running reddcoind node with RPC enabled
    - reddcoin-cli in PATH or ../src/reddcoin-cli
'''

import subprocess
import json
import sys
import os
from typing import List, Dict, Any, Optional

# Default interesting blocks to test
DEFAULT_BLOCKS = [
    0,       # Genesis block
    1,       # First premine block (545M RDD)
    2,
    3,
    4,
    5,       # Mid premine block
    100,     # Early PoW block with simple coinbase
    1000,    # Block with transactions
    5000,    # Another transaction block
    10000,   # Bonus period boundary (200k reward)
    260800,  # First PoS block
]

class ReddcoinCLI:
    """Wrapper for reddcoin-cli commands"""

    def __init__(self, cli_path: str = None):
        if cli_path:
            self.cli = cli_path
        elif os.path.exists("./src/reddcoin-cli"):
            self.cli = "./src/reddcoin-cli"
        else:
            self.cli = "reddcoin-cli"

    def call(self, *args) -> Any:
        """Execute reddcoin-cli command and return parsed JSON"""
        cmd = [self.cli] + list(args)
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, check=True)
            return json.loads(result.stdout) if result.stdout.strip() else None
        except subprocess.CalledProcessError as e:
            print(f"Error calling {' '.join(cmd)}: {e.stderr}", file=sys.stderr)
            raise
        except json.JSONDecodeError:
            # Return raw output if not JSON (like block hex)
            return result.stdout.strip()

    def getblockhash(self, height: int) -> str:
        """Get block hash at given height"""
        return self.call("getblockhash", str(height))

    def getblock(self, blockhash: str, verbosity: int = 2) -> Any:
        """Get block data (0=hex, 1=json, 2=json with tx details)"""
        return self.call("getblock", blockhash, str(verbosity))

    def getrawtransaction(self, txid: str, verbose: bool = True) -> Any:
        """Get raw transaction"""
        return self.call("getrawtransaction", txid, "1" if verbose else "0")


def get_prev_output_scripts(cli: ReddcoinCLI, block_data: Dict) -> List[str]:
    """
    Extract scriptPubKey hex for all inputs in the block (except coinbase).

    For blockfilter tests, we need the scripts of the UTXOs being spent,
    not the scripts in the current block.
    """
    prev_scripts = []

    # Skip first transaction (coinbase has no inputs)
    for tx in block_data.get('tx', [])[1:]:
        for vin in tx.get('vin', []):
            if 'coinbase' in vin:
                continue

            # Get the transaction being spent
            prev_txid = vin['txid']
            prev_vout = vin['vout']

            try:
                prev_tx = cli.getrawtransaction(prev_txid, True)
                prev_output = prev_tx['vout'][prev_vout]
                script_hex = prev_output['scriptPubKey']['hex']
                prev_scripts.append(script_hex)
            except Exception as e:
                print(f"Warning: Could not fetch prev tx {prev_txid}: {e}", file=sys.stderr)

    return prev_scripts


def compute_filter_header(prev_header: str, filter_hash: str) -> str:
    """
    Compute filter header = Hash(filter_hash || prev_header)

    Note: The actual computation happens in C++ BlockFilter::ComputeHeader().
    This is a placeholder - we'll get the values from running the actual test.
    For now, return empty string to indicate it needs to be computed.
    """
    return ""


def get_block_filter(cli: ReddcoinCLI, blockhash: str) -> Optional[tuple]:
    """
    Try to get block filter from RPC (requires -blockfilterindex=1).
    Returns (filter_hex, header_hex) or None if not available.
    """
    try:
        result = cli.call("getblockfilter", blockhash, "basic")
        return (result.get('filter', ''), result.get('header', ''))
    except Exception:
        # Filter index not enabled or other error
        return None


def generate_test_vector(cli: ReddcoinCLI, height: int, use_filter_index: bool = True) -> Optional[Dict]:
    """Generate a single test vector for the given block height"""

    try:
        # Get block hash and data
        blockhash = cli.getblockhash(height)
        block_hex = cli.getblock(blockhash, 0)  # Raw hex
        block_json = cli.getblock(blockhash, 2)  # Full JSON with tx details

        # Get previous output scripts
        prev_scripts = get_prev_output_scripts(cli, block_json)

        # Get previous block's filter header for proper chaining
        # Genesis block (height 0) has all-zeros as previous header
        prev_header = "0" * 64
        if height > 0:
            try:
                prev_blockhash = cli.getblockhash(height - 1)
                prev_filter_data = get_block_filter(cli, prev_blockhash)
                if prev_filter_data and prev_filter_data[1]:
                    prev_header = prev_filter_data[1]
            except Exception as e:
                print(f"Warning: Could not fetch prev header for block {height}: {e}", file=sys.stderr)

        # Try to get filter from RPC if enabled
        filter_hex = ""
        filter_header = ""
        if use_filter_index:
            filter_data = get_block_filter(cli, blockhash)
            if filter_data:
                filter_hex, filter_header = filter_data

        # Determine test notes
        notes = f"Block {height}"
        tx_count = len(block_json.get('tx', []))
        if tx_count == 1:
            notes += " - Coinbase only"
        else:
            notes += f" - {tx_count} transactions"

        # Build test vector
        vector = [
            height,
            blockhash,
            block_hex,
            prev_scripts,
            prev_header,
            filter_hex,
            filter_header,
            notes
        ]

        return vector

    except Exception as e:
        print(f"Error processing block {height}: {e}", file=sys.stderr)
        return None


def generate_vectors(block_heights: List[int]) -> str:
    """Generate all test vectors and return as JSON string"""

    cli = ReddcoinCLI()

    # Check if blockfilterindex is available
    filter_index_available = False
    try:
        test_hash = cli.getblockhash(100)
        test_filter = get_block_filter(cli, test_hash)
        filter_index_available = test_filter is not None
    except Exception:
        pass

    if filter_index_available:
        print("✓ Block filter index is available - will fetch complete vectors", file=sys.stderr)
    else:
        print("⚠ Block filter index not available (start reddcoind with -blockfilterindex=1)", file=sys.stderr)
        print("  Generating incomplete vectors - filter values must be computed manually", file=sys.stderr)

    vectors = []

    # Header row
    header = [
        "Block Height,Block Hash,Block,[Prev Output Scripts for Block],"
        "Previous Basic Header,Basic Filter,Basic Header,Notes"
    ]
    vectors.append(header)

    # Generate vectors for each block
    for height in sorted(block_heights):
        print(f"Processing block {height}...", file=sys.stderr)
        vector = generate_test_vector(cli, height, use_filter_index=filter_index_available)

        if vector:
            vectors.append(vector)
        else:
            print(f"Skipping block {height} due to errors", file=sys.stderr)

    # Format as pretty JSON
    return json.dumps(vectors, indent=2)


def main():
    """Main entry point"""

    # Parse command line arguments
    if len(sys.argv) > 1:
        try:
            block_heights = [int(arg) for arg in sys.argv[1:]]
        except ValueError:
            print("Error: All arguments must be block heights (integers)", file=sys.stderr)
            print(__doc__, file=sys.stderr)
            sys.exit(1)
    else:
        block_heights = DEFAULT_BLOCKS

    print(f"Generating block filter test vectors for blocks: {block_heights}", file=sys.stderr)
    print("", file=sys.stderr)

    # Generate and output vectors
    output = generate_vectors(block_heights)
    print(output)

    print("\n# Usage notes:", file=sys.stderr)
    print("# - With -blockfilterindex=1: Vectors are complete and ready to use", file=sys.stderr)
    print("# - Without index: Run tests to compute filter values, then update JSON manually", file=sys.stderr)


if __name__ == '__main__':
    main()
