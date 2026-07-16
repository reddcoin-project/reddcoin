#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Audit PoS-era transaction versions on ReddCoin mainnet/testnet.

Consensus-safety check for the rule added to ContextualCheckBlock (and the
mempool PreChecks) in src/validation.cpp:

    for blocks at height > consensus.nLastPowHeight, every transaction must
    have nVersion > POW_TX_VERSION (i.e. nVersion >= POSV_TX_VERSION == 2),
    else the block is rejected with "bad-txns-version-pos".

This rule is applied to ALL transactions in the block, INCLUDING the coinbase
(vtx[0]) and the coinstake (vtx[1]). If any historical PoS-era block on a live
network contains a transaction with nVersion <= 1, the new rule would reject
valid chain history and fork the node during (re)validation. This script scans
the chain and reports every such transaction so the rule can be verified safe
before it ships.

It talks to a running reddcoind over JSON-RPC and reads full blocks
(getblock verbosity=2), so the node — not this script — does the deserialization.

Exit status: 0 if no offending transactions were found, 1 if any were found,
2 on a usage/connection error.

Examples:
    # mainnet, explicit RPC credentials
    contrib/devtools/audit_tx_version.py --rpcuser U --rpcpassword P

    # testnet, auto-read the cookie from the datadir
    contrib/devtools/audit_tx_version.py --network test --datadir ~/.reddcoin

    # resume a previous run (or scan a bounded range)
    contrib/devtools/audit_tx_version.py --start-height 2000000 --stop-height 2100000
