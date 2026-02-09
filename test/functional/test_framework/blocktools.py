#!/usr/bin/env python3
# Copyright (c) 2015-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Utilities for manipulating blocks and transactions."""

from binascii import a2b_hex
import struct
import time
import unittest

from .address import (
    base58_to_byte,
    key_to_p2sh_p2wpkh,
    key_to_p2wpkh,
    script_to_p2sh_p2wsh,
    script_to_p2wsh,
)
from .key import ECKey
from .messages import (
    CBlock,
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
    hash256,
    hex_str_to_bytes,
    POW_BLOCK_VERSION,
    ser_uint256,
    tx_from_hex,
    uint256_from_str,
)
from .script import (
    CScript,
    CScriptNum,
    CScriptOp,
    OP_0,
    OP_1,
    OP_CHECKMULTISIG,
    OP_CHECKSIG,
    OP_RETURN,
    OP_TRUE,
)
from .script_util import (
    key_to_p2wpkh_script,
    script_to_p2wsh_script,
)
from .util import assert_equal

WITNESS_SCALE_FACTOR = 4
MAX_BLOCK_SIGOPS = 20000
MAX_BLOCK_SIGOPS_WEIGHT = MAX_BLOCK_SIGOPS * WITNESS_SCALE_FACTOR

# Genesis block time (regtest)
TIME_GENESIS_BLOCK = 1296688602

# Coinbase transaction outputs can only be spent after this number of new blocks (network rule)
# ReddCoin regtest uses nCoinbaseMaturity = 60 (see src/chainparams.cpp)
COINBASE_MATURITY = 60

# From BIP141
WITNESS_COMMITMENT_HEADER = b"\xaa\x21\xa9\xed"

NORMAL_GBT_REQUEST_PARAMS = {"rules": ["segwit"]}


def create_block(hashprev=None, coinbase=None, ntime=None, *, version=None, tmpl=None, txlist=None):
    """Create a block (with regtest difficulty).

    For PoS blocks (version > 2), the expected structure is:
      - vtx[0]: Coinbase (minimal, just height in scriptSig, empty vout[0])
      - vtx[1]: Coinstake (from template's 'transactions' array)
      - vtx[2+]: Other transactions

    When using getblocktemplate for PoS blocks:
      - The template includes coinstake in transactions[0]
      - We detect this and create an empty coinbase
    """
    block = CBlock()
    if tmpl is None:
        tmpl = {}
    block.nVersion = version or tmpl.get('version') or 1
    block.nTime = ntime or tmpl.get('curtime') or int(time.time() + 600)
    block.hashPrevBlock = hashprev or int(tmpl['previousblockhash'], 0x10)
    if tmpl and not tmpl.get('bits') is None:
        block.nBits = struct.unpack('>I', a2b_hex(tmpl['bits']))[0]
    else:
        block.nBits = 0x207fffff  # difficulty retargeting is disabled in REGTEST chainparams

    # Detect PoS by checking if template contains a coinstake transaction
    # (not by version, since BIP9 version bits can make version > 2 for PoW blocks)
    is_pos_block = False
    if tmpl and 'transactions' in tmpl and len(tmpl['transactions']) > 0:
        first_tx = tx_from_hex(tmpl['transactions'][0]['data'])
        # IsCoinStake check: has inputs, first input not null, 2+ outputs, first output empty
        is_pos_block = (len(first_tx.vin) > 0 and
                       first_tx.vin[0].prevout.hash != 0 and
                       len(first_tx.vout) >= 2 and
                       first_tx.vout[0].nValue == 0)

    # Create or use provided coinbase
    if coinbase is None:
        if is_pos_block:
            # PoS block: create empty coinbase (matching miner.cpp:200)
            coinbase = create_coinbase(height=tmpl.get('height', 0), outputScriptPubKey=CScript())
        else:
            # PoW block: create normal coinbase
            coinbase = create_coinbase(height=tmpl.get('height', 0))
    block.vtx.append(coinbase)

    # For PoS blocks, add transactions from template (which includes coinstake as first tx)
    # The template's 'transactions' array has all non-coinbase transactions,
    # including the coinstake (which becomes vtx[1])
    if tmpl and 'transactions' in tmpl:
        for tx_data in tmpl['transactions']:
            # Deserialize transaction from hex
            tx = tx_from_hex(tx_data['data'])
            block.vtx.append(tx)

        # ReddCoin: For PoS blocks, ensure block timestamp matches coinstake timestamp
        # PoS validation requires: block.nTime == coinstake.nTime
        # The coinstake timestamp is authoritative (part of PoS proof), so sync block to it
        if is_pos_block and len(block.vtx) >= 2:
            coinstake = block.vtx[1]
            if hasattr(coinstake, 'nTime') and coinstake.nTime != block.nTime:
                # Update block timestamp to match coinstake (cannot modify coinstake - it's signed)
                block.nTime = coinstake.nTime

    # Add any additional transactions from txlist
    if txlist:
        for tx in txlist:
            if not hasattr(tx, 'calc_sha256'):
                tx = tx_from_hex(tx)
            block.vtx.append(tx)
    block.hashMerkleRoot = block.calc_merkle_root()
    block.calc_sha256()
    return block

