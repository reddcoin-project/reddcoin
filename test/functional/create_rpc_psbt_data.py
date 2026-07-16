#!/usr/bin/env python3
# Copyright (c) 2024 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Generate ReddCoin-compatible BIP 174 PSBT test vectors.

ReddCoin transactions with nVersion > 1 include a 4-byte nTime field after
nLockTime that Bitcoin doesn't have. The BIP 174 test vectors use Bitcoin
nVersion=2 transactions without nTime. This script patches the JSON at build
time, inserting nTime AND fixing outpoint txids so the node won't reject them
with "Non-witness UTXO does not match outpoint hash".

Usage:
    python3 test/functional/create_rpc_psbt_data.py
"""

import base64
import hashlib
import io
import json
import os
import struct
import sys

# Add test framework to path for segwit_addr
sys.path.insert(0, os.path.join(os.path.dirname(os.path.realpath(__file__)), 'test_framework'))
from segwit_addr import bech32_decode, bech32_encode


NTIME = 1609459200  # 2021-01-01 00:00:00 UTC
REDDCOIN_MAX_MONEY_SAT = 92233720368 * 100000000  # 9,223,372,036,800,000,000


# ---------------------------------------------------------------------------
# Low-level helpers
# ---------------------------------------------------------------------------

def _read_compact_size(f):
    n = struct.unpack('B', f.read(1))[0]
    if n < 253:
        return n
    elif n == 253:
        return struct.unpack('<H', f.read(2))[0]
    elif n == 254:
        return struct.unpack('<I', f.read(4))[0]
    else:
        return struct.unpack('<Q', f.read(8))[0]


def _write_compact_size(n):
    if n < 253:
        return struct.pack('B', n)
    elif n < 0x10000:
        return struct.pack('<BH', 253, n)
    elif n < 0x100000000:
        return struct.pack('<BI', 254, n)
    else:
        return struct.pack('<BQ', 255, n)


def _sha256d(data):
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


# ---------------------------------------------------------------------------
# Transaction-level patching
# ---------------------------------------------------------------------------

def _add_ntime_to_tx(tx_bytes, ntime=NTIME, force_nonsegwit=False):
    """Insert nTime after nLockTime for nVersion >= 2 transactions.

    BIP 174 unsigned txs use non-segwit format (force_nonsegwit=True).
    Non-witness UTXOs may use segwit format (force_nonsegwit=False).
    """
    f = io.BytesIO(tx_bytes)
    version = struct.unpack('<I', f.read(4))[0]
    if version < 2:
        return tx_bytes

    pos = f.tell()
    marker = struct.unpack('B', f.read(1))[0]
    is_segwit = False
    if marker == 0 and not force_nonsegwit:
        flag = struct.unpack('B', f.read(1))[0]
        if flag != 0:
            is_segwit = True
        else:
            f.seek(pos)
    else:
        f.seek(pos)

    nin = _read_compact_size(f)
    for _ in range(nin):
        f.read(32 + 4)  # outpoint
        sl = _read_compact_size(f)
        f.read(sl)  # scriptSig
        f.read(4)  # sequence
    nout = _read_compact_size(f)
    for _ in range(nout):
        f.read(8)  # value
        sl = _read_compact_size(f)
        f.read(sl)  # scriptPubKey
    if is_segwit:
        for _ in range(nin):
            nstack = _read_compact_size(f)
            for _ in range(nstack):
                item_len = _read_compact_size(f)
                f.read(item_len)

    locktime_end = f.tell() + 4
    f.read(4)  # nLockTime
    remaining = f.read()
    return tx_bytes[:locktime_end] + struct.pack('<I', ntime) + remaining


def _tx_to_nonsegwit(tx_bytes):
    """Serialize a (possibly segwit) tx in non-segwit format for txid computation."""
    f = io.BytesIO(tx_bytes)
    version = f.read(4)

    pos = f.tell()
    marker = struct.unpack('B', f.read(1))[0]
    is_segwit = False
    if marker == 0:
        flag = struct.unpack('B', f.read(1))[0]
        if flag != 0:
            is_segwit = True
        else:
            f.seek(pos)
    else:
        f.seek(pos)

    # Read inputs
    nin = _read_compact_size(f)
    inputs_data = _write_compact_size(nin)
    for _ in range(nin):
        outpoint = f.read(32 + 4)
        sl = _read_compact_size(f)
        script = f.read(sl)
        seq = f.read(4)
        inputs_data += outpoint + _write_compact_size(sl) + script + seq

    # Read outputs
    nout = _read_compact_size(f)
    outputs_data = _write_compact_size(nout)
    for _ in range(nout):
        value = f.read(8)
        sl = _read_compact_size(f)
        script = f.read(sl)
        outputs_data += value + _write_compact_size(sl) + script

    # Skip witness if segwit
    if is_segwit:
        for _ in range(nin):
            nstack = _read_compact_size(f)
            for _ in range(nstack):
                item_len = _read_compact_size(f)
                f.read(item_len)

    locktime = f.read(4)
    remaining = f.read()  # nTime if present

    return version + inputs_data + outputs_data + locktime + remaining


def _compute_txid(tx_bytes):
    """Compute txid (hash of non-segwit serialization), returned as little-endian bytes."""
    nonsegwit = _tx_to_nonsegwit(tx_bytes)
    return _sha256d(nonsegwit)


def _parse_tx_outpoints(tx_bytes, force_nonsegwit=True):
    """Return list of (txid_bytes_le, vout) outpoints from a non-segwit tx."""
    f = io.BytesIO(tx_bytes)
    f.read(4)  # version

    if not force_nonsegwit:
        pos = f.tell()
        marker = struct.unpack('B', f.read(1))[0]
        if marker == 0:
            f.read(1)  # flag
        else:
            f.seek(pos)

    nin = _read_compact_size(f)
    outpoints = []
    for _ in range(nin):
        txid = f.read(32)
        vout = struct.unpack('<I', f.read(4))[0]
        outpoints.append((txid, vout))
        sl = _read_compact_size(f)
        f.read(sl)
        f.read(4)
    return outpoints


def _replace_outpoints_in_tx(tx_bytes, txid_map, force_nonsegwit=True):
    """Replace outpoint txids in a transaction using txid_map {old_le: new_le}."""
    f = io.BytesIO(tx_bytes)
    result = io.BytesIO()

    result.write(f.read(4))  # version

    if not force_nonsegwit:
        pos = f.tell()
        marker = struct.unpack('B', f.read(1))[0]
        if marker == 0:
            flag_byte = f.read(1)
            result.write(b'\x00')
            result.write(flag_byte)
        else:
            f.seek(pos)

    nin = _read_compact_size(f)
    result.write(_write_compact_size(nin))
    for _ in range(nin):
        txid = f.read(32)
        vout = f.read(4)
        if txid in txid_map:
            txid = txid_map[txid]
        result.write(txid)
        result.write(vout)
        sl = _read_compact_size(f)
        script = f.read(sl)
        result.write(_write_compact_size(sl))
        result.write(script)
        result.write(f.read(4))  # sequence

    # Copy the rest unchanged
    result.write(f.read())
    return result.getvalue()


# ---------------------------------------------------------------------------
# Raw hex transaction patching
# ---------------------------------------------------------------------------

def _patch_raw_tx(tx_hex, ntime=NTIME):
    """Patch a raw hex transaction: add nTime + fix outpoint txids.

    Used for the extractor 'result' field which is a raw serialized tx, not a PSBT.
    The raw tx may have segwit witness data.
    """
    tx_bytes = bytes.fromhex(tx_hex)
    f = io.BytesIO(tx_bytes)
    version = struct.unpack('<I', f.read(4))[0]
    if version < 2:
        return tx_hex

    # Detect segwit
    pos = f.tell()
    marker = struct.unpack('B', f.read(1))[0]
    is_segwit = False
    if marker == 0:
        flag = struct.unpack('B', f.read(1))[0]
        if flag != 0:
            is_segwit = True
        else:
            f.seek(pos)
    else:
        f.seek(pos)

    # For the raw extractor result, the outpoints reference other txs
    # that we can't look up. However, the extractor result is a fully-signed
    # tx whose outpoints must match the non_witness_utxo txids that we've
    # already patched in the PSBT. We need to build the same txid_map.
    #
    # Actually, for the extractor result, we need to do the same mapping:
    # the original outpoints reference Bitcoin txids; after adding nTime to
    # those referenced txs, the txids change. We need to know which txs
    # are referenced. Since the extractor result comes from the same PSBT
    # as the extractor extract, we handle this at a higher level.
    #
    # For now, just add nTime to the raw tx itself.
    patched = _add_ntime_to_tx(tx_bytes, ntime, force_nonsegwit=False)
    return patched.hex()


# ---------------------------------------------------------------------------
# PSBT-level patching
# ---------------------------------------------------------------------------

def _parse_psbt_sections(data):
    """Parse a PSBT into (magic, global_kvs, input_kvs_list, output_kvs_list).

    Each kv is (key_bytes, val_bytes). Returns raw bytes for reassembly.
    """
    f = io.BytesIO(data)
    magic = f.read(5)
    if magic != b'\x70\x73\x62\x74\xff':
        return None

    def read_kvs():
        kvs = []
        while True:
            key_len = _read_compact_size(f)
            if key_len == 0:
                break
            key = f.read(key_len)
            val_len = _read_compact_size(f)
            val = f.read(val_len)
            kvs.append((key, val))
        return kvs

    global_kvs = read_kvs()

    # Find unsigned tx to count inputs/outputs
    unsigned_tx = None
    for key, val in global_kvs:
        if key == b'\x00':
            unsigned_tx = val
            break

    if unsigned_tx is None:
        return None

    # Count inputs from unsigned tx (always non-segwit per BIP 174)
    ftx = io.BytesIO(unsigned_tx)
    ftx.read(4)  # version
    nin = _read_compact_size(ftx)
    for _ in range(nin):
        ftx.read(32 + 4)
        sl = _read_compact_size(ftx)
        ftx.read(sl)
        ftx.read(4)
    nout = _read_compact_size(ftx)

    input_kvs_list = [read_kvs() for _ in range(nin)]
    output_kvs_list = [read_kvs() for _ in range(nout)]

    return magic, global_kvs, input_kvs_list, output_kvs_list


def _reassemble_psbt(magic, global_kvs, input_kvs_list, output_kvs_list):
    """Reassemble PSBT from parsed sections."""
    result = io.BytesIO()
    result.write(magic)

    def write_kvs(kvs):
        for key, val in kvs:
            result.write(_write_compact_size(len(key)))
            result.write(key)
            result.write(_write_compact_size(len(val)))
            result.write(val)
        result.write(b'\x00')  # separator

    write_kvs(global_kvs)
    for ikvs in input_kvs_list:
        write_kvs(ikvs)
    for okvs in output_kvs_list:
        write_kvs(okvs)

    return result.getvalue()


def patch_psbt(psbt_b64, ntime=NTIME):
    """Full patch: add nTime to all embedded txs AND fix outpoint txids.

    1. For each input's non_witness_utxo (key 0x00): compute old txid,
       add nTime, compute new txid, build txid_map.
    2. In global unsigned tx (key 0x00): replace outpoint txids using
       txid_map, then add nTime.
    3. Reassemble and return patched base64.
    """
    try:
        data = base64.b64decode(psbt_b64)
    except Exception:
        return psbt_b64

    parsed = _parse_psbt_sections(data)
    if parsed is None:
        return psbt_b64

    magic, global_kvs, input_kvs_list, output_kvs_list = parsed

    # Step 1: Patch non_witness_utxo in each input, build txid_map
    txid_map = {}  # old_txid_le -> new_txid_le

    for i, ikvs in enumerate(input_kvs_list):
        new_kvs = []
        for key, val in ikvs:
            if key == b'\x00':  # non_witness_utxo
                old_txid = _compute_txid(val)
                patched_tx = _add_ntime_to_tx(val, ntime, force_nonsegwit=False)
                new_txid = _compute_txid(patched_tx)
                if old_txid != new_txid:
                    txid_map[old_txid] = new_txid
                new_kvs.append((key, patched_tx))
            else:
                new_kvs.append((key, val))
        input_kvs_list[i] = new_kvs

    # Step 2: Patch global unsigned tx - replace outpoints then add nTime
    new_global = []
    for key, val in global_kvs:
        if key == b'\x00':  # unsigned tx (non-segwit per BIP 174)
            if txid_map:
                val = _replace_outpoints_in_tx(val, txid_map, force_nonsegwit=True)
            val = _add_ntime_to_tx(val, ntime, force_nonsegwit=True)
            new_global.append((key, val))
        else:
            new_global.append((key, val))
    global_kvs = new_global

    # Step 3: Reassemble
    patched_data = _reassemble_psbt(magic, global_kvs, input_kvs_list, output_kvs_list)
    return base64.b64encode(patched_data).decode()


def patch_psbt_ntime_only(psbt_b64, ntime=NTIME):
    """Patch with nTime only, NO outpoint txid fixup.

    Used to produce a PSBT that will still fail with hash mismatch
    (for the analyzepsbt decode_failed test vector).
    """
    try:
        data = base64.b64decode(psbt_b64)
    except Exception:
        return psbt_b64

    parsed = _parse_psbt_sections(data)
    if parsed is None:
        return psbt_b64

    magic, global_kvs, input_kvs_list, output_kvs_list = parsed

    # Patch non_witness_utxo in inputs (nTime only, no txid fixup)
    for i, ikvs in enumerate(input_kvs_list):
        new_kvs = []
        for key, val in ikvs:
            if key == b'\x00':
                new_kvs.append((key, _add_ntime_to_tx(val, ntime, force_nonsegwit=False)))
            else:
                new_kvs.append((key, val))
        input_kvs_list[i] = new_kvs

    # Patch global unsigned tx (nTime only)
    new_global = []
    for key, val in global_kvs:
        if key == b'\x00':
            new_global.append((key, _add_ntime_to_tx(val, ntime, force_nonsegwit=True)))
        else:
            new_global.append((key, val))
    global_kvs = new_global

    patched_data = _reassemble_psbt(magic, global_kvs, input_kvs_list, output_kvs_list)
    return base64.b64encode(patched_data).decode()


# ---------------------------------------------------------------------------
# Amount patching (for analyzepsbt tests exceeding ReddCoin's MAX_MONEY)
# ---------------------------------------------------------------------------

def _patch_psbt_witness_utxo_amount(psbt_b64, input_idx, new_amount_sat):
    """Replace the witness_utxo value for a specific input in a PSBT.

    witness_utxo format: value (8 bytes LE) + scriptPubKey (varint len + bytes).
    """
    data = base64.b64decode(psbt_b64)
    parsed = _parse_psbt_sections(data)
    if parsed is None:
        return psbt_b64
    magic, global_kvs, input_kvs_list, output_kvs_list = parsed

    new_kvs = []
    for key, val in input_kvs_list[input_idx]:
        if key == b'\x01':  # witness_utxo
            new_val = struct.pack('<Q', new_amount_sat) + val[8:]
            new_kvs.append((key, new_val))
        else:
            new_kvs.append((key, val))
    input_kvs_list[input_idx] = new_kvs

    patched = _reassemble_psbt(magic, global_kvs, input_kvs_list, output_kvs_list)
    return base64.b64encode(patched).decode()


def _patch_psbt_output_amount(psbt_b64, output_idx, new_amount_sat):
    """Replace an output value in the unsigned tx of a PSBT."""
    data = base64.b64decode(psbt_b64)
    parsed = _parse_psbt_sections(data)
    if parsed is None:
        return psbt_b64
    magic, global_kvs, input_kvs_list, output_kvs_list = parsed

    new_global = []
    for key, val in global_kvs:
        if key == b'\x00':  # unsigned tx (non-segwit per BIP 174)
            f = io.BytesIO(val)
            result = io.BytesIO()
            result.write(f.read(4))  # version

            nin = _read_compact_size(f)
            result.write(_write_compact_size(nin))
            for _ in range(nin):
                result.write(f.read(32 + 4))  # outpoint
                sl = _read_compact_size(f)
                result.write(_write_compact_size(sl))
                result.write(f.read(sl))  # scriptSig
                result.write(f.read(4))  # sequence

            nout = _read_compact_size(f)
            result.write(_write_compact_size(nout))
            for j in range(nout):
                value = f.read(8)
                sl = _read_compact_size(f)
                script = f.read(sl)
                if j == output_idx:
                    value = struct.pack('<Q', new_amount_sat)
                result.write(value)
                result.write(_write_compact_size(sl))
                result.write(script)

            result.write(f.read())  # locktime, nTime, etc.
            new_global.append((key, result.getvalue()))
        else:
            new_global.append((key, val))
    global_kvs = new_global

    patched = _reassemble_psbt(magic, global_kvs, input_kvs_list, output_kvs_list)
    return base64.b64encode(patched).decode()


# ---------------------------------------------------------------------------
# Address conversion
# ---------------------------------------------------------------------------

def _convert_bcrt_to_rcrt(addr):
    """Convert bcrt1q... bech32 addresses to rcrt1q... for ReddCoin regtest.

    Must re-encode with proper bech32 checksum since the checksum covers the HRP.
    """
    if addr.startswith('bcrt1'):
        encoding, _hrp, data = bech32_decode(addr)
        return bech32_encode(encoding, 'rcrt', data)
    return addr


# ---------------------------------------------------------------------------
# Generator
# ---------------------------------------------------------------------------

def generate(input_path, output_path):
    """Read original Bitcoin BIP 174 test vectors, patch for ReddCoin, write output."""
    with open(input_path, encoding='utf-8') as f:
        d = json.load(f)

    out = {}

    # Unknown PSBT passthrough test vector
    orig_unknown = "cHNidP8BAD8CAAAAAf//////////////////////////////////////////AAAAAAD/////AQAAAAAAAAAAA2oBAAAAAAAACg8BAgMEBQYHCAkPAQIDBAUGBwgJCgsMDQ4PAAA="
    out['unknown'] = patch_psbt(orig_unknown)

    # Invalid PSBTs - leave unchanged (they fail for structural reasons)
    out['invalid'] = d['invalid']

    # Valid PSBTs - full patch
    out['valid'] = [patch_psbt(v) for v in d['valid']]

    # Creator - convert addresses, patch result
    patched_creators = []
    for creator in d['creator']:
        fixed_outputs = []
        for output in creator['outputs']:
            fixed = {}
            for addr, amount in output.items():
                fixed[_convert_bcrt_to_rcrt(addr)] = amount
            fixed_outputs.append(fixed)
        patched_creators.append({
            'inputs': creator['inputs'],
            'outputs': fixed_outputs,
            'result': patch_psbt(creator['result']),
        })
    out['creator'] = patched_creators

    # Signer - patch psbt and result, keep privkeys
    patched_signers = []
    for signer in d['signer']:
        patched_signers.append({
            'privkeys': signer['privkeys'],
            'psbt': patch_psbt(signer['psbt']),
            'result': patch_psbt(signer['result']),
        })
    out['signer'] = patched_signers

    # Combiner - patch combine[] and result
    patched_combiners = []
    for combiner in d['combiner']:
        patched_combiners.append({
            'combine': [patch_psbt(p) for p in combiner['combine']],
            'result': patch_psbt(combiner['result']),
        })
    out['combiner'] = patched_combiners

    # Finalizer - patch finalize and result
    patched_finalizers = []
    for finalizer in d['finalizer']:
        patched_finalizers.append({
            'finalize': patch_psbt(finalizer['finalize']),
            'result': patch_psbt(finalizer['result']),
        })
    out['finalizer'] = patched_finalizers

    # Extractor - patch extract PSBT and result raw tx
    # For the result (raw hex tx), we need the txid_map from the PSBT patching
    patched_extractors = []
    for extractor in d['extractor']:
        patched_extract = patch_psbt(extractor['extract'])

        # Build txid_map for the raw result tx by re-parsing the original PSBT
        orig_data = base64.b64decode(extractor['extract'])
        parsed = _parse_psbt_sections(orig_data)
        if parsed:
            _, orig_global_kvs, orig_input_kvs_list, _ = parsed
            txid_map = {}
            for ikvs in orig_input_kvs_list:
                for key, val in ikvs:
                    if key == b'\x00':
                        old_txid = _compute_txid(val)
                        patched_tx = _add_ntime_to_tx(val, NTIME, force_nonsegwit=False)
                        new_txid = _compute_txid(patched_tx)
                        if old_txid != new_txid:
                            txid_map[old_txid] = new_txid

            # Patch raw result tx: fix outpoints then add nTime
            result_bytes = bytes.fromhex(extractor['result'])
            if txid_map:
                result_bytes = _replace_outpoints_in_tx(result_bytes, txid_map, force_nonsegwit=False)
            result_bytes = _add_ntime_to_tx(result_bytes, NTIME, force_nonsegwit=False)
            patched_result = result_bytes.hex()
        else:
            patched_result = extractor['result']

        patched_extractors.append({
            'extract': patched_extract,
            'result': patched_result,
        })
    out['extractor'] = patched_extractors

    # analyzepsbt test vectors (inline PSBTs from rpc_psbt.py)
    out['analyzepsbt'] = {
        'unspendable_output': patch_psbt('cHNidP8BAJoCAAAAAljoeiG1ba8MI76OcHBFbDNvfLqlyHV5JPVFiHuyq911AAAAAAD/////g40EJ9DsZQpoqka7CwmK6kQiwHGyyng1Kgd5WdB86h0BAAAAAP////8CcKrwCAAAAAAWAEHYXCtx0AYLCcmIauuBXlCZHdoSTQDh9QUAAAAAFv8/wADXYP/7//////8JxOh0LR2HAI8AAAAAAAEBIADC6wsAAAAAF2oUt/X69ELjeX2nTof+fZ10l+OyAokDAQcJAwEHEAABAACAAAEBIADC6wsAAAAAF2oUt/X69ELjeX2nTof+fZ10l+OyAokDAQcJAwEHENkMak8AAAAA'),
        # Patch witness_utxo value to exceed ReddCoin's MAX_MONEY (original only exceeds Bitcoin's)
        'invalid_value': _patch_psbt_witness_utxo_amount(
            patch_psbt('cHNidP8BAHECAAAAAfA00BFgAm6tp86RowwH6BMImQNL5zXUcTT97XoLGz0BAAAAAAD/////AgD5ApUAAAAAFgAUKNw0x8HRctAgmvoevm4u1SbN7XL87QKVAAAAABYAFPck4gF7iL4NL4wtfRAKgQbghiTUAAAAAAABAR8AgIFq49AHABYAFJUDtxf2PHo641HEOBOAIvFMNTr2AAAA'),
            0, REDDCOIN_MAX_MONEY_SAT + 1),
        'signed_not_finalized': patch_psbt('cHNidP8BAHECAAAAAZYezcxdnbXoQCmrD79t/LzDgtUo9ERqixk8wgioAobrAAAAAAD9////AlDDAAAAAAAAFgAUy/UxxZuzZswcmFnN/E9DGSiHLUsuGPUFAAAAABYAFLsH5o0R38wXx+X2cCosTMCZnQ4baAAAAAABAR8A4fUFAAAAABYAFOBI2h5thf3+Lflb2LGCsVSZwsltIgIC/i4dtVARCRWtROG0HHoGcaVklzJUcwo5homgGkSNAnJHMEQCIGx7zKcMIGr7cEES9BR4Kdt/pzPTK3fKWcGyCJXb7MVnAiALOBgqlMH4GbC1HDh/HmylmO54fyEy4lKde7/BT/PWxwEBAwQBAAAAIgYC/i4dtVARCRWtROG0HHoGcaVklzJUcwo5homgGkSNAnIYDwVpQ1QAAIABAACAAAAAgAAAAAAAAAAAAAAiAgL+CIiB59NSCssOJRGiMYQK1chahgAaaJpIXE41Cyir+xgPBWlDVAAAgAEAAIAAAACAAQAAAAAAAAAA'),
        # Patch output value to exceed ReddCoin's MAX_MONEY (original only exceeds Bitcoin's)
        'output_amount_invalid': _patch_psbt_output_amount(
            patch_psbt('cHNidP8BAHECAAAAAfA00BFgAm6tp86RowwH6BMImQNL5zXUcTT97XoLGz0BAAAAAAD/////AgCAgWrj0AcAFgAUKNw0x8HRctAgmvoevm4u1SbN7XL87QKVAAAAABYAFPck4gF7iL4NL4wtfRAKgQbghiTUAAAAAAABAR8A8gUqAQAAABYAFJUDtxf2PHo641HEOBOAIvFMNTr2AAAA'),
            0, REDDCOIN_MAX_MONEY_SAT + 1),
        'decode_failed': patch_psbt_ntime_only("cHNidP8BAJoCAAAAAkvEW8NnDtdNtDpsmze+Ht2LH35IJcKv00jKAlUs21RrAwAAAAD/////S8Rbw2cO1020OmybN74e3Ysffkglwq/TSMoCVSzbVGsBAAAAAP7///8CwLYClQAAAAAWABSNJKzjaUb3uOxixsvh1GGE3fW7zQD5ApUAAAAAFgAUKNw0x8HRctAgmvoevm4u1SbN7XIAAAAAAAEAnQIAAAACczMa321tVHuN4GKWKRncycI22aX3uXgwSFUKM2orjRsBAAAAAP7///9zMxrfbW1Ue43gYpYpGdzJwjbZpfe5eDBIVQozaiuNGwAAAAAA/v///wIA+QKVAAAAABl2qRT9zXUVA8Ls5iVqynLHe5/vSe1XyYisQM0ClQAAAAAWABRmWQUcjSjghQ8/uH4Bn/zkakwLtAAAAAAAAQEfQM0ClQAAAAAWABRmWQUcjSjghQ8/uH4Bn/zkakwLtAAAAA=="),
        'invalid_prevout': patch_psbt("cHNidP8BAJoCAAAAAkvEW8NnDtdNtDpsmze+Ht2LH35IJcKv00jKAlUs21RrAwAAAAD/////S8Rbw2cO1020OmybN74e3Ysffkglwq/TSMoCVSzbVGsBAAAAAP7///8CwLYClQAAAAAWABSNJKzjaUb3uOxixsvh1GGE3fW7zQD5ApUAAAAAFgAUKNw0x8HRctAgmvoevm4u1SbN7XIAAAAAAAEAnQIAAAACczMa321tVHuN4GKWKRncycI22aX3uXgwSFUKM2orjRsBAAAAAP7///9zMxrfbW1Ue43gYpYpGdzJwjbZpfe5eDBIVQozaiuNGwAAAAAA/v///wIA+QKVAAAAABl2qRT9zXUVA8Ls5iVqynLHe5/vSe1XyYisQM0ClQAAAAAWABRmWQUcjSjghQ8/uH4Bn/zkakwLtAAAAAAAAQEfQM0ClQAAAAAWABRmWQUcjSjghQ8/uH4Bn/zkakwLtAAAAA=="),
    }

    with open(output_path, 'w', encoding='utf-8') as f:
        json.dump(out, f, indent=2)
        f.write('\n')

    print(f"Generated {output_path}")
    print(f"  invalid: {len(out['invalid'])} vectors (unchanged)")
    print(f"  valid: {len(out['valid'])} vectors (patched)")
    print(f"  creator: {len(out['creator'])} vectors")
    print(f"  signer: {len(out['signer'])} vectors")
    print(f"  combiner: {len(out['combiner'])} vectors")
    print(f"  finalizer: {len(out['finalizer'])} vectors")
    print(f"  extractor: {len(out['extractor'])} vectors")
    print(f"  unknown: 1 vector")
    print(f"  analyzepsbt: {len(out['analyzepsbt'])} vectors")


if __name__ == '__main__':
    # Determine paths relative to this script
    script_dir = os.path.dirname(os.path.realpath(__file__))
    # We need the ORIGINAL Bitcoin vectors as input. Back them up first if needed.
    input_path = os.path.join(script_dir, 'data', 'rpc_psbt.json')
    output_path = input_path  # Overwrite in place

    # Check if the file already has our keys (already patched)
    with open(input_path, encoding='utf-8') as f:
        d = json.load(f)
    if 'unknown' in d:
        print("WARNING: data/rpc_psbt.json appears to already be patched (has 'unknown' key).")
        print("To regenerate, restore the original Bitcoin test vectors first.")
        print("Proceeding anyway (will re-patch from current state)...")
        # If already patched, we can't safely re-patch (double nTime).
        # Exit instead.
        import sys
        sys.exit(1)

    generate(input_path, output_path)