"""
import argparse
import base64
import json
import os
import sys
import threading
import time
import http.client
from concurrent.futures import ThreadPoolExecutor

POW_TX_VERSION = 1                 # from src/primitives/transaction.h
MIN_POS_TX_VERSION = POW_TX_VERSION + 1  # == POSV_TX_VERSION (2); the rule requires >= this

# Per-network defaults (src/chainparamsbase.cpp, src/chainparams.cpp).
NETWORK_DEFAULTS = {
    'main':    {'rpcport': 45443, 'datadir_subdir': '',         'last_pow_height': 260799},
    'test':    {'rpcport': 55443, 'datadir_subdir': 'testnet3', 'last_pow_height': 1439},
    'regtest': {'rpcport': 56443, 'datadir_subdir': 'regtest',  'last_pow_height': 89},
}


class RPCError(Exception):
    pass


class RPCClient:
    """Minimal JSON-RPC client with one HTTP connection per thread and batch support."""

    def __init__(self, host, port, user, password, timeout=120):
        self.host = host
        self.port = port
        self._authhdr = 'Basic ' + base64.b64encode(f'{user}:{password}'.encode()).decode()
        self.timeout = timeout
        self._local = threading.local()

    def _conn(self):
        conn = getattr(self._local, 'conn', None)
        if conn is None:
            conn = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
            self._local.conn = conn
        return conn

    def _post(self, payload):
        body = json.dumps(payload).encode()
        headers = {'Authorization': self._authhdr, 'Content-Type': 'application/json'}
        last_err = None
        for _ in range(3):
            try:
                conn = self._conn()
                conn.request('POST', '/', body, headers)
                resp = conn.getresponse()
                data = resp.read()
                if resp.status == 401:
                    raise RPCError('RPC authentication failed (401). Check --rpcuser/--rpcpassword or the cookie file.')
                if resp.status not in (200, 500):
                    raise RPCError(f'HTTP {resp.status} {resp.reason}: {data[:200]!r}')
                return json.loads(data)
            except (http.client.HTTPException, OSError, ConnectionError) as e:
                last_err = e
                self._local.conn = None  # drop the poisoned connection and retry
        raise RPCError(f'RPC request failed after retries: {last_err}')

    def call(self, method, params=None):
        r = self._post({'jsonrpc': '1.0', 'id': 'audit', 'method': method, 'params': params or []})
        if r.get('error'):
            raise RPCError(f'{method}: {r["error"]}')
        return r['result']

    def batch(self, calls):
        """calls: list of (method, params). Returns results in the same order."""
        payload = [{'jsonrpc': '1.0', 'id': i, 'method': m, 'params': p or []}
                   for i, (m, p) in enumerate(calls)]
        resp = self._post(payload)
        by_id = {item['id']: item for item in resp}
        out = []
        for i, (m, _p) in enumerate(calls):
            item = by_id[i]
            if item.get('error'):
                raise RPCError(f'{m}: {item["error"]}')
            out.append(item['result'])
        return out


def resolve_credentials(args):
    """Return (user, password) from explicit args or the datadir cookie file."""
    if args.rpcuser and args.rpcpassword:
        return args.rpcuser, args.rpcpassword

    cookie = args.rpccookiefile
    if not cookie:
        subdir = NETWORK_DEFAULTS.get(args.network, {}).get('datadir_subdir', '')
        datadir = os.path.expanduser(args.datadir)
        cookie = os.path.join(datadir, subdir, '.cookie')
    if not os.path.exists(cookie):
        raise RPCError(f'No RPC credentials: pass --rpcuser/--rpcpassword, or ensure the cookie exists ({cookie}).')
    with open(cookie) as fh:
        user, _, password = fh.read().strip().partition(':')
    return user, password


def tx_role(index):
    # In the PoS era (height > nLastPowHeight) blocks are PoS: vtx[0] coinbase, vtx[1] coinstake.
    return {0: 'coinbase', 1: 'coinstake'}.get(index, 'regular')


def scan(args):
    if args.network not in NETWORK_DEFAULTS:
        raise RPCError(f'Unsupported --network {args.network!r} (expected main or test).')
    defaults = NETWORK_DEFAULTS[args.network]

    port = args.rpcport or defaults['rpcport']
    user, password = resolve_credentials(args)
    rpc = RPCClient(args.rpcconnect, port, user, password, timeout=args.rpc_timeout)

    # Confirm we're really talking to the network we think we are.
    info = rpc.call('getblockchaininfo')
    chain = info['chain']
    tip = info['blocks']
    if chain != args.network:
        raise RPCError(f'Node reports chain={chain!r} but --network={args.network!r}. '
                       f'Point at the right node or pass the matching --network.')

    last_pow = args.last_pow_height if args.last_pow_height is not None else defaults['last_pow_height']
    start = args.start_height if args.start_height is not None else last_pow + 1
    stop = args.stop_height if args.stop_height is not None else tip
    if start <= last_pow:
        print(f'[note] start height {start} is within the PoW era (<= nLastPowHeight {last_pow}); '
              f'the rule does not apply there. Clamping start to {last_pow + 1}.', file=sys.stderr)
        start = last_pow + 1
    if stop > tip:
        stop = tip

    total = max(0, stop - start + 1)
    print(f'Network        : {chain}')
    print(f'Chain tip      : {tip}')
    print(f'nLastPowHeight : {last_pow}  (rule applies to height > this)')
    print(f'Scan range     : {start} .. {stop}  ({total} blocks)')
    print(f'Requiring      : every tx nVersion >= {MIN_POS_TX_VERSION} '
          f'(reject nVersion <= {POW_TX_VERSION})')
    print(f'Threads        : {args.threads}', flush=True)
    if total == 0:
        print('Nothing to scan.')
        return 0

    violations = []
    version_hist = {}
    tx_count = 0
    hist_lock = threading.Lock()
    t0 = time.time()

    vfile = None
    if args.out:
        vfile = open(args.out, 'w')
        vfile.write('height,tx_index,role,txid,version\n')

    def fetch_block_versions(height):
        # getblock needs a hash, so resolve the height first, then fetch the full
        # block (verbosity=2) so the node deserializes each tx and reports version.
        blkhash = rpc.call('getblockhash', [height])
        block = rpc.call('getblock', [blkhash, 2])
        return height, block

    def handle(result):
        nonlocal tx_count
        height, block = result
        local_hist = {}
        local_txs = 0
        for idx, tx in enumerate(block['tx']):
            ver = tx['version']
            local_txs += 1
            local_hist[ver] = local_hist.get(ver, 0) + 1
            if ver <= POW_TX_VERSION:
                v = (height, idx, tx_role(idx), tx['txid'], ver)
                violations.append(v)
                line = f'{height},{idx},{v[2]},{v[3]},{ver}'
                print(f'  !! VIOLATION  height={height} idx={idx} ({v[2]}) '
                      f'version={ver} txid={v[3]}', file=sys.stderr, flush=True)
                if vfile:
                    vfile.write(line + '\n')
                    vfile.flush()
        with hist_lock:
            tx_count += local_txs
            for k, c in local_hist.items():
                version_hist[k] = version_hist.get(k, 0) + c

    # Process in ordered windows so progress/checkpointing is monotonic while
    # still fetching each window's blocks concurrently.
    done = 0
    with ThreadPoolExecutor(max_workers=args.threads) as pool:
        h = start
        while h <= stop:
            window = list(range(h, min(h + args.window, stop + 1)))
            for res in pool.map(fetch_block_versions, window):
                handle(res)
            done += len(window)
            h += len(window)
            if done % args.progress_every < args.window or h > stop:
                elapsed = time.time() - t0
                rate = done / elapsed if elapsed else 0
                remaining = (total - done) / rate if rate else 0
                print(f'  ... {done}/{total} blocks  ({100*done/total:.1f}%)  '
                      f'height={h-1}  {rate:.0f} blk/s  ETA {remaining/60:.1f} min  '
                      f'violations={len(violations)}', flush=True)

    if vfile:
        vfile.close()

    dt = time.time() - t0
    print()
    print('=' * 68)
    print(f'Scanned {done} blocks / {tx_count} transactions in {dt/60:.1f} min')
    print('nVersion distribution:')
    for ver in sorted(version_hist):
        flag = '  <-- BELOW PoS MINIMUM' if ver <= POW_TX_VERSION else ''
        print(f'    v{ver}: {version_hist[ver]}{flag}')
    print('=' * 68)
    if violations:
        print(f'RESULT: FAIL — {len(violations)} transaction(s) with nVersion <= {POW_TX_VERSION} '
              f'in the PoS era. The rule WOULD reject chain history; do NOT ship it as-is.')
        if args.out:
            print(f'Offending transactions written to {args.out}')
        # Show the first few inline for convenience.
        for v in violations[:20]:
            print(f'    height={v[0]} idx={v[1]} ({v[2]}) version={v[4]} txid={v[3]}')
        if len(violations) > 20:
            print(f'    ... and {len(violations) - 20} more')
        return 1

    print(f'RESULT: PASS — every PoS-era transaction has nVersion >= {MIN_POS_TX_VERSION}. '
          f'The rule is consistent with {chain} history over the scanned range.')
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--network', choices=['main', 'test', 'regtest'], default='main',
                   help='Network to audit (default: main). Must match the node.')
    p.add_argument('--rpcconnect', default='127.0.0.1', help='RPC host (default: 127.0.0.1)')
    p.add_argument('--rpcport', type=int, default=None, help='RPC port (default: per-network)')
    p.add_argument('--rpcuser', default=None, help='RPC username')
    p.add_argument('--rpcpassword', default=None, help='RPC password')
    p.add_argument('--rpccookiefile', default=None, help='Path to the RPC .cookie file')
    p.add_argument('--datadir', default='~/.reddcoin', help='Datadir to find the cookie (default: ~/.reddcoin)')
    p.add_argument('--rpc-timeout', type=int, default=120, help='Per-request RPC timeout in seconds')
    p.add_argument('--start-height', type=int, default=None, help='First height to scan (default: nLastPowHeight+1)')
    p.add_argument('--stop-height', type=int, default=None, help='Last height to scan (default: chain tip)')
    p.add_argument('--last-pow-height', type=int, default=None, help='Override nLastPowHeight')
    p.add_argument('--threads', type=int, default=8, help='Concurrent block fetches (default: 8)')
    p.add_argument('--window', type=int, default=256, help='Blocks per ordered window (default: 256)')
    p.add_argument('--progress-every', type=int, default=5000, help='Progress print cadence in blocks')
    p.add_argument('--out', default=None, help='Write offending transactions to this CSV file')
    args = p.parse_args()

    try:
        return scan(args)
    except RPCError as e:
        print(f'ERROR: {e}', file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print('\nInterrupted. Re-run with --start-height to resume.', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