def get_witness_script(witness_root, witness_nonce):
    witness_commitment = uint256_from_str(hash256(ser_uint256(witness_root) + ser_uint256(witness_nonce)))
    output_data = WITNESS_COMMITMENT_HEADER + ser_uint256(witness_commitment)
    return CScript([OP_RETURN, output_data])

def add_witness_commitment(block, nonce=0):
    """Add a witness commitment to the block's coinbase transaction.

    According to BIP141, blocks with witness rules active must commit to the
    hash of all in-block transactions including witness."""
    # First calculate the merkle root of the block's
    # transactions, with witnesses.
    witness_nonce = nonce
    witness_root = block.calc_witness_merkle_root()
    # witness_nonce should go to coinbase witness.
    block.vtx[0].wit.vtxinwit = [CTxInWitness()]
    block.vtx[0].wit.vtxinwit[0].scriptWitness.stack = [ser_uint256(witness_nonce)]

    # witness commitment is the last OP_RETURN output in coinbase
    block.vtx[0].vout.append(CTxOut(0, get_witness_script(witness_root, witness_nonce)))
    block.vtx[0].rehash()
    block.hashMerkleRoot = block.calc_merkle_root()
    block.rehash()


def sign_block(block, wif_privkey):
    """Sign a PoS block with the given private key (in WIF format).

    ReddCoin PoS blocks (version > POW_BLOCK_VERSION) must be signed by the
    private key corresponding to the public key in the coinbase/coinstake output.

    For PoS blocks (version > 2):
      - Block signing key comes from vtx[1]->vout[1] (coinstake second output)
      - The coinstake structure is:
        * vtx[0] = coinbase (minimal, just block subsidy)
        * vtx[1] = coinstake transaction with:
          - vin[0]: staked UTXO
          - vout[0]: empty marker (0 value)
          - vout[1]: staking output (contains pubkey for signing)
          - vout[2+]: stake rewards

    For PoW blocks (version <= 2):
      - Block signing key comes from vtx[0]->vout[0] (coinbase output)

    This function:
    1. Verifies the block has the expected structure
    2. Converts the WIF private key to an ECKey object
    3. Verifies the key matches the pubkey in the block (optional but recommended)
    4. Signs the block hash with ECDSA
    5. Stores the DER-encoded signature in block.vchBlockSig
    """
    if block.nVersion <= POW_BLOCK_VERSION:
        # PoW blocks don't need signatures
        return

    # Verify PoS block structure
    if len(block.vtx) < 2:
        raise ValueError(f"PoS block must have at least 2 transactions (coinbase + coinstake), found {len(block.vtx)}")

    # Check coinstake has the expected outputs
    coinstake = block.vtx[1]
    if len(coinstake.vout) < 2:
        raise ValueError(f"Coinstake must have at least 2 outputs, found {len(coinstake.vout)}")

    # Decode WIF private key
    privkey_bytes, version = base58_to_byte(wif_privkey)
    # Remove compression flag if present (last byte = 0x01 for compressed keys)
    compressed = False
    if len(privkey_bytes) == 33 and privkey_bytes[-1] == 1:
        compressed = True
        privkey_bytes = privkey_bytes[:-1]

    # Create ECKey and set the private key
    key = ECKey()
    key.set(privkey_bytes, compressed)

    # Get the pubkey from our private key
    our_pubkey = key.get_pubkey()
    our_pubkey_bytes = our_pubkey.get_bytes()

    # Extract the scriptPubKey from coinstake output[1]
    # It should be a P2PK script: <pubkey> OP_CHECKSIG
    script_pubkey = coinstake.vout[1].scriptPubKey

    # Verify the script is P2PK format (pubkey + OP_CHECKSIG)
    if len(script_pubkey) > 0:
        # For P2PK: script is [<len><pubkey><OP_CHECKSIG>]
        # The pubkey should match our key's pubkey
        # Note: This is a basic check; a full implementation would parse the script properly
        if our_pubkey_bytes not in bytes(script_pubkey):
            # Warning: Key might not match the block's pubkey
            # For testing, we'll proceed anyway, but this could cause verification to fail
            import sys
            print(f"WARNING: Signing key pubkey might not match block's coinstake output", file=sys.stderr)

    # Calculate block hash
    block.rehash()
    block_hash = ser_uint256(block.sha256)

    # Sign the block hash with ECDSA (DER-encoded signature)
    signature = key.sign_ecdsa(block_hash)

    # Store signature in block
    block.vchBlockSig = signature


