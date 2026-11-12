#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Audit transaction versions and timestamps on ReddCoin mainnet/testnet.

Consensus-safety check for two rules on develop:

- bad-txns-version-pos (ContextualCheckBlock and the mempool PreChecks in
  src/validation.cpp): from consensus.nPosTxVersionFloorHeight on, every
  transaction must have nVersion > POW_TX_VERSION. The floor height has to be
  above every PoS-era transaction with nVersion <= POW_TX_VERSION ("v1").

- bad-txns-time-zero (CheckTransaction in src/consensus/tx_check.cpp): every
  transaction with nVersion > POW_TX_VERSION must have a non-zero nTime. This
  rule has no activation height, so it applies to all of history.

Both rules cover every transaction in a block, the coinbase (vtx[0]) and the
coinstake (vtx[1]) included. A historical transaction that breaks either one
would make a node reject valid chain history and stall during initial block
download or -reindex. The script reports every such transaction.

It talks to a running node over JSON-RPC, fetches each block as raw hex
(getblock verbosity=0) and parses the transactions itself, so it reads nVersion
and nTime as they are serialized. Every parsed block is checked against the
merkle root in its header. Raw hex also works on nodes whose getblock
verbosity=2 fails on PoS blocks.

Exit status: 0 if no offending transactions were found, 1 if any were found,
2 on a usage/connection error.

Examples:
    # mainnet, credentials from ~/.reddcoin/.cookie or reddcoin.conf
    contrib/devtools/audit_tx_version.py

    # testnet with another datadir
    contrib/devtools/audit_tx_version.py --network test --datadir ~/.reddcoin_sync

    # resume a previous run (or scan a bounded range)
    contrib/devtools/audit_tx_version.py --start-height 2000000 --stop-height 2100000
