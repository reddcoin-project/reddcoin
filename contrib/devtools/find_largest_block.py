#!/usr/bin/env python3
# Copyright (c) 2026 The Reddcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Scan Reddcoin mainnet over a height range via JSON-RPC and report the
largest block(s) by serialized size, so a representative block can be captured
as benchmark data (bench/data/*.raw).

Optionally dump the largest block's raw serialization, and/or save a whole
range of raw blocks plus a manifest for later deserialize/CheckBlock
verification against real chain data.

Auth resolution order (first that works wins):
  1. --rpcuser / --rpcpassword on the command line
  2. rpcuser / rpcpassword from the reddcoin.conf ([main] section preferred)
  3. the .cookie file in the data directory (auto cookie auth)

Examples:
  # Largest block in a recent window, and dump it to a .raw file:
  ./find_largest_block.py --start 6000000 --end 6520000 --dump --out /tmp/biggest.raw

  # Whole chain (slow; shows progress), just report the top 10:
  ./find_largest_block.py --top 10

  # Save every raw block in a range for a later verification corpus:
  ./find_largest_block.py --start 413560 --end 413600 --save-range ./corpus
"""

import argparse
import base64
import heapq
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

DEFAULT_PORT = 45443            # reddcoind mainnet RPC (chainparamsbase.cpp)
DEFAULT_DATADIR = Path.home() / ".reddcoin"
DEFAULT_CONF = DEFAULT_DATADIR / "reddcoin.conf"


def parse_conf(conf_path):
    """Return a dict of config values, preferring the [main] section over the
    global scope for keys that appear in both."""
    glob, main = {}, {}
    section = None
    if not conf_path or not os.path.exists(conf_path):
        return {}
    with open(conf_path) as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip().lower()
                continue
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            (main if section == "main" else glob)[k.strip()] = v.strip()
    merged = dict(glob)
    merged.update(main)
    return merged


def resolve_auth(args):
    """Return (auth_header_value, description)."""
    conf = parse_conf(args.conf)
    user = args.rpcuser or conf.get("rpcuser")
    pw = args.rpcpassword or conf.get("rpcpassword")
    if user and pw:
        token = base64.b64encode(f"{user}:{pw}".encode()).decode()
        return "Basic " + token, f"user/password ({user})"
    # Fall back to cookie auth.
    cookie = Path(args.datadir) / ".cookie"
    if cookie.exists():
        token = base64.b64encode(cookie.read_text().strip().encode()).decode()
        return "Basic " + token, f"cookie ({cookie})"
    sys.exit(
        "No RPC credentials found. Pass --rpcuser/--rpcpassword, set them in "
        f"{args.conf}, or ensure {cookie} exists (node running).")


class RPC:
    def __init__(self, url, auth):
        self.url = url
        self.auth = auth

    def _post(self, payload):
        data = json.dumps(payload).encode()
        req = urllib.request.Request(
            self.url, data=data,
            headers={"Content-Type": "text/plain", "Authorization": self.auth})
        try:
            with urllib.request.urlopen(req, timeout=600) as r:
                return json.loads(r.read().decode())
        except urllib.error.HTTPError as e:
            body = e.read().decode(errors="replace")
            try:
                return json.loads(body)
            except Exception:
                raise RuntimeError(f"HTTP {e.code} from RPC: {body[:200]}")
        except urllib.error.URLError as e:
            raise RuntimeError(f"Cannot reach RPC at {self.url}: {e.reason}")

    def call(self, method, *params):
        resp = self._post({"jsonrpc": "1.0", "id": "1",
                           "method": method, "params": list(params)})
        if resp.get("error"):
            raise RuntimeError(f"RPC error in {method}: {resp['error']}")
        return resp["result"]

    def batch(self, calls):
        """calls: list of (method, [params]). Returns results in order."""
        payload = [{"jsonrpc": "1.0", "id": i, "method": m, "params": p}
                   for i, (m, p) in enumerate(calls)]
        resp = self._post(payload)
        out = [None] * len(calls)
        for item in resp:
            i = item["id"]
            if item.get("error"):
                raise RuntimeError(f"RPC batch error ({calls[i][0]}): {item['error']}")
            out[i] = item["result"]
        return out


def block_sizes(rpc, heights, batch_size):
    """Yield (height, hash, size) for each height, using batched RPC."""
    for i in range(0, len(heights), batch_size):
        chunk = heights[i:i + batch_size]
        hashes = rpc.batch([("getblockhash", [h]) for h in chunk])
        infos = rpc.batch([("getblock", [h, 1]) for h in hashes])
        for h, bh, info in zip(chunk, hashes, infos):
            yield h, bh, int(info["size"])


def main():
    ap = argparse.ArgumentParser(
        description="Find the largest Reddcoin mainnet block(s) over a height range.",
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--rpchost", default="127.0.0.1")
    ap.add_argument("--rpcport", type=int, default=None,
                    help=f"default: rpcport from conf, else {DEFAULT_PORT}")
    ap.add_argument("--rpcuser")
    ap.add_argument("--rpcpassword")
    ap.add_argument("--conf", default=str(DEFAULT_CONF),
                    help=f"reddcoin.conf path (default {DEFAULT_CONF})")
    ap.add_argument("--datadir", default=str(DEFAULT_DATADIR),
                    help="data dir for cookie auth")
    ap.add_argument("--start", type=int, default=0, help="first height (default 0)")
    ap.add_argument("--end", type=int, default=None,
                    help="last height inclusive (default: chain tip)")
    ap.add_argument("--batch-size", type=int, default=200,
                    help="heights per RPC batch (default 200)")
    ap.add_argument("--top", type=int, default=10,
                    help="how many of the largest blocks to report (default 10)")
    ap.add_argument("--dump", action="store_true",
                    help="write the single largest block's raw bytes to --out")
    ap.add_argument("--out", default=None,
                    help="output path for --dump (default ./block_<height>.raw)")
    ap.add_argument("--save-range", metavar="DIR", default=None,
                    help="write every block in the range as <height>.raw plus "
                         "manifest.json (corpus for later verification)")
    args = ap.parse_args()

    conf = parse_conf(args.conf)
    port = args.rpcport or int(conf.get("rpcport", DEFAULT_PORT))
    host = args.rpchost or conf.get("rpcconnect", "127.0.0.1")
    auth, how = resolve_auth(args)
    rpc = RPC(f"http://{host}:{port}", auth)

    tip = int(rpc.call("getblockcount"))
    end = args.end if args.end is not None else tip
    end = min(end, tip)
    start = max(0, args.start)
    if start > end:
        sys.exit(f"start {start} > end {end}")

    heights = list(range(start, end + 1))
    print(f"Auth: {how}", file=sys.stderr)
    print(f"Chain tip {tip}; scanning heights {start}..{end} "
          f"({len(heights)} blocks) in batches of {args.batch_size}", file=sys.stderr)

    # Optional corpus dir.
    corpus = None
    manifest = []
    if args.save_range:
        corpus = Path(args.save_range)
        corpus.mkdir(parents=True, exist_ok=True)

    # Keep only the largest blocks in a bounded min-heap so a full-chain scan
    # stays O(keep) in memory instead of retaining every block.
    keep = max(args.top, 20)
    heap = []  # min-heap of (size, height, hash)
    t0 = time.time()
    done = 0
    for height, bh, size in block_sizes(rpc, heights, args.batch_size):
        if len(heap) < keep:
            heapq.heappush(heap, (size, height, bh))
        elif size > heap[0][0]:
            heapq.heapreplace(heap, (size, height, bh))
        if corpus is not None:
            raw = bytes.fromhex(rpc.call("getblock", bh, 0))
            (corpus / f"{height}.raw").write_bytes(raw)
            manifest.append({"height": height, "hash": bh, "size": size})
        done += 1
        if done % 2000 == 0 or done == len(heights):
            rate = done / max(1e-6, time.time() - t0)
            print(f"  {done}/{len(heights)} scanned ({rate:.0f} blk/s)", file=sys.stderr)

    biggest = sorted(heap, reverse=True)
    topn = biggest[:args.top]
    print(f"\nTop {len(topn)} largest blocks in {start}..{end}:")
    print(f"{'height':>10}  {'size':>10}  hash")
    for size, height, bh in topn:
        print(f"{height:>10}  {size:>10}  {bh}")

    if corpus is not None:
        (corpus / "manifest.json").write_text(json.dumps(manifest, indent=2))
        print(f"\nSaved {len(manifest)} raw blocks + manifest.json to {corpus}")

    if args.dump and topn:
        size, height, bh = topn[0]
        out = Path(args.out) if args.out else Path(f"block_{height}.raw")
        raw = bytes.fromhex(rpc.call("getblock", bh, 0))
        assert len(raw) == size, f"raw {len(raw)} != reported size {size}"
        out.write_bytes(raw)
        print(f"\nDumped largest block height {height} ({size} bytes) to {out}")
        print(f"  hash {bh}")


if __name__ == "__main__":
    main()