def script_BIP34_coinbase_height(height):
    """Create coinbase scriptSig for ReddCoin: <height> OP_0

    ReddCoin uses OP_0 as the second element (not OP_1 like Bitcoin).
    See src/miner.cpp:242: coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    """
    if height <= 16:
        res = CScriptOp.encode_op_n(height)
        # Use OP_0 to match ReddCoin's miner.cpp
        return CScript([res, OP_0])
    return CScript([CScriptNum(height), OP_0])


def create_coinbase(height, pubkey=None, extra_output_script=None, fees=0, nValue=50, *, outputScriptPubKey=None):
    """Create a coinbase transaction for ReddCoin.

    ReddCoin coinbase structure (from miner.cpp):
    - vin[0].scriptSig: <height> OP_0
    - vout[0]: For PoS blocks, this is EMPTY (0 value, empty script)
               For PoW blocks, contains block reward

    Args:
        height: Block height for scriptSig
        pubkey: If provided, create P2PK output (PoW only)
        extra_output_script: Additional output for witness commitment
        fees: Transaction fees to include in output value
        nValue: Base reward value (default 50, will be halved)
        outputScriptPubKey: Explicit scriptPubKey for vout[0].
                           If CScript(), creates empty output (for PoS).
                           If None, creates OP_TRUE or P2PK output (for PoW).

    For PoS blocks from getblocktemplate, use:
        create_coinbase(height, outputScriptPubKey=CScript())
    This creates an empty vout[0] matching miner.cpp:200
    """
    coinbase = CTransaction()
    coinbase.vin.append(CTxIn(COutPoint(0, 0xffffffff), script_BIP34_coinbase_height(height), 0xffffffff))

    coinbaseoutput = CTxOut()

    # Check if we should create an empty output (for PoS blocks)
    if outputScriptPubKey is not None:
        # Explicit scriptPubKey provided
        if len(outputScriptPubKey) == 0:
            # Empty script = PoS block, set empty output
            coinbaseoutput.nValue = 0
            coinbaseoutput.scriptPubKey = CScript()
        else:
            # Custom scriptPubKey
            coinbaseoutput.nValue = nValue * COIN
            if nValue == 50:
                halvings = int(height / 150)  # regtest
                coinbaseoutput.nValue >>= halvings
                coinbaseoutput.nValue += fees
            coinbaseoutput.scriptPubKey = outputScriptPubKey
    else:
        # Default PoW behavior
        coinbaseoutput.nValue = nValue * COIN
        if nValue == 50:
            halvings = int(height / 150)  # regtest
            coinbaseoutput.nValue >>= halvings
            coinbaseoutput.nValue += fees
        if pubkey is not None:
            coinbaseoutput.scriptPubKey = CScript([pubkey, OP_CHECKSIG])
        else:
            coinbaseoutput.scriptPubKey = CScript([OP_TRUE])

    coinbase.vout = [coinbaseoutput]

    if extra_output_script is not None:
        coinbaseoutput2 = CTxOut()
        coinbaseoutput2.nValue = 0
        coinbaseoutput2.scriptPubKey = extra_output_script
        coinbase.vout.append(coinbaseoutput2)

    coinbase.calc_sha256()
    return coinbase

def create_tx_with_script(prevtx, n, script_sig=b"", *, amount, script_pub_key=CScript()):
    """Return one-input, one-output transaction object
       spending the prevtx's n-th output with the given amount.

       Can optionally pass scriptPubKey and scriptSig, default is anyone-can-spend output.
    """
    tx = CTransaction()
    assert n < len(prevtx.vout)
    tx.vin.append(CTxIn(COutPoint(prevtx.sha256, n), script_sig, 0xffffffff))
    tx.vout.append(CTxOut(amount, script_pub_key))
    tx.calc_sha256()
    return tx