"""
import argparse
import base64
import hashlib
import json
import os
import sys
import threading
import time
import http.client
from concurrent.futures import ThreadPoolExecutor

POW_TX_VERSION = 1                 # from src/primitives/transaction.h
POW_BLOCK_VERSION = 2              # from src/primitives/block.h: later blocks carry vchBlockSig
MIN_POS_TX_VERSION = POW_TX_VERSION + 1  # == POSV_TX_VERSION (2)

# Per-network defaults (src/chainparamsbase.cpp, src/chainparams.cpp).
NETWORK_DEFAULTS = {
    'main':    {'rpcport': 45443, 'datadir_subdir': '',         'conf_section': 'main',    'last_pow_height': 260799},
    'test':    {'rpcport': 55443, 'datadir_subdir': 'testnet3', 'conf_section': 'test',    'last_pow_height': 1439},
    'regtest': {'rpcport': 56443, 'datadir_subdir': 'regtest',  'conf_section': 'regtest', 'last_pow_height': 89},
}


class RPCError(Exception):
    pass


class ParseError(Exception):
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
                    raise RPCError('RPC authentication failed (401). Check --rpcuser/--rpcpassword, the cookie file or reddcoin.conf.')
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


def read_conf_credentials(path, section):
    """rpcuser/rpcpassword from a reddcoin.conf, honouring [main]/[test]/[regtest] sections."""
    user = password = None
    current = None
    with open(path, encoding="utf8") as fh:
        for raw in fh:
            line = raw.split('#', 1)[0].strip()
            if not line:
                continue
            if line.startswith('[') and line.endswith(']'):
                current = line[1:-1].strip()
                continue
            key, sep, value = line.partition('=')
            if not sep:
                continue
            key, value = key.strip(), value.strip()
            if current not in (None, section):
                continue
            if key == 'rpcuser':
                user = value
            elif key == 'rpcpassword':
                password = value
    return user, password


def resolve_credentials(args):
    """Return (user, password) from explicit args, the cookie file or reddcoin.conf."""
    if args.rpcuser and args.rpcpassword:
        return args.rpcuser, args.rpcpassword

    defaults = NETWORK_DEFAULTS[args.network]
    datadir = os.path.expanduser(args.datadir)
    cookie = args.rpccookiefile or os.path.join(datadir, defaults['datadir_subdir'], '.cookie')
    if os.path.isfile(cookie):
        with open(cookie, encoding="utf8") as fh:
            user, _, password = fh.read().strip().partition(':')
        return user, password

    conf = os.path.expanduser(args.conf) if args.conf else os.path.join(datadir, 'reddcoin.conf')
    if os.path.isfile(conf):
        user, password = read_conf_credentials(conf, defaults['conf_section'])
        if user and password:
            return user, password
    raise RPCError(f'No RPC credentials: pass --rpcuser/--rpcpassword, or make sure {cookie} exists '
                   f'or {conf} sets rpcuser and rpcpassword.')


def sha256d(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def read_compact_size(b, p):
    n = b[p]
    if n < 0xfd:
        return n, p + 1
    if n == 0xfd:
        return int.from_bytes(b[p + 1:p + 3], 'little'), p + 3
    if n == 0xfe:
        return int.from_bytes(b[p + 1:p + 5], 'little'), p + 5
    return int.from_bytes(b[p + 1:p + 9], 'little'), p + 9


def skip_bytes(b, p):
    """Skip a compact-size-prefixed byte vector."""
    n, p = read_compact_size(b, p)
    return p + n


def parse_tx(b, p):
    """Parse one transaction at offset p, as UnserializeTransaction does.

    Returns (tx, end offset). tx is (version, ntime, txid hex), where txid hashes
    the serialization without witness data, like CTransaction::GetHash.
    """
    start = p
    version = int.from_bytes(b[p:p + 4], 'little', signed=True)
    p += 4
    flags = 0
    n_vin, p = read_compact_size(b, p)
    if n_vin == 0:
        flags = b[p]
        p += 1
        if flags != 1:
            raise ParseError(f'unknown transaction flags {flags}')
        n_vin, p = read_compact_size(b, p)
    vin_start = p
    for _ in range(n_vin):
        p += 36                                     # prevout
        p = skip_bytes(b, p)                        # scriptSig
        p += 4                                      # nSequence
    n_vout, p = read_compact_size(b, p)
    for _ in range(n_vout):
        p += 8                                      # nValue
        p = skip_bytes(b, p)                        # scriptPubKey
    vout_end = p
    if flags & 1:
        for _ in range(n_vin):
            n_items, p = read_compact_size(b, p)
            for _ in range(n_items):
                p = skip_bytes(b, p)
    tail_start = p
    p += 4                                          # nLockTime
    ntime = None
    if version > POW_TX_VERSION:
        ntime = int.from_bytes(b[p:p + 4], 'little')
        p += 4
    if p > len(b):
        raise ParseError('transaction runs past the end of the block')
    if flags:
        stripped = b[start:start + 4] + b[vin_start - len(encode_compact_size(n_vin)):vout_end] + b[tail_start:p]
    else:
        stripped = b[start:p]
    return (version, ntime, sha256d(stripped)[::-1].hex()), p


def encode_compact_size(n):
    if n < 0xfd:
        return bytes([n])
    if n <= 0xffff:
        return b'\xfd' + n.to_bytes(2, 'little')
    if n <= 0xffffffff:
        return b'\xfe' + n.to_bytes(4, 'little')
    return b'\xff' + n.to_bytes(8, 'little')


def merkle_root(txids_le):
    level = list(txids_le)
    while len(level) > 1:
        if len(level) % 2:
            level.append(level[-1])
        level = [sha256d(level[i] + level[i + 1]) for i in range(0, len(level), 2)]
    return level[0]


def parse_block(raw):
    """Parse a serialized block. Returns (block nTime, [(version, ntime, txid)])."""
    b = bytes.fromhex(raw)
    block_version = int.from_bytes(b[0:4], 'little', signed=True)
    header_merkle = b[36:68]
    block_time = int.from_bytes(b[68:72], 'little')
    n_tx, p = read_compact_size(b, 80)
    txs = []
    for _ in range(n_tx):
        tx, p = parse_tx(b, p)
        txs.append(tx)
    if block_version > POW_BLOCK_VERSION:
        p = skip_bytes(b, p)                        # vchBlockSig
    if p != len(b):
        raise ParseError(f'{len(b) - p} unparsed bytes at the end of the block')
    if merkle_root([bytes.fromhex(t[2])[::-1] for t in txs]) != header_merkle:
        raise ParseError('merkle root does not match the parsed transactions')
    return block_time, txs


def tx_role(height, index, last_pow):
    if index == 0:
        return 'coinbase'
    if index == 1 and height > last_pow:
        return 'coinstake'
    return 'regular'


def scan(args):
    if args.network not in NETWORK_DEFAULTS:
        raise RPCError(f'Unsupported --network {args.network!r}.')
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
    start = args.start_height if args.start_height is not None else 0
    stop = min(args.stop_height if args.stop_height is not None else tip, tip)

    total = max(0, stop - start + 1)
    print(f'Network        : {chain}')
    print(f'Chain tip      : {tip}')
    print(f'nLastPowHeight : {last_pow}')
    print(f'Scan range     : {start} .. {stop}  ({total} blocks)')
    print(f'Reporting      : nVersion <= {POW_TX_VERSION} above nLastPowHeight (bad-txns-version-pos)')
    print(f'                 nVersion > {POW_TX_VERSION} with nTime == 0 at any height (bad-txns-time-zero)')
    print(f'Threads        : {args.threads}', flush=True)
    if total == 0:
        print('Nothing to scan.')
        return 0

    findings = []
    version_hist = {}
    tx_count = 0
    t0 = time.time()

    vfile = None
    if args.out:
        vfile = open(args.out, 'w', encoding="utf8")
        vfile.write('rule,height,tx_index,role,txid,version,ntime,block_time\n')

    def fetch(heights):
        hashes = rpc.batch([('getblockhash', [h]) for h in heights])
        raws = rpc.batch([('getblock', [bh, 0]) for bh in hashes])
        return list(zip(heights, hashes, raws))

    def handle(height, blkhash, raw):
        nonlocal tx_count
        try:
            block_time, txs = parse_block(raw)
        except (ParseError, IndexError, ValueError) as e:
            raise RPCError(f'cannot parse block {height} {blkhash}: {e}')
        for idx, (ver, ntime, txid) in enumerate(txs):
            tx_count += 1
            version_hist[ver] = version_hist.get(ver, 0) + 1
            rule = None
            if ver <= POW_TX_VERSION and height > last_pow:
                rule = 'version-pos'
            elif ver > POW_TX_VERSION and ntime == 0:
                rule = 'time-zero'
            if rule:
                f = (rule, height, idx, tx_role(height, idx, last_pow), txid, ver, ntime if ntime is not None else '', block_time)
                findings.append(f)
                print(f'  !! {rule}  height={height} idx={idx} ({f[3]}) version={ver} '
                      f'ntime={f[6]} txid={txid}', file=sys.stderr, flush=True)
                if vfile:
                    vfile.write(','.join(str(x) for x in f) + '\n')
                    vfile.flush()

    # Process in ordered windows so progress is monotonic while each window's
    # batches are fetched concurrently.
    done = 0
    next_progress = args.progress_every
    with ThreadPoolExecutor(max_workers=args.threads) as pool:
        h = start
        while h <= stop:
            window_end = min(h + args.window * args.threads, stop + 1)
            chunks = [list(range(a, min(a + args.window, window_end))) for a in range(h, window_end, args.window)]
            for chunk in pool.map(fetch, chunks):
                for height, blkhash, raw in chunk:
                    handle(height, blkhash, raw)
            done += window_end - h
            h = window_end
            if done >= next_progress or h > stop:
                next_progress = done + args.progress_every
                elapsed = time.time() - t0
                rate = done / elapsed if elapsed else 0
                remaining = (total - done) / rate if rate else 0
                print(f'  ... {done}/{total} blocks  ({100*done/total:.1f}%)  '
                      f'height={h-1}  {rate:.0f} blk/s  ETA {remaining/60:.1f} min  '
                      f'findings={len(findings)}', flush=True)

    if vfile:
        vfile.close()

    dt = time.time() - t0
    print()
    print('=' * 68)
    print(f'Scanned {done} blocks / {tx_count} transactions in {dt/60:.1f} min')
    print('nVersion distribution:')
    for ver in sorted(version_hist):
        print(f'    v{ver}: {version_hist[ver]}')
    by_rule = {}
    for f in findings:
        by_rule.setdefault(f[0], []).append(f)
    print(f'bad-txns-version-pos candidates (v<={POW_TX_VERSION} above {last_pow}): {len(by_rule.get("version-pos", []))}')
    print(f'bad-txns-time-zero candidates (v>{POW_TX_VERSION}, nTime 0): {len(by_rule.get("time-zero", []))}')
    print('=' * 68)
    if findings:
        print(f'RESULT: FAIL: {len(findings)} transaction(s) that a rule would reject in chain history.')
        if args.out:
            print(f'Offending transactions written to {args.out}')
        for f in findings[:20]:
            print(f'    {f[0]} height={f[1]} idx={f[2]} ({f[3]}) version={f[5]} ntime={f[6]} txid={f[4]}')
        if len(findings) > 20:
            print(f'    ... and {len(findings) - 20} more')
        return 1

    print(f'RESULT: PASS: no transaction in {start}..{stop} breaks either rule.')
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
    p.add_argument('--datadir', default='~/.reddcoin', help='Datadir to find the cookie or reddcoin.conf (default: ~/.reddcoin)')
    p.add_argument('--conf', default=None, help='reddcoin.conf to read rpcuser/rpcpassword from (default: <datadir>/reddcoin.conf)')
    p.add_argument('--rpc-timeout', type=int, default=120, help='Per-request RPC timeout in seconds')
    p.add_argument('--start-height', type=int, default=None, help='First height to scan (default: 0)')
    p.add_argument('--stop-height', type=int, default=None, help='Last height to scan (default: chain tip)')
    p.add_argument('--last-pow-height', type=int, default=None, help='Override nLastPowHeight')
    p.add_argument('--threads', type=int, default=8, help='Concurrent RPC batches (default: 8)')
    p.add_argument('--window', type=int, default=64, help='Blocks per RPC batch (default: 64)')
    p.add_argument('--progress-every', type=int, default=50000, help='Progress print cadence in blocks')
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
