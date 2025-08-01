#!/usr/bin/env python3
# Copyright (c) 2014-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the rawtransaction RPCs.

Test the following RPCs:
   - getrawtransaction
   - createrawtransaction
   - signrawtransactionwithwallet
   - sendrawtransaction
   - decoderawtransaction
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.messages import (
    CTransaction,
    tx_from_hex,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    find_vout_for_address,
)


TXID = "1d1d4e24ed99057e84c3f80fd8fbec79ed9e1acee37da269356ecea000000000"


class multidict(dict):
    """Dictionary that allows duplicate keys.

    Constructed with a list of (key, value) tuples. When dumped by the json module,
    will output invalid json with repeated keys, eg:
    >>> json.dumps(multidict([(1,2),(1,2)])
    '{"1": 2, "1": 2}'

    Used to test calls to rpc methods with repeated keys in the json object."""

    def __init__(self, x):
        dict.__init__(self, x)
        self.x = x

    def items(self):
        return self.x


class RawTransactionsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 4
        self.extra_args = [
            ["-txindex"],
            ["-txindex"],
            ["-txindex"],
            [],
        ]
        # whitelist all peers to speed up tx relay / mempool sync
        for args in self.extra_args:
            args.append("-whitelist=noban@127.0.0.1")

        self.supports_cli = False

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        super().setup_network()
        self.connect_nodes(0, 2)

    def run_test(self):
        self.log.info("Prepare some coins for multiple *rawtransaction commands")
        self.generate(self.nodes[2], 1)
        self.generate(self.nodes[0], COINBASE_MATURITY + 1)
        for amount in [1.5, 1.0, 5.0]:
            self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), amount)
        self.sync_all()
        self.generate(self.nodes[0], 5)

        self.getrawtransaction_tests()
        self.createrawtransaction_tests()
        self.signrawtransactionwithwallet_tests()
        self.sendrawtransaction_tests()
        self.sendrawtransaction_testmempoolaccept_tests()
        self.decoderawtransaction_tests()
        self.transaction_version_number_tests()
        if not self.options.descriptors:
            self.raw_multisig_transaction_legacy_tests()

    def getrawtransaction_tests(self):
        addr = self.nodes[1].getnewaddress()
        txid = self.nodes[0].sendtoaddress(addr, 10)
        self.generate(self.nodes[0], 1)
        vout = find_vout_for_address(self.nodes[1], txid, addr)
        rawTx = self.nodes[1].createrawtransaction([{'txid': txid, 'vout': vout}], [{self.nodes[1].getnewaddress(): 9.999}, {"fee": 0.001}])
        rawTxSigned = self.nodes[1].signrawtransactionwithwallet(rawTx)
        txId = self.nodes[1].sendrawtransaction(rawTxSigned['hex'])
        self.generateblock(self.nodes[0], output=self.nodes[0].getnewaddress(), transactions=[rawTxSigned['hex']])
        err_msg = (
            "No such mempool transaction. Use -txindex or provide a block hash to enable"
            " blockchain transaction queries. Use gettransaction for wallet transactions."
        )

        for n in [0, 3]:
            self.log.info(f"Test getrawtransaction {'with' if n == 0 else 'without'} -txindex")

            if n == 0:
                # With -txindex.
                # 1. valid parameters - only supply txid
                assert_equal(self.nodes[n].getrawtransaction(txId), rawTxSigned['hex'])

                # 2. valid parameters - supply txid and 0 for non-verbose
                assert_equal(self.nodes[n].getrawtransaction(txId, 0), rawTxSigned['hex'])

                # 3. valid parameters - supply txid and False for non-verbose
                assert_equal(self.nodes[n].getrawtransaction(txId, False), rawTxSigned['hex'])

                # 4. valid parameters - supply txid and 1 for verbose.
                # We only check the "hex" field of the output so we don't need to update this test every time the output format changes.
                assert_equal(self.nodes[n].getrawtransaction(txId, 1)["hex"], rawTxSigned['hex'])

                # 5. valid parameters - supply txid and True for non-verbose
                assert_equal(self.nodes[n].getrawtransaction(txId, True)["hex"], rawTxSigned['hex'])
            else:
                # Without -txindex, expect to raise.
                for verbose in [None, 0, False, 1, True]:
                    assert_raises_rpc_error(-5, err_msg, self.nodes[n].getrawtransaction, txId, verbose)

            # 6. invalid parameters - supply txid and invalid boolean values (strings) for verbose
            for value in ["True", "False"]:
                assert_raises_rpc_error(-1, "not a boolean", self.nodes[n].getrawtransaction, txid=txId, verbose=value)

            # 7. invalid parameters - supply txid and empty array
            assert_raises_rpc_error(-1, "not a boolean", self.nodes[n].getrawtransaction, txId, [])

            # 8. invalid parameters - supply txid and empty dict
            assert_raises_rpc_error(-1, "not a boolean", self.nodes[n].getrawtransaction, txId, {})

        # Make a tx by sending, then generate 2 blocks; block1 has the tx in it
        tx = self.nodes[2].sendtoaddress(self.nodes[1].getnewaddress(), 1)
        block1, block2 = self.generate(self.nodes[2], 2)
        for n in [0, 3]:
            self.log.info(f"Test getrawtransaction {'with' if n == 0 else 'without'} -txindex, with blockhash")
            # We should be able to get the raw transaction by providing the correct block
            gottx = self.nodes[n].getrawtransaction(txid=tx, verbose=True, blockhash=block1)
            assert_equal(gottx['txid'], tx)
            assert_equal(gottx['in_active_chain'], True)
            if n == 0:
                self.log.info("Test getrawtransaction with -txindex, without blockhash: 'in_active_chain' should be absent")
                gottx = self.nodes[n].getrawtransaction(txid=tx, verbose=True)
                assert_equal(gottx['txid'], tx)
                assert 'in_active_chain' not in gottx
            else:
                self.log.info("Test getrawtransaction without -txindex, without blockhash: expect the call to raise")
                assert_raises_rpc_error(-5, err_msg, self.nodes[n].getrawtransaction, txid=tx, verbose=True)
            # We should not get the tx if we provide an unrelated block
            assert_raises_rpc_error(-5, "No such transaction found", self.nodes[n].getrawtransaction, txid=tx, blockhash=block2)
            # An invalid block hash should raise the correct errors
            assert_raises_rpc_error(-1, "JSON value is not a string as expected", self.nodes[n].getrawtransaction, txid=tx, blockhash=True)
            assert_raises_rpc_error(-8, "parameter 3 must be of length 64 (not 6, for 'foobar')", self.nodes[n].getrawtransaction, txid=tx, blockhash="foobar")
            assert_raises_rpc_error(-8, "parameter 3 must be of length 64 (not 8, for 'abcd1234')", self.nodes[n].getrawtransaction, txid=tx, blockhash="abcd1234")
            foo = "ZZZ0000000000000000000000000000000000000000000000000000000000000"
            assert_raises_rpc_error(-8, f"parameter 3 must be hexadecimal string (not '{foo}')", self.nodes[n].getrawtransaction, txid=tx, blockhash=foo)
            bar = "0000000000000000000000000000000000000000000000000000000000000000"
            assert_raises_rpc_error(-5, "Block hash not found", self.nodes[n].getrawtransaction, txid=tx, blockhash=bar)
            # Undo the blocks and verify that "in_active_chain" is false.
            self.nodes[n].invalidateblock(block1)
            gottx = self.nodes[n].getrawtransaction(txid=tx, verbose=True, blockhash=block1)
            assert_equal(gottx['in_active_chain'], False)
            self.nodes[n].reconsiderblock(block1)
            assert_equal(self.nodes[n].getbestblockhash(), block2)

        self.log.info("Test getrawtransaction on genesis block coinbase returns an error")
        block = self.nodes[0].getblock(self.nodes[0].getblockhash(0))
        assert_raises_rpc_error(-5, "The genesis block coinbase is not considered an ordinary transaction", self.nodes[0].getrawtransaction, block['merkleroot'])

    def createrawtransaction_tests(self):
        self.log.info("Test createrawtransaction")
        # Test `createrawtransaction` required parameters
        assert_raises_rpc_error(-1, "createrawtransaction", self.nodes[0].createrawtransaction)
        assert_raises_rpc_error(-1, "createrawtransaction", self.nodes[0].createrawtransaction, [])

        # Test `createrawtransaction` invalid extra parameters
        # ELEMENTS: we have extra elements arguments
        assert_raises_rpc_error(-1, "createrawtransaction", self.nodes[0].createrawtransaction, [], [], 0, False, 'foo')

        # Test `createrawtransaction` invalid `inputs`
        assert_raises_rpc_error(-3, "Expected type array", self.nodes[0].createrawtransaction, 'foo', {})
        assert_raises_rpc_error(-1, "JSON value is not an object as expected", self.nodes[0].createrawtransaction, ['foo'], [])
        assert_raises_rpc_error(-1, "JSON value is not a string as expected", self.nodes[0].createrawtransaction, [{}], [])
        assert_raises_rpc_error(-8, "txid must be of length 64 (not 3, for 'foo')", self.nodes[0].createrawtransaction, [{'txid': 'foo'}], [])
        txid = "ZZZ7bb8b1697ea987f3b223ba7819250cae33efacb068d23dc24859824a77844"
        assert_raises_rpc_error(-8, f"txid must be hexadecimal string (not '{txid}')", self.nodes[0].createrawtransaction, [{'txid': txid, 'vout': 0}], [{}])
        # ELEMENTS: these are rejected because txid is not hex
        # assert_raises_rpc_error(-8, "Invalid parameter, missing vout key", self.nodes[0].createrawtransaction, [{'txid': txid}], [])
        # assert_raises_rpc_error(-8, "Invalid parameter, missing vout key", self.nodes[0].createrawtransaction, [{'txid': txid, 'vout': 'foo'}], [])
        # assert_raises_rpc_error(-8, "Invalid parameter, vout cannot be negative", self.nodes[0].createrawtransaction, [{'txid': txid, 'vout': -1}], [])
        # sequence number out of range
        for invalid_seq in [-1, 4294967296]:
            inputs = [{'txid': TXID, 'vout': 1, 'sequence': invalid_seq}]
            outputs = [{self.nodes[0].getnewaddress(): 1}]
            assert_raises_rpc_error(-8, 'Invalid parameter, sequence number is out of range',
                                    self.nodes[0].createrawtransaction, inputs, outputs)
        # with valid sequence number
        for valid_seq in [1000, 4294967294]:
            inputs = [{'txid': TXID, 'vout': 1, 'sequence': valid_seq}]
            outputs = [{self.nodes[0].getnewaddress(): 1}]
            rawtx = self.nodes[0].createrawtransaction(inputs, outputs)
            decrawtx = self.nodes[0].decoderawtransaction(rawtx)
            assert_equal(decrawtx['vin'][0]['sequence'], valid_seq)

        # Test `createrawtransaction` invalid `outputs`
        address = self.nodes[0].getnewaddress()
        address2 = self.nodes[0].getnewaddress()
        assert_raises_rpc_error(-3, "Expected type array, got string", self.nodes[0].createrawtransaction, [], 'foo')
        self.nodes[0].createrawtransaction(inputs=[], outputs=[])
        assert_raises_rpc_error(-8, "Data must be hexadecimal string", self.nodes[0].createrawtransaction, [], [{'data': 'foo'}])
        assert_raises_rpc_error(-5, "Invalid Bitcoin address", self.nodes[0].createrawtransaction, [], [{'foo': 0}])
        assert_raises_rpc_error(-3, "Invalid amount", self.nodes[0].createrawtransaction, [], [{address: 'foo'}])
        assert_raises_rpc_error(-3, "Amount out of range", self.nodes[0].createrawtransaction, [], [{address: -1}])
        assert_raises_rpc_error(-8, "Invalid parameter, duplicated address and asset: %s" % address, self.nodes[0].createrawtransaction, [], [{address: 1}, {address: 1}])
        assert_raises_rpc_error(-8, "Invalid parameter, duplicate key: data", self.nodes[0].createrawtransaction, [], [{"data": 'aa'}, {"data": "bb"}])
        assert_raises_rpc_error(-1, "JSON value is not an object as expected", self.nodes[0].createrawtransaction, [], [['key-value pair1'], ['2']])

        # Test `createrawtransaction` invalid `locktime`
        assert_raises_rpc_error(-3, "Expected type number", self.nodes[0].createrawtransaction, [], [], 'foo')
        assert_raises_rpc_error(-8, "Invalid parameter, locktime out of range", self.nodes[0].createrawtransaction, [], [], -1)
        assert_raises_rpc_error(-8, "Invalid parameter, locktime out of range", self.nodes[0].createrawtransaction, [], [], 4294967296)

        # Test `createrawtransaction` invalid `replaceable`
        assert_raises_rpc_error(-3, "Expected type bool", self.nodes[0].createrawtransaction, [], [], 0, 'foo')

        # Test that createrawtransaction accepts an array and object as outputs
        # One output
        tx = tx_from_hex(self.nodes[2].createrawtransaction(inputs=[{'txid': TXID, 'vout': 9}], outputs=[{address: 99}]))
        assert_equal(len(tx.vout), 1)
        assert_equal(
            tx.serialize().hex(),
            self.nodes[2].createrawtransaction(inputs=[{'txid': TXID, 'vout': 9}], outputs=[{address: 99}]),
        )
        # Two outputs
        tx = tx_from_hex(self.nodes[2].createrawtransaction(inputs=[{'txid': TXID, 'vout': 9}], outputs=[{address: 99}, {address2: 99}]))
        assert_equal(len(tx.vout), 2)
        assert_equal(
            tx.serialize().hex(),
            self.nodes[2].createrawtransaction(inputs=[{'txid': TXID, 'vout': 9}], outputs=[{address: 99}, {address2: 99}]),
        )
        # Multiple mixed outputs
        tx = tx_from_hex(self.nodes[2].createrawtransaction(inputs=[{'txid': TXID, 'vout': 9}], outputs=[{address: 99}, {address2: 99}, {'data': '99'}]))
        assert_equal(len(tx.vout), 3)
        assert_equal(
            tx.serialize().hex(),
            self.nodes[2].createrawtransaction(inputs=[{'txid': TXID, 'vout': 9}], outputs=[{address: 99}, {address2: 99}, {'data': '99'}]),
        )

    def signrawtransactionwithwallet_tests(self):
        for type in ["bech32", "p2sh-segwit", "legacy"]:
            self.log.info(f"Test signrawtransactionwithwallet with missing prevtx info ({type})")
            addr = self.nodes[0].getnewaddress("", type)
            addrinfo = self.nodes[0].getaddressinfo(addr)
            pubkey = addrinfo["scriptPubKey"]
            inputs = [{'txid': TXID, 'vout': 3, 'sequence': 1000}]
            outputs = [{self.nodes[0].getnewaddress(): 1}]
            rawtx = self.nodes[0].createrawtransaction(inputs, outputs)


            prevtx = dict(txid=TXID, scriptPubKey=pubkey, vout=3, amount=1)
            succ = self.nodes[0].signrawtransactionwithwallet(rawtx, [prevtx])
            assert succ["complete"]

            if type == "legacy":
                del prevtx["amount"]
                succ = self.nodes[0].signrawtransactionwithwallet(rawtx, [prevtx])
                assert succ["complete"]
            else:
                assert_raises_rpc_error(-3, "Missing amount", self.nodes[0].signrawtransactionwithwallet, rawtx, [
                    {
                        "txid": TXID,
                        "scriptPubKey": pubkey,
                        "vout": 3,
                    }
                ])

            assert_raises_rpc_error(-3, "Missing vout", self.nodes[0].signrawtransactionwithwallet, rawtx, [
                {
                    "txid": TXID,
                    "scriptPubKey": pubkey,
                    "amount": 1,
                }
            ])
            assert_raises_rpc_error(-3, "Missing txid", self.nodes[0].signrawtransactionwithwallet, rawtx, [
                {
                    "scriptPubKey": pubkey,
                    "vout": 3,
                    "amount": 1,
                }
            ])
            assert_raises_rpc_error(-3, "Missing scriptPubKey", self.nodes[0].signrawtransactionwithwallet, rawtx, [
                {
                    "txid": TXID,
                    "vout": 3,
                    "amount": 1
                }
            ])

    def sendrawtransaction_tests(self):
        self.log.info('sendrawtransaction with missing input')
        inputs  = [ {'txid' : "1d1d4e24ed99057e84c3f80fd8fbec79ed9e1acee37da269356ecea000000000", 'vout' : 1}] #won't exists
        outputs = [{ self.nodes[0].getnewaddress() : 4.998 }]
        rawtx   = self.nodes[2].createrawtransaction(inputs, outputs)
        rawtx   = self.nodes[2].signrawtransactionwithwallet(rawtx)
        assert_raises_rpc_error(-25, "bad-txns-inputs-missingorspent", self.nodes[2].sendrawtransaction, rawtx['hex'])

    def sendrawtransaction_testmempoolaccept_tests(self):
        self.log.info("Test sendrawtransaction/testmempoolaccept with maxfeerate")
        fee_exceeds_max = "Fee exceeds maximum configured by user (e.g. -maxtxfee, maxfeerate)"

        # Test a transaction with a small fee.
        txId = self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 1.0)
        rawTx = self.nodes[0].getrawtransaction(txId, True)
        vout = next(o for o in rawTx['vout'] if o['value'] == Decimal('1.00000000'))

        self.sync_all()
        inputs = [{"txid": txId, "vout": vout['n']}]
        # Fee 10,000 satoshis, (1 - (10000 sat * 0.00000001 BTC/sat)) = 0.9999
        outputs = [{self.nodes[0].getnewaddress(): Decimal("0.99990000")}, {"fee": 0.0001}]
        rawTx = self.nodes[2].createrawtransaction(inputs, outputs)
        rawTxSigned = self.nodes[2].signrawtransactionwithwallet(rawTx)
        assert_equal(rawTxSigned['complete'], True)
        # Fee 10,000 satoshis, ~100 b transaction, fee rate should land around 100 sat/byte = 0.00100000 BTC/kB
        # Thus, testmempoolaccept should reject
        testres = self.nodes[2].testmempoolaccept([rawTxSigned['hex']], 0.00001000)[0]
        assert_equal(testres['allowed'], False)
        assert_equal(testres['reject-reason'], 'max-fee-exceeded')
        # and sendrawtransaction should throw
        assert_raises_rpc_error(-25, fee_exceeds_max, self.nodes[2].sendrawtransaction, rawTxSigned['hex'], 0.00001000)
        # and the following calls should both succeed
        testres = self.nodes[2].testmempoolaccept(rawtxs=[rawTxSigned['hex']])[0]
        assert_equal(testres['allowed'], True)
        self.nodes[2].sendrawtransaction(hexstring=rawTxSigned['hex'])

        # Test a transaction with a large fee.
        txId = self.nodes[0].sendtoaddress(self.nodes[2].getnewaddress(), 1.0)
        rawTx = self.nodes[0].getrawtransaction(txId, True)
        vout = next(o for o in rawTx['vout'] if o['value'] == Decimal('1.00000000'))

        self.sync_all()
        inputs = [{"txid": txId, "vout": vout['n']}]
        # Fee 2,000,000 satoshis, (1 - (2000000 sat * 0.00000001 BTC/sat)) = 0.98
        outputs = [{self.nodes[0].getnewaddress() : Decimal("0.98000000")}, {"fee": 0.02}]
        rawTx = self.nodes[2].createrawtransaction(inputs, outputs)
        rawTxSigned = self.nodes[2].signrawtransactionwithwallet(rawTx)
        assert_equal(rawTxSigned['complete'], True)
        # Fee 2,000,000 satoshis, ~100 b transaction, fee rate should land around 20,000 sat/byte = 0.20000000 BTC/kB
        # Thus, testmempoolaccept should reject
        testres = self.nodes[2].testmempoolaccept([rawTxSigned['hex']])[0]
        assert_equal(testres['allowed'], False)
        assert_equal(testres['reject-reason'], 'max-fee-exceeded')
        # and sendrawtransaction should throw
        assert_raises_rpc_error(-25, fee_exceeds_max, self.nodes[2].sendrawtransaction, rawTxSigned['hex'])
        # and the following calls should both succeed
        testres = self.nodes[2].testmempoolaccept(rawtxs=[rawTxSigned['hex']], maxfeerate='0.20000000')[0]
        assert_equal(testres['allowed'], True)
        self.nodes[2].sendrawtransaction(hexstring=rawTxSigned['hex'], maxfeerate='0.20000000')

        self.log.info("Test sendrawtransaction/testmempoolaccept with tx already in the chain")
        self.generate(self.nodes[2], 1)
        for node in self.nodes:
            testres = node.testmempoolaccept([rawTxSigned['hex']])[0]
            assert_equal(testres['allowed'], False)
            assert_equal(testres['reject-reason'], 'txn-already-known')
            assert_raises_rpc_error(-27, 'Transaction already in block chain', node.sendrawtransaction, rawTxSigned['hex'])

    def decoderawtransaction_tests(self):
        self.log.info("Test decoderawtransaction")
        # witness transaction
        # new in core:
        #encrawtx = "010000000001010000000000000072c1a6a246ae63f74f931e8365e15a089c68d61900000000000000000000ffffffff0100e1f50500000000000102616100000000"
        encrawtx = "0100000001010000000000000072c1a6a246ae63f74f931e8365e15a089c68d61900000000000000000000ffffffff0101ac2e6a47e85fdc2a5a27334544440f2f5135553a7476f4f5e3b9792da6a58fe0010000000005f5e100000000000000000001026161000000"
        decrawtx = self.nodes[0].decoderawtransaction(encrawtx, True)  # decode as witness transaction
        assert_equal(decrawtx['vout'][0]['value'], Decimal('1.00000000'))

    def transaction_version_number_tests(self):
        self.log.info("Test transaction version numbers")

        # Test the minimum transaction version number that fits in a signed 32-bit integer.
        # As transaction version is unsigned, this should convert to its unsigned equivalent.
        tx = CTransaction()
        tx.nVersion = -0x80000000
        rawtx = tx.serialize().hex()
        decrawtx = self.nodes[0].decoderawtransaction(rawtx)
        assert_equal(decrawtx['version'], 0x80000000)

        # Test the maximum transaction version number that fits in a signed 32-bit integer.
        tx = CTransaction()
        tx.nVersion = 0x7fffffff
        rawtx = tx.serialize().hex()
        decrawtx = self.nodes[0].decoderawtransaction(rawtx)
        assert_equal(decrawtx['version'], 0x7fffffff)

    def raw_multisig_transaction_legacy_tests(self):
        self.log.info("Test raw multisig transactions (legacy)")
        # The traditional multisig workflow does not work with descriptor wallets so these are legacy only.
        # The multisig workflow with descriptor wallets uses PSBTs and is tested elsewhere, no need to do them here.

        # 2of2 test
        addr1 = self.nodes[2].getnewaddress()
        addr2 = self.nodes[2].getnewaddress()

        addr1Obj = self.nodes[2].getaddressinfo(addr1)
        addr2Obj = self.nodes[2].getaddressinfo(addr2)

        # Tests for createmultisig and addmultisigaddress
        assert_raises_rpc_error(-5, "Invalid public key", self.nodes[0].createmultisig, 1, ["01020304"])
        # createmultisig can only take public keys
        self.nodes[0].createmultisig(2, [addr1Obj['pubkey'], addr2Obj['pubkey']])
        # addmultisigaddress can take both pubkeys and addresses so long as they are in the wallet, which is tested here
        assert_raises_rpc_error(-5, "Invalid public key", self.nodes[0].createmultisig, 2, [addr1Obj['pubkey'], addr1])

        mSigObj = self.nodes[2].addmultisigaddress(2, [addr1Obj['pubkey'], addr1])['address']

        # use balance deltas instead of absolute values
        bal = self.nodes[2].getbalance()

        # send 1.2 BTC to msig adr
        txId = self.nodes[0].sendtoaddress(mSigObj, 1.2)
        self.sync_all()
        self.generate(self.nodes[0], 1)
        # node2 has both keys of the 2of2 ms addr, tx should affect the balance
        assert_equal(self.nodes[2].getbalance()['bitcoin'], bal['bitcoin'] + Decimal('1.20000000'))


        # 2of3 test from different nodes
        bal = self.nodes[2].getbalance()
        addr1 = self.nodes[1].getnewaddress()
        addr2 = self.nodes[2].getnewaddress()
        addr3 = self.nodes[2].getnewaddress()

        addr1Obj = self.nodes[1].getaddressinfo(addr1)
        addr2Obj = self.nodes[2].getaddressinfo(addr2)
        addr3Obj = self.nodes[2].getaddressinfo(addr3)

        mSigObj = self.nodes[2].addmultisigaddress(2, [addr1Obj['pubkey'], addr2Obj['pubkey'], addr3Obj['pubkey']])['address']

        txId = self.nodes[0].sendtoaddress(mSigObj, 2.2)
        decTx = self.nodes[0].gettransaction(txId)
        rawTx = self.nodes[0].decoderawtransaction(decTx['hex'])
        self.sync_all()
        self.generate(self.nodes[0], 1)

        # THIS IS AN INCOMPLETE FEATURE
        # NODE2 HAS TWO OF THREE KEYS AND THE FUNDS SHOULD BE SPENDABLE AND COUNT AT BALANCE CALCULATION
        assert_equal(self.nodes[2].getbalance(), bal)  # for now, assume the funds of a 2of3 multisig tx are not marked as spendable

        txDetails = self.nodes[0].gettransaction(txId, True)
        rawTx = self.nodes[0].decoderawtransaction(txDetails['hex'])
        vout = next(o for o in rawTx['vout'] if o['value'] == Decimal('2.20000000'))

        bal = self.nodes[0].getbalance()
        inputs = [{"txid": txId, "vout": vout['n'], "scriptPubKey": vout['scriptPubKey']['hex'], "amount": vout['value']}]
        outputs = [{self.nodes[0].getnewaddress(): 2.19}, {"fee": 0.01}]
        rawTx = self.nodes[2].createrawtransaction(inputs, outputs)
        rawTxPartialSigned = self.nodes[1].signrawtransactionwithwallet(rawTx, inputs)
        assert_equal(rawTxPartialSigned['complete'], False)  # node1 only has one key, can't comp. sign the tx

        rawTxSigned = self.nodes[2].signrawtransactionwithwallet(rawTx, inputs)
        assert_equal(rawTxSigned['complete'], True)  # node2 can sign the tx compl., own two of three keys
        self.nodes[2].sendrawtransaction(rawTxSigned['hex'])
        rawTx = self.nodes[0].decoderawtransaction(rawTxSigned['hex'])
        self.sync_all()
        self.generate(self.nodes[0], 1)
        assert_equal(self.nodes[0].getbalance()['bitcoin'], bal['bitcoin'] + Decimal('50.00000000') + Decimal('2.19000000'))  # block reward + tx

        # 2of2 test for combining transactions
        bal = self.nodes[2].getbalance()
        addr1 = self.nodes[1].getnewaddress()
        addr2 = self.nodes[2].getnewaddress()

        addr1Obj = self.nodes[1].getaddressinfo(addr1)
        addr2Obj = self.nodes[2].getaddressinfo(addr2)

        self.nodes[1].addmultisigaddress(2, [addr1Obj['pubkey'], addr2Obj['pubkey']])['address']
        mSigObj = self.nodes[2].addmultisigaddress(2, [addr1Obj['pubkey'], addr2Obj['pubkey']])['address']
        mSigObjValid = self.nodes[2].getaddressinfo(mSigObj)

        txId = self.nodes[0].sendtoaddress(mSigObj, 2.2)
        decTx = self.nodes[0].gettransaction(txId)
        rawTx2 = self.nodes[0].decoderawtransaction(decTx['hex'])
        self.sync_all()
        self.generate(self.nodes[0], 1)

        assert_equal(self.nodes[2].getbalance(), bal)  # the funds of a 2of2 multisig tx should not be marked as spendable

        txDetails = self.nodes[0].gettransaction(txId, True)
        rawTx2 = self.nodes[0].decoderawtransaction(txDetails['hex'])
        vout = next(o for o in rawTx2['vout'] if o['value'] == Decimal('2.20000000'))

        bal = self.nodes[0].getbalance()
        inputs = [{"txid": txId, "vout": vout['n'], "scriptPubKey": vout['scriptPubKey']['hex'], "redeemScript": mSigObjValid['hex'], "amount": vout['value']}]
        outputs = [{self.nodes[0].getnewaddress(): 2.19}, {"fee": 0.01}]
        rawTx2 = self.nodes[2].createrawtransaction(inputs, outputs)
        rawTxPartialSigned1 = self.nodes[1].signrawtransactionwithwallet(rawTx2, inputs)
        self.log.debug(rawTxPartialSigned1)
        assert_equal(rawTxPartialSigned1['complete'], False)  # node1 only has one key, can't comp. sign the tx

        rawTxPartialSigned2 = self.nodes[2].signrawtransactionwithwallet(rawTx2, inputs)
        self.log.debug(rawTxPartialSigned2)
        assert_equal(rawTxPartialSigned2['complete'], False)  # node2 only has one key, can't comp. sign the tx
        rawTxComb = self.nodes[2].combinerawtransaction([rawTxPartialSigned1['hex'], rawTxPartialSigned2['hex']])
        self.log.debug(rawTxComb)
        self.nodes[2].sendrawtransaction(rawTxComb)
        rawTx2 = self.nodes[0].decoderawtransaction(rawTxComb)
        self.sync_all()
        self.generate(self.nodes[0], 1)
        assert_equal(self.nodes[0].getbalance()['bitcoin'], bal['bitcoin'] + Decimal('50.00000000') + Decimal('2.19000000'))  # block reward + tx


if __name__ == '__main__':
    RawTransactionsTest().main()
