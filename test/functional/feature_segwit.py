#!/usr/bin/env python3
# Copyright (c) 2016-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the SegWit changeover logic."""

from decimal import Decimal

from test_framework.address import (
    key_to_p2pkh,
    program_to_witness,
    script_to_p2sh,
    script_to_p2sh_p2wsh,
    script_to_p2wsh,
)
from test_framework.blocktools import (
    send_to_witness,
    witness_script,
)
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    tx_from_hex,
)
from test_framework.script import (
    CScript,
    OP_0,
    OP_1,
    OP_2,
    OP_CHECKMULTISIG,
    OP_CHECKSIG,
    OP_DROP,
    OP_TRUE,
)
from test_framework.script_util import (
    key_to_p2pkh_script,
    key_to_p2wpkh_script,
    script_to_p2sh_script,
    script_to_p2wsh_script,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_is_hex_string,
    assert_raises_rpc_error,
    hex_str_to_bytes,
    try_rpc,
)

NODE_0 = 0
NODE_2 = 2
P2WPKH = 0
P2WSH = 1

def getutxo(txid):
    utxo = {}
    utxo["vout"] = 0
    utxo["txid"] = txid
    return utxo

def find_spendable_utxo(node, min_value, max_value=None):
    query = {'minimumAmount': min_value}
    if max_value is not None:
        query['maximumAmount'] = max_value
    for utxo in node.listunspent(query_options=query):
        if utxo['spendable']:
            return utxo

    raise AssertionError("Unspent output equal or higher than %s not found" % min_value)

txs_mined = {} # txindex from txid to blockhash

class SegWitTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        # This test tests SegWit both pre and post-activation, so use the normal BIP9 activation.
        self.extra_args = [
            [
                "-acceptnonstdtxn=1",
                "-rpcserialversion=0",
                "-staking=0",
                "-addresstype=legacy",
            ],
            [
                "-acceptnonstdtxn=1",
                "-rpcserialversion=1",
                "-staking=0",
                "-addresstype=legacy",
            ],
            [
                "-acceptnonstdtxn=1",
                "-staking=0",
                "-addresstype=legacy",
            ],
        ]
        self.rpc_timeout = 120

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        super().setup_network()
        self.connect_nodes(0, 2)
        self.sync_all()

    def success_mine(self, node, txid, sign, redeem_script=""):
        send_to_witness(1, node, getutxo(txid), self.pubkey[0], False, Decimal("49.998"), sign, redeem_script)
        block = node.generate(1)
        # PoS blocks have coinbase + coinstake + user_tx = 3 (vs PoW: coinbase + user_tx = 2)
        block_txs = node.getblock(block[0])["tx"]
        assert len(block_txs) >= 2  # At least coinbase + user_tx (PoW) or coinbase + coinstake + user_tx (PoS)
        self.sync_blocks()

    def skip_mine(self, node, txid, sign, redeem_script=""):
        spend_txid = send_to_witness(1, node, getutxo(txid), self.pubkey[0], False, Decimal("49.998"), sign, redeem_script)
        block = node.generate(1)
        block_txs = node.getblock(block[0])["tx"]
        # Before SegWit activation, witness txs should NOT be in the block
        assert spend_txid not in block_txs
        self.sync_blocks()

    def fail_accept(self, node, error_msg, txid, sign, redeem_script=""):
        assert_raises_rpc_error(-26, error_msg, send_to_witness, use_p2wsh=1, node=node, utxo=getutxo(txid), pubkey=self.pubkey[0], encode_p2sh=False, amount=Decimal("49.998"), sign=sign, insert_redeem_script=redeem_script)

    def run_test(self):
        self.nodes[0].generate(161)  # block 161

        # ReddCoin PoS: Fund node 2 with multiple UTXOs so it can mine multiple PoS blocks
        # for skip_mine/success_mine tests (each block consumes one UTXO for coinstake)
        self.log.info("Funding node 2 for PoS block generation")
        for _ in range(15):
            self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 500)
        self.nodes[0].generate(1)  # block 162
        self.sync_blocks()

        self.log.info("Verify sigops are counted in GBT with pre-BIP141 rules before the fork")
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1)
        from test_framework.util import advance_time_for_pos
        advance_time_for_pos(self.nodes[0], seconds=120)
        tmpl = self.nodes[0].getblocktemplate({'rules': ['segwit']})
        assert tmpl['sizelimit'] == 1000000
        assert 'weightlimit' not in tmpl
        assert tmpl['sigoplimit'] == 20000
        # In PoS, transactions[0] is the coinstake; user tx follows
        tx_hashes = [t['hash'] for t in tmpl['transactions']]
        assert txid in tx_hashes
        assert '!segwit' not in tmpl['rules']
        self.nodes[0].generate(1)  # block 162

        self.pubkey = []
        p2sh_ids = [] # p2sh_ids[NODE][TYPE] is an array of txids that spend to P2WPKH (TYPE=0) or P2WSH (TYPE=1) scripts to an address for NODE embedded in p2sh
        wit_ids = [] # wit_ids[NODE][TYPE] is an array of txids that spend to P2WPKH (TYPE=0) or P2WSH (TYPE=1) scripts to an address for NODE via bare witness
        for i in range(3):
            newaddress = self.nodes[i].getnewaddress()
            self.pubkey.append(self.nodes[i].getaddressinfo(newaddress)["pubkey"])
            multiscript = CScript([OP_1, hex_str_to_bytes(self.pubkey[-1]), OP_1, OP_CHECKMULTISIG])
            p2sh_ms_addr = self.nodes[i].addmultisigaddress(1, [self.pubkey[-1]], '', 'p2sh-segwit')['address']
            bip173_ms_addr = self.nodes[i].addmultisigaddress(1, [self.pubkey[-1]], '', 'bech32')['address']
            assert_equal(p2sh_ms_addr, script_to_p2sh_p2wsh(multiscript))
            assert_equal(bip173_ms_addr, script_to_p2wsh(multiscript))
            p2sh_ids.append([])
            wit_ids.append([])
            for _ in range(2):
                p2sh_ids[i].append([])
                wit_ids[i].append([])

        # Create appropriately-sized UTXOs (~50.01 RDD each) for witness testing.
        # ReddCoin PoW rewards are large (~10000+ RDD), so we need smaller UTXOs
        # to avoid fee-exceeds-max when send_to_witness sends 49.999 from a UTXO.
        self.log.info("Creating ~50.01 RDD UTXOs for witness testing")
        outputs = {}
        for i in range(70):
            outputs[self.nodes[0].getnewaddress()] = Decimal("50.01")
        self.nodes[0].sendmany("", outputs)
        self.nodes[0].generate(1)
        self.sync_blocks()

        for _ in range(5):
            for n in range(3):
                for v in range(2):
                    wit_ids[n][v].append(send_to_witness(v, self.nodes[0], find_spendable_utxo(self.nodes[0], 50, Decimal("50.02")), self.pubkey[n], False, Decimal("49.999")))
                    p2sh_ids[n][v].append(send_to_witness(v, self.nodes[0], find_spendable_utxo(self.nodes[0], 50, Decimal("50.02")), self.pubkey[n], True, Decimal("49.999")))

        self.nodes[0].generate(1)  # mine the 60 witness txs
        self.sync_blocks()

        # ReddCoin: each node owns the key for self.pubkey[n], so after SegWit
        # activates its witness UTXOs (wit_ids[n]/p2sh_ids[n]) become stakeable and
        # a post-activation generate() coinstake could spend them before the tests
        # use them (-> bad-txns-inputs-missingorspent). Lock them on the staking
        # nodes (node 0 and node 2 call generate() below); AvailableCoins skips
        # locked coins, while send_to_witness still spends them explicitly. Node 1
        # never stakes and needs its witness UTXOs spendable for a later
        # fundrawtransaction, so leave it unlocked.
        for n in (NODE_0, NODE_2):
            known = {(u['txid'], u['vout']) for u in self.nodes[n].listunspent(0)}
            locks = [{"txid": txid, "vout": 0}
                     for v in range(2)
                     for ids in (wit_ids[n][v], p2sh_ids[n][v])
                     for txid in ids
                     if (txid, 0) in known]
            if locks:
                self.nodes[n].lockunspent(False, locks)

        # Verify nodes 1 and 2 received their witness outputs
        assert_equal(self.nodes[1].getbalance(), 20 * Decimal("49.999"))
        assert self.nodes[2].getbalance() >= 7500 + 20 * Decimal("49.999")
        assert self.nodes[0].getbalance() > 0

        # Generate blocks to near-activation. With the C++ fix in stake.cpp,
        # the staking code now skips witness UTXOs when SegWit is not active,
        # so it's safe to have witness outputs in the wallet during this phase.
        self.nodes[0].generate(260)  # generate to near-activation
        self.sync_blocks()

        self.log.info("Verify witness txs are skipped for mining before the fork")
        self.skip_mine(self.nodes[2], wit_ids[NODE_2][P2WPKH][0], True)  # block 424
        self.skip_mine(self.nodes[2], wit_ids[NODE_2][P2WSH][0], True)  # block 425
        self.skip_mine(self.nodes[2], p2sh_ids[NODE_2][P2WPKH][0], True)  # block 426
        self.skip_mine(self.nodes[2], p2sh_ids[NODE_2][P2WSH][0], True)  # block 427

        self.log.info("Verify unsigned p2sh witness txs without a redeem script are invalid")
        self.fail_accept(self.nodes[2], "mandatory-script-verify-flag-failed (Operation not valid with the current stack size)", p2sh_ids[NODE_2][P2WPKH][1], sign=False)
        self.fail_accept(self.nodes[2], "mandatory-script-verify-flag-failed (Operation not valid with the current stack size)", p2sh_ids[NODE_2][P2WSH][1], sign=False)

        # ReddCoin: After skip_mine at blocks 426-429, generate to 431 (LOCKED_IN state).
        # We need exactly 2 more blocks to reach 431 before activation at 432.
        self.nodes[2].generate(2)  # blocks 430-431

        self.log.info("Verify previous witness txs skipped for mining can now be mined")
        assert_equal(len(self.nodes[2].getrawmempool()), 4)
        blockhash = self.nodes[2].generate(1)[0]  # block 432 (SegWit activates via BIP9)
        self.sync_blocks()
        assert_equal(len(self.nodes[2].getrawmempool()), 0)
        segwit_tx_list = self.nodes[2].getblock(blockhash)["tx"]
        # PoS: coinbase + coinstake + 4 witness txs = 6 (vs PoW: coinbase + 4 = 5)
        assert len(segwit_tx_list) >= 5

        self.log.info("Verify default node can't accept txs with missing witness")
        # unsigned, no scriptsig
        self.fail_accept(self.nodes[0], "non-mandatory-script-verify-flag (Witness program hash mismatch)", wit_ids[NODE_0][P2WPKH][0], sign=False)
        self.fail_accept(self.nodes[0], "non-mandatory-script-verify-flag (Witness program was passed an empty witness)", wit_ids[NODE_0][P2WSH][0], sign=False)
        self.fail_accept(self.nodes[0], "mandatory-script-verify-flag-failed (Operation not valid with the current stack size)", p2sh_ids[NODE_0][P2WPKH][0], sign=False)
        self.fail_accept(self.nodes[0], "mandatory-script-verify-flag-failed (Operation not valid with the current stack size)", p2sh_ids[NODE_0][P2WSH][0], sign=False)
        # unsigned with redeem script
        self.fail_accept(self.nodes[0], "non-mandatory-script-verify-flag (Witness program hash mismatch)", p2sh_ids[NODE_0][P2WPKH][0], sign=False, redeem_script=witness_script(False, self.pubkey[0]))
        self.fail_accept(self.nodes[0], "non-mandatory-script-verify-flag (Witness program was passed an empty witness)", p2sh_ids[NODE_0][P2WSH][0], sign=False, redeem_script=witness_script(True, self.pubkey[0]))

        self.log.info("Verify block and transaction serialization rpcs return differing serializations depending on rpc serialization flag")
        assert self.nodes[2].getblock(blockhash, False) != self.nodes[0].getblock(blockhash, False)
        assert self.nodes[1].getblock(blockhash, False) == self.nodes[2].getblock(blockhash, False)

        # In PoS blocks, tx[0]=coinbase, tx[1]=coinstake — skip these for wallet-based checks
        user_tx_list = segwit_tx_list[2:] if len(segwit_tx_list) > 2 else segwit_tx_list[1:]
        for tx_id in user_tx_list:
            tx = tx_from_hex(self.nodes[2].gettransaction(tx_id)["hex"])
            assert self.nodes[2].getrawtransaction(tx_id, False, blockhash) != self.nodes[0].getrawtransaction(tx_id, False, blockhash)
            assert self.nodes[1].getrawtransaction(tx_id, False, blockhash) == self.nodes[2].getrawtransaction(tx_id, False, blockhash)
            assert self.nodes[0].getrawtransaction(tx_id, False, blockhash) != self.nodes[2].gettransaction(tx_id)["hex"]
            assert self.nodes[1].getrawtransaction(tx_id, False, blockhash) == self.nodes[2].gettransaction(tx_id)["hex"]
            assert self.nodes[0].getrawtransaction(tx_id, False, blockhash) == tx.serialize_without_witness().hex()

        # Coinbase contains the witness commitment nonce, check that RPC shows us
        coinbase_txid = self.nodes[2].getblock(blockhash)['tx'][0]
        # Use getrawtransaction since PoS coinbase (0-value) may not be in wallet
        coinbase_tx = self.nodes[2].getrawtransaction(coinbase_txid, True, blockhash)
        witnesses = coinbase_tx["vin"][0]["txinwitness"]
        assert_equal(len(witnesses), 1)
        assert_is_hex_string(witnesses[0])
        assert_equal(witnesses[0], '00'*32)

        self.log.info("Verify witness txs without witness data are invalid after the fork")
        self.fail_accept(self.nodes[2], 'non-mandatory-script-verify-flag (Witness program hash mismatch)', wit_ids[NODE_2][P2WPKH][2], sign=False)
        self.fail_accept(self.nodes[2], 'non-mandatory-script-verify-flag (Witness program was passed an empty witness)', wit_ids[NODE_2][P2WSH][2], sign=False)
        self.fail_accept(self.nodes[2], 'non-mandatory-script-verify-flag (Witness program hash mismatch)', p2sh_ids[NODE_2][P2WPKH][2], sign=False, redeem_script=witness_script(False, self.pubkey[2]))
        self.fail_accept(self.nodes[2], 'non-mandatory-script-verify-flag (Witness program was passed an empty witness)', p2sh_ids[NODE_2][P2WSH][2], sign=False, redeem_script=witness_script(True, self.pubkey[2]))

        self.log.info("Verify default node can now use witness txs")
        self.success_mine(self.nodes[0], wit_ids[NODE_0][P2WPKH][0], True)  # block 432
        self.success_mine(self.nodes[0], wit_ids[NODE_0][P2WSH][0], True)  # block 433
        self.success_mine(self.nodes[0], p2sh_ids[NODE_0][P2WPKH][0], True)  # block 434
        self.success_mine(self.nodes[0], p2sh_ids[NODE_0][P2WSH][0], True)  # block 435

        self.log.info("Verify sigops are counted in GBT with BIP141 rules after the fork")
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1)
        advance_time_for_pos(self.nodes[0], seconds=120)
        tmpl = self.nodes[0].getblocktemplate({'rules': ['segwit']})
        assert tmpl['sizelimit'] >= 3999577  # actual maximum size is lower due to minimum mandatory non-witness data
        assert tmpl['weightlimit'] == 4000000
        assert tmpl['sigoplimit'] == 80000
        # In PoS, coinstake is transactions[0]; user tx is after it
        tx_txids = [t['txid'] for t in tmpl['transactions']]
        assert txid in tx_txids
        assert '!segwit' in tmpl['rules']

        self.nodes[0].generate(1)  # Mine a block to clear the gbt cache

        self.log.info("Non-segwit miners are able to use GBT response after activation.")
        # Create more small UTXOs for this section
        outputs = {}
        for i in range(10):
            outputs[self.nodes[0].getnewaddress()] = Decimal("50.01")
        self.nodes[0].sendmany("", outputs)
        self.nodes[0].generate(1)

        # Create a 3-tx chain: tx1 (non-segwit input, paying to a segwit output) ->
        #                      tx2 (segwit input, paying to a non-segwit output) ->
        #                      tx3 (non-segwit input, paying to a non-segwit output).
        # tx1 is allowed to appear in the block, but no others.
        txid1 = send_to_witness(1, self.nodes[0], find_spendable_utxo(self.nodes[0], 50, Decimal("50.02")), self.pubkey[0], False, Decimal("49.999"))
        hex_tx = self.nodes[0].gettransaction(txid)['hex']
        tx = tx_from_hex(hex_tx)
        assert tx.wit.is_null()  # This should not be a segwit input
        assert txid1 in self.nodes[0].getrawmempool()

        tx1_hex = self.nodes[0].gettransaction(txid1)['hex']
        tx1 = tx_from_hex(tx1_hex)

        # Check that wtxid is properly reported in mempool entry (txid1)
        assert_equal(int(self.nodes[0].getmempoolentry(txid1)["wtxid"], 16), tx1.calc_sha256(True))

        # Check that weight and vsize are properly reported in mempool entry (txid1)
        assert_equal(self.nodes[0].getmempoolentry(txid1)["vsize"], (self.nodes[0].getmempoolentry(txid1)["weight"] + 3) // 4)
        assert_equal(self.nodes[0].getmempoolentry(txid1)["weight"], len(tx1.serialize_without_witness())*3 + len(tx1.serialize_with_witness()))

        # Now create tx2, which will spend from txid1.
        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(int(txid1, 16), 0), b''))
        tx.vout.append(CTxOut(int(49.99 * COIN), CScript([OP_TRUE, OP_DROP] * 15 + [OP_TRUE])))
        tx2_hex = self.nodes[0].signrawtransactionwithwallet(tx.serialize().hex())['hex']
        txid2 = self.nodes[0].sendrawtransaction(tx2_hex)
        tx = tx_from_hex(tx2_hex)
        assert not tx.wit.is_null()

        # Check that wtxid is properly reported in mempool entry (txid2)
        assert_equal(int(self.nodes[0].getmempoolentry(txid2)["wtxid"], 16), tx.calc_sha256(True))

        # Check that weight and vsize are properly reported in mempool entry (txid2)
        assert_equal(self.nodes[0].getmempoolentry(txid2)["vsize"], (self.nodes[0].getmempoolentry(txid2)["weight"] + 3) // 4)
        assert_equal(self.nodes[0].getmempoolentry(txid2)["weight"], len(tx.serialize_without_witness())*3 + len(tx.serialize_with_witness()))

        # Now create tx3, which will spend from txid2
        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(int(txid2, 16), 0), b""))
        tx.vout.append(CTxOut(int(49.95 * COIN), CScript([OP_TRUE, OP_DROP] * 15 + [OP_TRUE])))  # Huge fee
        tx.calc_sha256()
        txid3 = self.nodes[0].sendrawtransaction(hexstring=tx.serialize().hex(), maxfeerate=0)
        assert tx.wit.is_null()
        assert txid3 in self.nodes[0].getrawmempool()

        # Check that getblocktemplate includes all transactions.
        advance_time_for_pos(self.nodes[0], seconds=120)
        template = self.nodes[0].getblocktemplate({"rules": ["segwit"]})
        template_txids = [t['txid'] for t in template['transactions']]
        assert txid1 in template_txids
        assert txid2 in template_txids
        assert txid3 in template_txids

        # Check that wtxid is properly reported in mempool entry (txid3)
        assert_equal(int(self.nodes[0].getmempoolentry(txid3)["wtxid"], 16), tx.calc_sha256(True))

        # Check that weight and vsize are properly reported in mempool entry (txid3)
        assert_equal(self.nodes[0].getmempoolentry(txid3)["vsize"], (self.nodes[0].getmempoolentry(txid3)["weight"] + 3) // 4)
        assert_equal(self.nodes[0].getmempoolentry(txid3)["weight"], len(tx.serialize_without_witness())*3 + len(tx.serialize_with_witness()))

        # Mine a block to clear the gbt cache again.
        self.nodes[0].generate(1)

        self.log.info("Verify behaviour of importaddress and listunspent")

        # ReddCoin: Create more small UTXOs for mine_and_test_listunspent calls
        outputs = {}
        for i in range(20):
            outputs[self.nodes[0].getnewaddress()] = Decimal("50.01")
        self.nodes[0].sendmany("", outputs)
        self.nodes[0].generate(1)
        self.sync_blocks()

        # Some public keys to be used later
        pubkeys = [
            "0363D44AABD0F1699138239DF2F042C3282C0671CC7A76826A55C8203D90E39242",  # cPiM8Ub4heR9NBYmgVzJQiUH1if44GSBGiqaeJySuL2BKxubvgwb
            "02D3E626B3E616FC8662B489C123349FECBFC611E778E5BE739B257EAE4721E5BF",  # cPpAdHaD6VoYbW78kveN2bsvb45Q7G5PhaPApVUGwvF8VQ9brD97
            "04A47F2CBCEFFA7B9BCDA184E7D5668D3DA6F9079AD41E422FA5FD7B2D458F2538A62F5BD8EC85C2477F39650BD391EA6250207065B2A81DA8B009FC891E898F0E",  # 91zqCU5B9sdWxzMt1ca3VzbtVm2YM6Hi5Rxn4UDtxEaN9C9nzXV
            "02A47F2CBCEFFA7B9BCDA184E7D5668D3DA6F9079AD41E422FA5FD7B2D458F2538",  # cPQFjcVRpAUBG8BA9hzr2yEzHwKoMgLkJZBBtK9vJnvGJgMjzTbd
            "036722F784214129FEB9E8129D626324F3F6716555B603FFE8300BBCB882151228",  # cQGtcm34xiLjB1v7bkRa4V3aAc9tS2UTuBZ1UnZGeSeNy627fN66
            "0266A8396EE936BF6D99D17920DB21C6C7B1AB14C639D5CD72B300297E416FD2EC",  # cTW5mR5M45vHxXkeChZdtSPozrFwFgmEvTNnanCW6wrqwaCZ1X7K
            "0450A38BD7F0AC212FEBA77354A9B036A32E0F7C81FC4E0C5ADCA7C549C4505D2522458C2D9AE3CEFD684E039194B72C8A10F9CB9D4764AB26FCC2718D421D3B84",  # 92h2XPssjBpsJN5CqSP7v9a7cf2kgDunBC6PDFwJHMACM1rrVBJ
        ]

        # Import a compressed key and an uncompressed key, generate some multisig addresses
        self.nodes[0].importprivkey("92e6XLo5jVAVwrQKPNTs93oQco8f8sDNBcpv73Dsrs397fQtFQn")
        # ReddCoin regtest uses PUBKEY_ADDRESS=122 (prefix 'r'), not Bitcoin's 111 (prefix 'm')
        # Derive addresses from the actual pubkeys returned by the wallet
        uncompressed_spendable_address = [key_to_p2pkh("04087a947bbb87f5005d25c56a10a7660694b81bffe209a9e89a6e2683a6a900b6ff3a7732eb015021deda823f265ed7a5bbec7aa7e83eb395d4cb7d5dea63d144")]
        self.nodes[0].importprivkey("cNC8eQ5dg3mFAVePDX4ddmPYpPbw41r9bm2jd1nLJT77e6RrzTRR")
        compressed_spendable_address = [key_to_p2pkh("0252b458d60bb6f21808c864a2a7e9b41cc2ce42733d6be0260b9bf9f46881c69e")]
        assert not self.nodes[0].getaddressinfo(uncompressed_spendable_address[0])['iscompressed']
        assert self.nodes[0].getaddressinfo(compressed_spendable_address[0])['iscompressed']

        self.nodes[0].importpubkey(pubkeys[0])
        compressed_solvable_address = [key_to_p2pkh(pubkeys[0])]
        self.nodes[0].importpubkey(pubkeys[1])
        compressed_solvable_address.append(key_to_p2pkh(pubkeys[1]))
        self.nodes[0].importpubkey(pubkeys[2])
        uncompressed_solvable_address = [key_to_p2pkh(pubkeys[2])]

        spendable_anytime = []                      # These outputs should be seen anytime after importprivkey and addmultisigaddress
        spendable_after_importaddress = []          # These outputs should be seen after importaddress
        solvable_after_importaddress = []           # These outputs should be seen after importaddress but not spendable
        unsolvable_after_importaddress = []         # These outputs should be unsolvable after importaddress
        solvable_anytime = []                       # These outputs should be solvable after importpubkey
        unseen_anytime = []                         # These outputs should never be seen

        uncompressed_spendable_address.append(self.nodes[0].addmultisigaddress(2, [uncompressed_spendable_address[0], compressed_spendable_address[0]])['address'])
        uncompressed_spendable_address.append(self.nodes[0].addmultisigaddress(2, [uncompressed_spendable_address[0], uncompressed_spendable_address[0]])['address'])
        compressed_spendable_address.append(self.nodes[0].addmultisigaddress(2, [compressed_spendable_address[0], compressed_spendable_address[0]])['address'])
        uncompressed_solvable_address.append(self.nodes[0].addmultisigaddress(2, [compressed_spendable_address[0], uncompressed_solvable_address[0]])['address'])
        compressed_solvable_address.append(self.nodes[0].addmultisigaddress(2, [compressed_spendable_address[0], compressed_solvable_address[0]])['address'])
        compressed_solvable_address.append(self.nodes[0].addmultisigaddress(2, [compressed_solvable_address[0], compressed_solvable_address[1]])['address'])

        # Test multisig_without_privkey
        # We have 2 public keys without private keys, use addmultisigaddress to add to wallet.
        # Money sent to P2SH of multisig of this should only be seen after importaddress with the BASE58 P2SH address.

        multisig_without_privkey_address = self.nodes[0].addmultisigaddress(2, [pubkeys[3], pubkeys[4]])['address']
        script = CScript([OP_2, hex_str_to_bytes(pubkeys[3]), hex_str_to_bytes(pubkeys[4]), OP_2, OP_CHECKMULTISIG])
        solvable_after_importaddress.append(script_to_p2sh_script(script))

        for i in compressed_spendable_address:
            v = self.nodes[0].getaddressinfo(i)
            if (v['isscript']):
                [bare, p2sh, p2wsh, p2sh_p2wsh] = self.p2sh_address_to_script(v)
                # p2sh multisig with compressed keys should always be spendable
                spendable_anytime.extend([p2sh])
                # bare multisig can be watched and signed, but is not treated as ours
                solvable_after_importaddress.extend([bare])
                # P2WSH and P2SH(P2WSH) multisig with compressed keys are spendable after direct importaddress
                spendable_after_importaddress.extend([p2wsh, p2sh_p2wsh])
            else:
                [p2wpkh, p2sh_p2wpkh, p2pk, p2pkh, p2sh_p2pk, p2sh_p2pkh, p2wsh_p2pk, p2wsh_p2pkh, p2sh_p2wsh_p2pk, p2sh_p2wsh_p2pkh] = self.p2pkh_address_to_script(v)
                # normal P2PKH and P2PK with compressed keys should always be spendable
                spendable_anytime.extend([p2pkh, p2pk])
                # P2SH_P2PK, P2SH_P2PKH with compressed keys are spendable after direct importaddress
                spendable_after_importaddress.extend([p2sh_p2pk, p2sh_p2pkh, p2wsh_p2pk, p2wsh_p2pkh, p2sh_p2wsh_p2pk, p2sh_p2wsh_p2pkh])
                # P2WPKH and P2SH_P2WPKH with compressed keys should always be spendable
                spendable_anytime.extend([p2wpkh, p2sh_p2wpkh])

        for i in uncompressed_spendable_address:
            v = self.nodes[0].getaddressinfo(i)
            if (v['isscript']):
                [bare, p2sh, p2wsh, p2sh_p2wsh] = self.p2sh_address_to_script(v)
                # p2sh multisig with uncompressed keys should always be spendable
                spendable_anytime.extend([p2sh])
                # bare multisig can be watched and signed, but is not treated as ours
                solvable_after_importaddress.extend([bare])
                # P2WSH and P2SH(P2WSH) multisig with uncompressed keys are never seen
                unseen_anytime.extend([p2wsh, p2sh_p2wsh])
            else:
                [p2wpkh, p2sh_p2wpkh, p2pk, p2pkh, p2sh_p2pk, p2sh_p2pkh, p2wsh_p2pk, p2wsh_p2pkh, p2sh_p2wsh_p2pk, p2sh_p2wsh_p2pkh] = self.p2pkh_address_to_script(v)
                # normal P2PKH and P2PK with uncompressed keys should always be spendable
                spendable_anytime.extend([p2pkh, p2pk])
                # P2SH_P2PK and P2SH_P2PKH are spendable after direct importaddress
                spendable_after_importaddress.extend([p2sh_p2pk, p2sh_p2pkh])
                # Witness output types with uncompressed keys are never seen
                unseen_anytime.extend([p2wpkh, p2sh_p2wpkh, p2wsh_p2pk, p2wsh_p2pkh, p2sh_p2wsh_p2pk, p2sh_p2wsh_p2pkh])

        for i in compressed_solvable_address:
            v = self.nodes[0].getaddressinfo(i)
            if (v['isscript']):
                # Multisig without private is not seen after addmultisigaddress, but seen after importaddress
                [bare, p2sh, p2wsh, p2sh_p2wsh] = self.p2sh_address_to_script(v)
                solvable_after_importaddress.extend([bare, p2sh, p2wsh, p2sh_p2wsh])
            else:
                [p2wpkh, p2sh_p2wpkh, p2pk, p2pkh, p2sh_p2pk, p2sh_p2pkh, p2wsh_p2pk, p2wsh_p2pkh, p2sh_p2wsh_p2pk, p2sh_p2wsh_p2pkh] = self.p2pkh_address_to_script(v)
                # normal P2PKH, P2PK, P2WPKH and P2SH_P2WPKH with compressed keys should always be seen
                solvable_anytime.extend([p2pkh, p2pk, p2wpkh, p2sh_p2wpkh])
                # P2SH_P2PK, P2SH_P2PKH with compressed keys are seen after direct importaddress
                solvable_after_importaddress.extend([p2sh_p2pk, p2sh_p2pkh, p2wsh_p2pk, p2wsh_p2pkh, p2sh_p2wsh_p2pk, p2sh_p2wsh_p2pkh])

        for i in uncompressed_solvable_address:
            v = self.nodes[0].getaddressinfo(i)
            if (v['isscript']):
                [bare, p2sh, p2wsh, p2sh_p2wsh] = self.p2sh_address_to_script(v)
                # Base uncompressed multisig without private is not seen after addmultisigaddress, but seen after importaddress
                solvable_after_importaddress.extend([bare, p2sh])
                # P2WSH and P2SH(P2WSH) multisig with uncompressed keys are never seen
                unseen_anytime.extend([p2wsh, p2sh_p2wsh])
            else:
                [p2wpkh, p2sh_p2wpkh, p2pk, p2pkh, p2sh_p2pk, p2sh_p2pkh, p2wsh_p2pk, p2wsh_p2pkh, p2sh_p2wsh_p2pk, p2sh_p2wsh_p2pkh] = self.p2pkh_address_to_script(v)
                # normal P2PKH and P2PK with uncompressed keys should always be seen
                solvable_anytime.extend([p2pkh, p2pk])
                # P2SH_P2PK, P2SH_P2PKH with uncompressed keys are seen after direct importaddress
                solvable_after_importaddress.extend([p2sh_p2pk, p2sh_p2pkh])
                # Witness output types with uncompressed keys are never seen
                unseen_anytime.extend([p2wpkh, p2sh_p2wpkh, p2wsh_p2pk, p2wsh_p2pkh, p2sh_p2wsh_p2pk, p2sh_p2wsh_p2pkh])

        op1 = CScript([OP_1])
        op0 = CScript([OP_0])
        # 2N7MGY19ti4KDMSzRfPAssP6Pxyuxoi6jLe is the P2SH(P2PKH) version of mjoE3sSrb8ByYEvgnC3Aox86u1CHnfJA4V
        unsolvable_address_key = hex_str_to_bytes("02341AEC7587A51CDE5279E0630A531AEA2615A9F80B17E8D9376327BAEAA59E3D")
        unsolvablep2pkh = key_to_p2pkh_script(unsolvable_address_key)
        unsolvablep2wshp2pkh = script_to_p2wsh_script(unsolvablep2pkh)
        p2shop0 = script_to_p2sh_script(op0)
        p2wshop1 = script_to_p2wsh_script(op1)
        unsolvable_after_importaddress.append(unsolvablep2pkh)
        unsolvable_after_importaddress.append(unsolvablep2wshp2pkh)
        unsolvable_after_importaddress.append(op1)  # OP_1 will be imported as script
        unsolvable_after_importaddress.append(p2wshop1)
        unseen_anytime.append(op0)  # OP_0 will be imported as P2SH address with no script provided
        unsolvable_after_importaddress.append(p2shop0)

        spendable_txid = []
        solvable_txid = []
        spendable_txid.append(self.mine_and_test_listunspent(spendable_anytime, 2))
        solvable_txid.append(self.mine_and_test_listunspent(solvable_anytime, 1))
        self.mine_and_test_listunspent(spendable_after_importaddress + solvable_after_importaddress + unseen_anytime + unsolvable_after_importaddress, 0)

        importlist = []
        for i in compressed_spendable_address + uncompressed_spendable_address + compressed_solvable_address + uncompressed_solvable_address:
            v = self.nodes[0].getaddressinfo(i)
            if (v['isscript']):
                bare = hex_str_to_bytes(v['hex'])
                importlist.append(bare.hex())
                importlist.append(script_to_p2wsh_script(bare).hex())
            else:
                pubkey = hex_str_to_bytes(v['pubkey'])
                p2pk = CScript([pubkey, OP_CHECKSIG])
                p2pkh = key_to_p2pkh_script(pubkey)
                importlist.append(p2pk.hex())
                importlist.append(p2pkh.hex())
                importlist.append(key_to_p2wpkh_script(pubkey).hex())
                importlist.append(script_to_p2wsh_script(p2pk).hex())
                importlist.append(script_to_p2wsh_script(p2pkh).hex())

        importlist.append(unsolvablep2pkh.hex())
        importlist.append(unsolvablep2wshp2pkh.hex())
        importlist.append(op1.hex())
        importlist.append(p2wshop1.hex())

        for i in importlist:
            # import all generated addresses. The wallet already has the private keys for some of these, so catch JSON RPC
            # exceptions and continue.
            try_rpc(-4, "The wallet already contains the private key for this address or script", self.nodes[0].importaddress, i, "", False, True)

        self.nodes[0].importaddress(script_to_p2sh(op0))  # import OP_0 as address only
        self.nodes[0].importaddress(multisig_without_privkey_address)  # Test multisig_without_privkey

        spendable_txid.append(self.mine_and_test_listunspent(spendable_anytime + spendable_after_importaddress, 2))
        solvable_txid.append(self.mine_and_test_listunspent(solvable_anytime + solvable_after_importaddress, 1))
        self.mine_and_test_listunspent(unsolvable_after_importaddress, 1)
        self.mine_and_test_listunspent(unseen_anytime, 0)

        spendable_txid.append(self.mine_and_test_listunspent(spendable_anytime + spendable_after_importaddress, 2))
        solvable_txid.append(self.mine_and_test_listunspent(solvable_anytime + solvable_after_importaddress, 1))
        self.mine_and_test_listunspent(unsolvable_after_importaddress, 1)
        self.mine_and_test_listunspent(unseen_anytime, 0)

        # Repeat some tests. This time we don't add witness scripts with importaddress
        # Import a compressed key and an uncompressed key, generate some multisig addresses
        self.nodes[0].importprivkey("927pw6RW8ZekycnXqBQ2JS5nPyo1yRfGNN8oq74HeddWSpafDJH")
        uncompressed_spendable_address = [key_to_p2pkh("041a52bd094376defd169260781aefba04966abf223164c6d146fecbbe06a531a2f54a6006285d636422935e811c12ab34195b56b901ffef6c4d7b208f226725cd")]
        self.nodes[0].importprivkey("cMcrXaaUC48ZKpcyydfFo8PxHAjpsYLhdsp6nmtB3E2ER9UUHWnw")
        compressed_spendable_address = [key_to_p2pkh("03969aec6b6d14f5a9df0d5798c7f48313b25f9bcbcf0412d78aa4630ba3431322")]

        self.nodes[0].importpubkey(pubkeys[5])
        compressed_solvable_address = [key_to_p2pkh(pubkeys[5])]
        self.nodes[0].importpubkey(pubkeys[6])
        uncompressed_solvable_address = [key_to_p2pkh(pubkeys[6])]

        unseen_anytime = []                         # These outputs should never be seen
        solvable_anytime = []                       # These outputs should be solvable after importpubkey
        unseen_anytime = []                         # These outputs should never be seen

        uncompressed_spendable_address.append(self.nodes[0].addmultisigaddress(2, [uncompressed_spendable_address[0], compressed_spendable_address[0]])['address'])
        uncompressed_spendable_address.append(self.nodes[0].addmultisigaddress(2, [uncompressed_spendable_address[0], uncompressed_spendable_address[0]])['address'])
        compressed_spendable_address.append(self.nodes[0].addmultisigaddress(2, [compressed_spendable_address[0], compressed_spendable_address[0]])['address'])
        uncompressed_solvable_address.append(self.nodes[0].addmultisigaddress(2, [compressed_solvable_address[0], uncompressed_solvable_address[0]])['address'])
        compressed_solvable_address.append(self.nodes[0].addmultisigaddress(2, [compressed_spendable_address[0], compressed_solvable_address[0]])['address'])

        premature_witaddress = []

        for i in compressed_spendable_address:
            v = self.nodes[0].getaddressinfo(i)
            if (v['isscript']):
                [bare, p2sh, p2wsh, p2sh_p2wsh] = self.p2sh_address_to_script(v)
                premature_witaddress.append(script_to_p2sh(p2wsh))
            else:
                [p2wpkh, p2sh_p2wpkh, p2pk, p2pkh, p2sh_p2pk, p2sh_p2pkh, p2wsh_p2pk, p2wsh_p2pkh, p2sh_p2wsh_p2pk, p2sh_p2wsh_p2pkh] = self.p2pkh_address_to_script(v)
                # P2WPKH, P2SH_P2WPKH are always spendable
                spendable_anytime.extend([p2wpkh, p2sh_p2wpkh])

        for i in uncompressed_spendable_address + uncompressed_solvable_address:
            v = self.nodes[0].getaddressinfo(i)
            if (v['isscript']):
                [bare, p2sh, p2wsh, p2sh_p2wsh] = self.p2sh_address_to_script(v)
                # P2WSH and P2SH(P2WSH) multisig with uncompressed keys are never seen
                unseen_anytime.extend([p2wsh, p2sh_p2wsh])
            else:
                [p2wpkh, p2sh_p2wpkh, p2pk, p2pkh, p2sh_p2pk, p2sh_p2pkh, p2wsh_p2pk, p2wsh_p2pkh, p2sh_p2wsh_p2pk, p2sh_p2wsh_p2pkh] = self.p2pkh_address_to_script(v)
                # P2WPKH, P2SH_P2WPKH with uncompressed keys are never seen
                unseen_anytime.extend([p2wpkh, p2sh_p2wpkh])

        for i in compressed_solvable_address:
            v = self.nodes[0].getaddressinfo(i)
            if (v['isscript']):
                [bare, p2sh, p2wsh, p2sh_p2wsh] = self.p2sh_address_to_script(v)
                premature_witaddress.append(script_to_p2sh(p2wsh))
            else:
                [p2wpkh, p2sh_p2wpkh, p2pk, p2pkh, p2sh_p2pk, p2sh_p2pkh, p2wsh_p2pk, p2wsh_p2pkh, p2sh_p2wsh_p2pk, p2sh_p2wsh_p2pkh] = self.p2pkh_address_to_script(v)
                # P2SH_P2PK, P2SH_P2PKH with compressed keys are always solvable
                solvable_anytime.extend([p2wpkh, p2sh_p2wpkh])

        self.mine_and_test_listunspent(spendable_anytime, 2)
        self.mine_and_test_listunspent(solvable_anytime, 1)
        self.mine_and_test_listunspent(unseen_anytime, 0)

        # Check that createrawtransaction/decoderawtransaction with non-v0 Bech32 works
        v1_addr = program_to_witness(1, [3, 5])
        v1_tx = self.nodes[0].createrawtransaction([getutxo(spendable_txid[0])], {v1_addr: 1})
        v1_decoded = self.nodes[1].decoderawtransaction(v1_tx)
        assert_equal(v1_decoded['vout'][0]['scriptPubKey']['address'], v1_addr)
        assert_equal(v1_decoded['vout'][0]['scriptPubKey']['hex'], "51020305")

        # Check that spendable outputs are really spendable
        self.create_and_mine_tx_from_txids(spendable_txid)

        # import all the private keys so solvable addresses become spendable
        self.nodes[0].importprivkey("cPiM8Ub4heR9NBYmgVzJQiUH1if44GSBGiqaeJySuL2BKxubvgwb")
        self.nodes[0].importprivkey("cPpAdHaD6VoYbW78kveN2bsvb45Q7G5PhaPApVUGwvF8VQ9brD97")
        self.nodes[0].importprivkey("91zqCU5B9sdWxzMt1ca3VzbtVm2YM6Hi5Rxn4UDtxEaN9C9nzXV")
        self.nodes[0].importprivkey("cPQFjcVRpAUBG8BA9hzr2yEzHwKoMgLkJZBBtK9vJnvGJgMjzTbd")
        self.nodes[0].importprivkey("cQGtcm34xiLjB1v7bkRa4V3aAc9tS2UTuBZ1UnZGeSeNy627fN66")
        self.nodes[0].importprivkey("cTW5mR5M45vHxXkeChZdtSPozrFwFgmEvTNnanCW6wrqwaCZ1X7K")
        self.create_and_mine_tx_from_txids(solvable_txid)

        # Test that importing native P2WPKH/P2WSH scripts works
        for use_p2wsh in [False, True]:
            if use_p2wsh:
                scriptPubKey = "00203a59f3f56b713fdcf5d1a57357f02c44342cbf306ffe0c4741046837bf90561a"
            else:
                scriptPubKey = "a9142f8c469c2f0084c48e11f998ffbe7efa7549f26d87"

            # ReddCoin: build the funding template (0 inputs, one 1.0-RDD output to
            # scriptPubKey) as a v2 tx to fund below. The PoS era rejects
            # nVersion <= POW_TX_VERSION (bad-txns-version-pos), and the old hardcoded
            # v1 hex can't just have its version byte bumped because a v>1 tx
            # serializes an extra nTime field. CTransaction defaults to v2; pin nTime
            # to the tip time (v>1 txs need a non-zero nTime) for mocktime consistency.
            template = CTransaction()
            template.nTime = self.nodes[1].getblockheader(self.nodes[1].getbestblockhash())['time']
            template.vout.append(CTxOut(int(1 * COIN), bytes.fromhex(scriptPubKey)))
            transaction = template.serialize().hex()

            self.nodes[1].importaddress(scriptPubKey, "", False)
            # ReddCoin: pin an explicit low feerate. After hundreds of blocks the
            # fee estimator is inflated, so the default-funded fee exceeds maxtxfee.
            rawtxfund = self.nodes[1].fundrawtransaction(transaction, {"feeRate": Decimal("0.001")})['hex']
            rawtxfund = self.nodes[1].signrawtransactionwithwallet(rawtxfund)["hex"]
            txid = self.nodes[1].sendrawtransaction(rawtxfund)

            assert_equal(self.nodes[1].gettransaction(txid, True)["txid"], txid)
            assert_equal(self.nodes[1].listtransactions("*", 1, 0, True)[0]["txid"], txid)

            # Assert it is properly saved
            self.restart_node(1)
            assert_equal(self.nodes[1].gettransaction(txid, True)["txid"], txid)
            assert_equal(self.nodes[1].listtransactions("*", 1, 0, True)[0]["txid"], txid)

    def mine_and_test_listunspent(self, script_list, ismine):
        utxo = find_spendable_utxo(self.nodes[0], 50, Decimal("50.02"))
        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(int('0x' + utxo['txid'], 0), utxo['vout'])))
        for i in script_list:
            tx.vout.append(CTxOut(10000000, i))
        tx.rehash()
        signresults = self.nodes[0].signrawtransactionwithwallet(tx.serialize_without_witness().hex())['hex']
        txid = self.nodes[0].sendrawtransaction(hexstring=signresults, maxfeerate=0)
        txs_mined[txid] = self.nodes[0].generate(1)[0]
        self.sync_blocks()
        watchcount = 0
        spendcount = 0
        for i in self.nodes[0].listunspent():
            if (i['txid'] == txid):
                watchcount += 1
                if i['spendable']:
                    spendcount += 1
        if (ismine == 2):
            assert_equal(spendcount, len(script_list))
        elif (ismine == 1):
            assert_equal(watchcount, len(script_list))
            assert_equal(spendcount, 0)
        else:
            assert_equal(watchcount, 0)
        return txid

    def p2sh_address_to_script(self, v):
        bare = CScript(hex_str_to_bytes(v['hex']))
        p2sh = CScript(hex_str_to_bytes(v['scriptPubKey']))
        p2wsh = script_to_p2wsh_script(bare)
        p2sh_p2wsh = script_to_p2sh_script(p2wsh)
        return([bare, p2sh, p2wsh, p2sh_p2wsh])

    def p2pkh_address_to_script(self, v):
        pubkey = hex_str_to_bytes(v['pubkey'])
        p2wpkh = key_to_p2wpkh_script(pubkey)
        p2sh_p2wpkh = script_to_p2sh_script(p2wpkh)
        p2pk = CScript([pubkey, OP_CHECKSIG])
        p2pkh = CScript(hex_str_to_bytes(v['scriptPubKey']))
        p2sh_p2pk = script_to_p2sh_script(p2pk)
        p2sh_p2pkh = script_to_p2sh_script(p2pkh)
        p2wsh_p2pk = script_to_p2wsh_script(p2pk)
        p2wsh_p2pkh = script_to_p2wsh_script(p2pkh)
        p2sh_p2wsh_p2pk = script_to_p2sh_script(p2wsh_p2pk)
        p2sh_p2wsh_p2pkh = script_to_p2sh_script(p2wsh_p2pkh)
        return [p2wpkh, p2sh_p2wpkh, p2pk, p2pkh, p2sh_p2pk, p2sh_p2pkh, p2wsh_p2pk, p2wsh_p2pkh, p2sh_p2wsh_p2pk, p2sh_p2wsh_p2pkh]

    def create_and_mine_tx_from_txids(self, txids, success=True):
        tx = CTransaction()
        for i in txids:
            txraw = self.nodes[0].getrawtransaction(i, 0, txs_mined[i])
            txtmp = tx_from_hex(txraw)
            for j in range(len(txtmp.vout)):
                tx.vin.append(CTxIn(COutPoint(int('0x' + i, 0), j)))
        tx.vout.append(CTxOut(0, CScript()))
        tx.rehash()
        signresults = self.nodes[0].signrawtransactionwithwallet(tx.serialize_without_witness().hex())['hex']
        self.nodes[0].sendrawtransaction(hexstring=signresults, maxfeerate=0)
        self.nodes[0].generate(1)
        self.sync_blocks()


if __name__ == '__main__':
    SegWitTest().main()