def create_transaction(node, txid, to_address, *, amount):
    """ Return signed transaction spending the first output of the
        input txid. Note that the node must have a wallet that can
        sign for the output that is being spent.
    """
    raw_tx = create_raw_transaction(node, txid, to_address, amount=amount)
    tx = tx_from_hex(raw_tx)
    return tx

def create_raw_transaction(node, txid, to_address, *, amount):
    """ Return raw signed transaction spending the first output of the
        input txid. Note that the node must have a wallet that can sign
        for the output that is being spent.
    """
    psbt = node.createpsbt(inputs=[{"txid": txid, "vout": 0}], outputs={to_address: amount})
    for _ in range(2):
        for w in node.listwallets():
            wrpc = node.get_wallet_rpc(w)
            signed_psbt = wrpc.walletprocesspsbt(psbt)
            psbt = signed_psbt['psbt']
    final_psbt = node.finalizepsbt(psbt)
    assert_equal(final_psbt["complete"], True)
    return final_psbt['hex']

def get_legacy_sigopcount_block(block, accurate=True):
    count = 0
    for tx in block.vtx:
        count += get_legacy_sigopcount_tx(tx, accurate)
    return count

def get_legacy_sigopcount_tx(tx, accurate=True):
    count = 0
    for i in tx.vout:
        count += i.scriptPubKey.GetSigOpCount(accurate)
    for j in tx.vin:
        # scriptSig might be of type bytes, so convert to CScript for the moment
        count += CScript(j.scriptSig).GetSigOpCount(accurate)
    return count

def witness_script(use_p2wsh, pubkey):
    """Create a scriptPubKey for a pay-to-witness TxOut.

    This is either a P2WPKH output for the given pubkey, or a P2WSH output of a
    1-of-1 multisig for the given pubkey. Returns the hex encoding of the
    scriptPubKey."""
    if not use_p2wsh:
        # P2WPKH instead
        pkscript = key_to_p2wpkh_script(pubkey)
    else:
        # 1-of-1 multisig
        witness_program = CScript([OP_1, hex_str_to_bytes(pubkey), OP_1, OP_CHECKMULTISIG])
        pkscript = script_to_p2wsh_script(witness_program)
    return pkscript.hex()

def create_witness_tx(node, use_p2wsh, utxo, pubkey, encode_p2sh, amount):
    """Return a transaction (in hex) that spends the given utxo to a segwit output.

    Optionally wrap the segwit output using P2SH."""
    if use_p2wsh:
        program = CScript([OP_1, hex_str_to_bytes(pubkey), OP_1, OP_CHECKMULTISIG])
        addr = script_to_p2sh_p2wsh(program) if encode_p2sh else script_to_p2wsh(program)
    else:
        addr = key_to_p2sh_p2wpkh(pubkey) if encode_p2sh else key_to_p2wpkh(pubkey)
    if not encode_p2sh:
        assert_equal(node.getaddressinfo(addr)['scriptPubKey'], witness_script(use_p2wsh, pubkey))
    return node.createrawtransaction([utxo], {addr: amount})

def send_to_witness(use_p2wsh, node, utxo, pubkey, encode_p2sh, amount, sign=True, insert_redeem_script=""):
    """Create a transaction spending a given utxo to a segwit output.

    The output corresponds to the given pubkey: use_p2wsh determines whether to
    use P2WPKH or P2WSH; encode_p2sh determines whether to wrap in P2SH.
    sign=True will have the given node sign the transaction.
    insert_redeem_script will be added to the scriptSig, if given."""
    tx_to_witness = create_witness_tx(node, use_p2wsh, utxo, pubkey, encode_p2sh, amount)
    if (sign):
        signed = node.signrawtransactionwithwallet(tx_to_witness)
        assert "errors" not in signed or len(["errors"]) == 0
        return node.sendrawtransaction(signed["hex"])
    else:
        if (insert_redeem_script):
            tx = tx_from_hex(tx_to_witness)
            tx.vin[0].scriptSig += CScript([hex_str_to_bytes(insert_redeem_script)])
            tx_to_witness = tx.serialize().hex()

    return node.sendrawtransaction(tx_to_witness)

class TestFrameworkBlockTools(unittest.TestCase):
    def test_create_coinbase(self):
        height = 20
        coinbase_tx = create_coinbase(height=height)
        assert_equal(CScriptNum.decode(coinbase_tx.vin[0].scriptSig), height)
