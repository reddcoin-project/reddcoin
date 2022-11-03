// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>

#include <chainparamsseeds.h>
#include <consensus/merkle.h>
#include <deploymentinfo.h>
#include <hash.h> // for signet block challenge hash
#include <util/system.h>

#include <assert.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTimeTx, uint32_t nTimeBlock, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew(nTimeTx);
    txNew.nVersion = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.nTime    = nTimeBlock;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nVersion = nVersion;
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

/**
 * Build the genesis block. Note that the output of its generation
 * transaction cannot be spent since it did not originally exist in the
 * database.
 *
 * CBlock(hash=000000000019d6, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=4a5e1e, nTime=1231006505, nBits=1d00ffff, nNonce=2083236893, vtx=1)
 *   CTransaction(hash=4a5e1e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d0104455468652054696d65732030332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b73)
 *     CTxOut(nValue=50.00000000, scriptPubKey=0x5F1DF16B2B704C8A578D0B)
 *   vMerkleTree: 4a5e1e
 */
static CBlock CreateGenesisBlock(uint32_t nTimeTx, uint32_t nTimeBlock, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "January 21st 2014 was such a nice day...";
    const CScript genesisOutputScript = CScript() << ParseHex("040184710fa689ad5023690c80f3a49c8f13f8d45b8c857fbcbc8bc4a8e4d3eb4b10f4d4604fa08dce601aaf0f470216fe1b51850b4acf21b179c45070ac7b03a9") << OP_CHECKSIG;
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTimeTx, nTimeBlock, nNonce, nBits, nVersion, genesisReward);
}

/**
 * Main network on which people trade goods and services.
 */
class CMainParams : public CChainParams {
public:
    CMainParams() {
        strNetworkID = CBaseChainParams::MAIN;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 210000;
        consensus.nLastPowHeight = 260799;
        consensus.nRevertCoinbase = 254208; //! disregard bip34 from this height (?)
        consensus.nCoinbaseMaturity = 30;
        consensus.BIP16Exception = uint256S("0x0000000000000000000000000000000000000000000000000000000000000000");
        consensus.BIP34Height = std::numeric_limits<int>::max();
        consensus.BIP34Hash = uint256S("0x0000000000000000000000000000000000000000000000000000000000000000");
        consensus.BIP65Height = std::numeric_limits<int>::max();
        consensus.BIP66Height = 1564232; // a6e944cc38a0d8c7c5740569501e622ec2a011e7c9dd97a78e5fba40a45b8c61
        consensus.MinBIP9WarningHeight = std::numeric_limits<int>::max();
        consensus.devScript = { CScript() << ParseHex("03c8fc5c87f00bcc32b5ce5c036957f8befeff05bf4d88d2dcde720249f78d9313") << OP_CHECKSIG };

        /* pow specific */
        consensus.powLimit = uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"); //! << 20
        consensus.posLimit = uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"); //! << 20
        consensus.posReset = uint256S("00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"); //! << 32
        consensus.nPowTargetTimespan = 24 * 60 * 60;
        consensus.nPowTargetSpacing = 60;
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;

        /* pos specific */
        consensus.nStakeMinAge = 8 * 60 * 60; // 8 hours
        consensus.nStakeMaxAge = 45 * 24 *  60 * 60; // 45 days
        consensus.nModifierInterval = 13 * 60;

        consensus.nRuleChangeActivationThreshold = 12960; // 90% of 14400
        consensus.nMinerConfirmationWindow = 14400; // (nPowTargetTimespan / nPowTargetSpacing) * 10 (10 Days)
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        // Deployment of BIP34.
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].bit = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE; // Sat Oct 01 2022 00:00:00 GMT+0000
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT; // Sun Oct 01 2023 00:00:00 GMT+0000

        // Deployment of BIP65.
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].bit = 1;
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE; // Tue Nov 01 2022 00:00:00 GMT+0000
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT; // Wed Nov 01 2023 00:00:00 GMT+0000

        // Deployment of BIP68, BIP112, and BIP113.
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE; // Thu Dec 01 2022 00:00:00 GMT+0000
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT; // Fri Dec 01 2023 00:00:00 GMT+0000

        // Deployment of SegWit (BIP141, BIP143, and BIP147)
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].bit = 3;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE; // Sun Jan 01 2023 00:00:00 GMT+0000
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT; // Mon Jan 01 2024 00:00:00 GMT+0000

        // Deployment of Taproot (BIPs 340-342)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 4;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation delay

        consensus.nMinimumChainWork = uint256S("0x0000000000000000000000000000000000000000000000000000000000000000");
        consensus.defaultAssumeValid = uint256S("0x0000000000000000000000000000000000000000000000000000000000000000");

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        pchMessageStart[0] = 0xfb;
        pchMessageStart[1] = 0xc0;
        pchMessageStart[2] = 0xb6;
        pchMessageStart[3] = 0xdb;
        nDefaultPort = 45444;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 0;

        genesis = CreateGenesisBlock(1390280400, 1390280400, 222583475, 0x1e0ffff0, 1, 10000 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("b868e0d95a3c3c0e0dadc67ee587aaf9dc8acbf99e3b4b3110fad4eb74c1decc"));
        assert(genesis.hashMerkleRoot == uint256S("b502bc1dc42b07092b9187e92f70e32f9a53247feae16d821bebffa916af79ff"));

        // Note that of those which support the service bits prefix, most only support a subset of
        // possible options.
        // This is fine at runtime as we'll fall back to using them as an addrfetch if they don't support the
        // service bits we want, but we should get them updated to support all service bits wanted by any
        // release ASAP to avoid it where possible.
        vSeeds.emplace_back("seed.reddcoin.net");
        vSeeds.emplace_back("reddcoin.com");
        vSeeds.emplace_back("dnsseed01.redd.ink");
        vSeeds.emplace_back("dnsseed02.redd.ink");
        vSeeds.emplace_back("dnsseed03.redd.ink");

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,61);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,5);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,189);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        bech32_hrp = "rdd";

        // Reddcoin BIP44 cointype in mainnet is '4'
        nExtCoinType = 4;

        vFixedSeeds.clear();

        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        m_is_test_chain = false;
        m_is_mockable_chain = false;

        checkpointData = {
            {
                {     10, uint256S("0xa198c38a77555a9fbff0b147bf7ce0660416d6abdaa86adaa3a9be97092592ed")},
                {   1000, uint256S("0x9d849e078deac30d58372db898318186cf5073a7f0b109b4776393b21b7b4e5a")},
                {   2000, uint256S("0x4674c50137c1d9bf47d96dee5e8c38c41895d494a0bb71e243d1c8a6c805e1f7")},
                {   3000, uint256S("0x0deff246b8dfc102ccdbc3a306649be82c441e1da0fba8ca1075cf6b5d7f3c01")},
                {   4000, uint256S("0xad880a4c23d511f04311e98ee77f5163e54cd92f80433e9f3bd6bc2261d50592")},
                {   5000, uint256S("0x3f673ef045f4a7d71fb841ce96ed190b20569182bd3dfe628527ec3a7934d08f")},
                {   6000, uint256S("0x1222056e58dce70c0a6638e07415bd6190fa5ccdd6d5e7f6af68abb30ebd4eb8")},
                {   6150, uint256S("0xe221b12cf8b0c264697e9bb3c9c9f0f7ada5f2736e054cbd53b94852908cdbd3")},
                {  10000, uint256S("0x35d5f9cbd94c15771d5ebebf55ea4bfc649c473c0a868fe4d1832f5b45bd5d0c")},
                {  15000, uint256S("0x87a8c4289e661720095f2ab6a077151bc84b9615a53c5e7207ba1c20418bd178")},
                {  20000, uint256S("0x6a86a4cbbcea694027591ba416ae3831b4f3f9aa3cc6a0a1b5627f920dd765bb")},
                {  44878, uint256S("0xd81a3724a81b78e762821d16bfe23565576905685b2c072ea9a3fa7d36dbad8b")},
                {  45189, uint256S("0xd10b5da162b922d3cf09c44ea3d533a203c9ab1c442015d7e72f21062d20aeb4")},
                {  45239, uint256S("0xe14dba7c7d1ed1a7566e23b0ca0989e3e26099b7beaa673d324001d1291223f7")},
                { 114834, uint256S("0xdc5c776ca006c6d40e48c90aeeb43bf6493de589e28f779b486e64aa3403344a")},
                { 184000, uint256S("0xe22e6b027cd49cd9aa2ba6df0e0c264c2eed5656b1fa39052c8235d52f2b04d6")},
                { 244999, uint256S("0x0b7bb56edfae2f2f1e71ac39daab16614fccf1a1e02c58d4169521d76d880b42")},
                { 285319, uint256S("0x4cc87e04718ecc7972f7639481002cd6f4c8f37846390cb50007eddccb64c73c")},
                { 325639, uint256S("0x77a09ff950d4a25325395ca9b90b1bfb9b00a9b9eb7beb919c9bcbebe9ced05f")},
                { 365959, uint256S("0xc54de093f57aed303a8cf23752a62f724f4e92605680a41be1d7bad71be69206")},
                { 406279, uint256S("0x2fb13b9504d3e5817b12b2e7291256a1c5cbdc327ea4b232558142a96bc4cffd")},
                { 446599, uint256S("0x7748fb2b7058c4001ef37a6bd8067f2314cb96acc4603fb2c35eb3d1595b3c78")},
                { 486919, uint256S("0xf751a1cfe32c1cfddbd5db4d925a1f45f3a6ab680afcd82c8e37c5df4bcb5294")},
                { 527239, uint256S("0x476aa826c0a4f61edc66684aa3be1d22e21363262710f944e1cb69052116841e")},
                { 567559, uint256S("0x334db8b00ce5ae2202d02beaaca028d9082b0d3415ca29b3f4b164306d99d11f")},
                { 607879, uint256S("0x1f97ac7d62896aef736c13918a8a63854c55c1b3b4aea668fa68a475a6ce5d1c")},
                { 648199, uint256S("0xbc28b257140edd7823c3c68527d8f659c2cded7e72b9e2d8b1b451e2a583b71c")},
                { 688519, uint256S("0x6be18349c18743418a2ae44d9e59fd7e44af0dd118836e3ac3997ceaca7fb06e")},
                { 728839, uint256S("0xffde2f99b00291f5972215e196a3ed0f95f7993e692e5f189c0ac5b6dc48c21e")},
                { 769159, uint256S("0xe30e85d460eafa3787bc46b91dca3795aa47196fa4e2a4294033dffb2e995605")},
                { 809479, uint256S("0xfe410999f834c8ec50935789f98e0e8b91ae9ae6c6f2153f047e2763b7c2696f")},
                { 849799, uint256S("0x59b8677a52fd5c487185c08bd7f7a2d957d7e407c2a1e3d1570f2c90e2a14740")},
                { 890119, uint256S("0xa20cf4b103dc081f4e57fa17b3a7a3d42d973d2da070bff2c83b2cb9b17f67cb")},
                { 930439, uint256S("0xb65e5bb7b7973ddc87db097833c5bb7ee563495702d21e3b92cdf4538e6313ea")},
                { 970759, uint256S("0xfe377082ffa049df27761c55c54b3bae58d4b9b52f04a514164e21a2d71dad1a")},
                {1011079, uint256S("0x2c667186705704e64d2acc7331e30f72d79b76f34d6c19ab59e8bec0317ae10b")},
                {1051399, uint256S("0x94d1c7526079f885cc62a7a9c58a7b4ee1624c15a7352bddce092fb7cc3ca520")},
                {1091719, uint256S("0x8456b8b6d1eef1ca71d176e49948b5125c38ac413797674c8fcf0691a2f875ca")},
                {1132039, uint256S("0xb2a8999b48ce4212d64fa8be809419b979931ffdb8e0963c18feebd9b9222802")},
                {1172359, uint256S("0x56e0863848054793910c8a814742fb09b2e26926a542c5c21cdcb8adce44c2f8")},
                {1212679, uint256S("0xbce76bde00be65672fd8e73cb2fb8f1ff77554d7454c9f373d3937bd409cc2ae")},
                {1252999, uint256S("0x0a5a3797b50426ff7d7d61a26dd638b4d9b450c986eef9f595230cf4eec8d43d")},
                {1293319, uint256S("0x34abf8942d6ef1b7c2b7c54469bb5976a0e42a0b46d5b2a9edce653ef7407c82")},
                {1333639, uint256S("0x5043a64580937b53dada19b53d06aeac35a22a0138f3ebe7552eec9de3496cb9")},
                {1373959, uint256S("0x62d3853820d0a686941ee70d57a05d6a4bf1f2041b4a30a6d5a0c9938cc0e3b9")},
                {1414279, uint256S("0x160611c311ff2432625ab12721185f6f17589116564cd30a843d9e4e243026c3")},
                {1454599, uint256S("0xe0e8469d711b9202ca32ee8860770b08a18baaf317b94e52c51436d01d74b2e8")},
                {1494919, uint256S("0x52b04f7e196a32034a5927b0d6faa6aa66ecb83563b12a2b2bc097b963028917")},
                {1535239, uint256S("0x5e88742aaddc5522c95924a435b42edc8ac77e2efa9bc3fa0d883c92795f0384")},
                {1575559, uint256S("0x0dedc34ed4cf8ab7d142fbc0d46bb4df6e670ea081061a3fac375346a79bf604")},
                {1615879, uint256S("0x488de615fe220a7c1e89db59c58389bbb80dddfb93fe6d8a0bb935876164fb41")},
                {1656199, uint256S("0xf4374f80000ac3d26d1db27351374c9487df4187df63bd1b2fa040fcc3996b7a")},
                {1696519, uint256S("0xb0a14b1743e834c674d79397a02cb866f28c081b2d8b64050a50611a6b47f8b8")},
                {1736839, uint256S("0x6f44a08e7a09893d95dc2271628a451d932b53782e292da9a197a6c4b7c72b9e")},
                {1777159, uint256S("0xfafa0f25ed1c75a58148af890a1c871dddbaf043a753793b5fe2f47502edda98")},
                {1817479, uint256S("0x42ce470859a46c77e5774db27b2d00b7c0265c4d2556d8f1106aee7006ef03f8")},
                {1857799, uint256S("0xb144603ed32f83d8d35b4b7cebef7f6cd3c40e1d418322c7831a54de454fc629")},
                {1898119, uint256S("0x43e0547f8b9138649fdcb3e3d590cc29ec658060bd1cffc24b316798c5892caf")},
                {1938439, uint256S("0x6fe379f36055fdae8cceb610ca991989a54903024be645a736722e3bd998a6d3")},
                {1978759, uint256S("0xfa4c90ce464816fec8dce0ff6060207e4440e765b464ab07e69c9d08d506b19e")},
                {2019079, uint256S("0x62e7ee7ae512eebe15b83fb929ac14084f7abd5d56329cf67d521a8289def91b")},
                {2059399, uint256S("0x1465b7307f87f25b86949850a070f6e57dbf82201c94ed9d6298802baa8cd48b")},
                {2099719, uint256S("0x64a0b5d2255a35b9fa25fbbf424e060822f9bf527caee87af979782f75f7f8fe")},
                {2140039, uint256S("0xefab922a28b266339d349c77904186fcc9fa61047be3dd283f927a11e37afab3")},
                {2180359, uint256S("0x68eb8ae6eb80f826f3a40c8e3274e73ea7be787732e7e18206274703ebd2b758")},
                {2220679, uint256S("0x97df9dfb9de984b8a1e8dc206bd5c54ac97607edab676137948388c9918a7479")},
                {2260999, uint256S("0x10afaff132a9d85877a95c8a480d42586920137559df1f631cba2db9cb9ea01c")},
                {2301319, uint256S("0x5d7a21c52624e7ad348915ad6ad7e39d0dbd5906327c2f205fa4073cc9d35b81")},
                {2341639, uint256S("0xe8723dc93a313d7248031128714118dcc8e4a69fc7f75a820820f5fdfa701740")},
                {2381959, uint256S("0x6d24d8f34b0979c75de3907f434a98881d669deb10baa8d2367585cb5e5f743b")},
                {2422279, uint256S("0x18ea3b905655fd6e90136409074a14becc97eaa97157216cb7a6733dcdef6e93")},
                {2462599, uint256S("0x4ea0d74708e601187f2bb501e913dc8eea8ab5fd16bcb18bece9200a089456e7")},
                {2502919, uint256S("0x3b5765e5c86a6c168b6d4abcf7648248b5528cd6c5d2f21ead95e2c6e4f7f200")},
                {2543239, uint256S("0xcf69068aaedb3f9c2dea524ffbd23204a0d5dc54fe5a724f4ef2154008a7d381")},
                {2583559, uint256S("0x61d8efefc796f02098ad1d361ff0e9de1652ec36007953647f9c99a695882110")},
                {2623879, uint256S("0xde13a7bedd88beac6be7042c971f307da882b55e555cbde99aa05299fd35172e")},
                {2664199, uint256S("0xbf5a342935484775d0121f75e9f0131bc5c0a14d70941774add832e39b2f31fd")},
                {2704519, uint256S("0x6ea30e05944d823d14297b7a815c5481c07ed036e6807148d7fe474715adf167")},
                {2744839, uint256S("0x364d13facdc900e879f2e2f7db0b44a4c090004bcd1b7adc8fdebcc01a787e2d")},
                {2785159, uint256S("0x5fb71d131544372542405bc64143620388d10abafdd4581a06516db77e621ef9")},
                {2825479, uint256S("0x3175360e06db1096dd90286fa0885d00310843d3af6ba0367d63f05a1bff1272")},
                {2865799, uint256S("0x9b674f80b52af1266bcba63c2f56afb27f6450017437cf555d7afb2ea8c51551")},
                {2906119, uint256S("0xfe77c3953a272dad34267534216c5d61fd51314ee7238d1ac18e48e1c8580e95")},
                {2946439, uint256S("0x3bd913f17fda7afb134eee707d19667da61506ffe43f43ac0f3862600f853fba")},
                {2986759, uint256S("0xa7ce2b440bb607cf0469451165089772ddc09a12508a6e3ab81ff1a3ba014242")},
                {3027079, uint256S("0x67ac62feac2997f95fe56f519984e8df758dc0d855ffeef8e1ba20ee870f34c1")},
                {3067399, uint256S("0x09b90ba8825d1e29fcb95291abcfd22769380e1d83e2c22c6ac1c46bcbc9eb8c")},
                {3107719, uint256S("0x20295578ddc0604bc44362cc4d6bef04549336e18740b0845b6fa119e282496a")},
                {3148039, uint256S("0x1fe291200fa247d2ea1e1a33a0788734a646a033ada28b1a31fb1ce9805e4497")},
                {3188359, uint256S("0x3a69bba8b252a461d0e76333cc50fec19a8fcfd4ad5bb7a8ce9c0e8ef7284b94")},
                {3228679, uint256S("0x20b15eac55ba0cef31977e540d9034a0fdba574a3cb02c0f02b64ee947216eac")},
                {3268999, uint256S("0x02bcaeebf00136b943cdd30832147e1f36f063cb6f71df52b6d0e55b5c633b5f")}, 
                {3309319, uint256S("0x68ff1ef71586f083ab77090f60e52bc8bd121734baadf8b5c6afbada869649ae")}, 
            }
        };

        m_assumeutxo_data = MapAssumeutxo{
         // TODO to be specified in a future patch.
        };

        chainTxData = ChainTxData{
            // Data from RPC: getchaintxstats 4096 00000000000000000008a89e854d57e5667df88f1cdef6fde2fbca1de5b639ad
            /* nTime    */ 1632457661,
            /* nTxCount */ 11100239,
            /* dTxRate  */ 0.03697349,
        };
    }
};

/**
 * Testnet (v3): public test network which is reset from time to time.
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams() {
        strNetworkID = CBaseChainParams::TESTNET;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 210000;
        consensus.nLastPowHeight = 1439;
        consensus.nCoinbaseMaturity = 50;
        consensus.BIP16Exception = uint256S("0x00000000000002dc756eebf4f49723ed8d30cc28a5f108eb94b1ba88ac4f9c22");
        consensus.BIP34Height = 1227931;
        consensus.BIP34Hash = uint256S("0x000000000000024b89b42a942fe0d9fea3bb44ab7bd1b19115dd6a759c0808b8");
        consensus.BIP65Height = 388381; // 000000000000000004c2b624ed5d7756c508d90fd0da2c7c679febfa6c4735f0
        consensus.BIP66Height = 2189; // 84c24e7ec0023d9cf2ba50366f2a1806c30e7256606aaeb31ebd17a4c0a82e9b
        consensus.MinBIP9WarningHeight = 483840; // segwit activation height + miner confirmation window
        consensus.devScript = { CScript() << ParseHex("03d4b22ae69b0ff7554f4c343cd213d00fd5131466cc21d8ebfab97c52ec9a00c9") << OP_CHECKSIG, // Correct dev address
                                CScript() << ParseHex("03081542439583f7632ce9ff7c8851b0e9f56d0a6db9a13645ce102a8809287d4f") << OP_CHECKSIG };

        /* pow specific */
        consensus.powLimit = uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimit = uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"); //! << 20
        consensus.posReset = uint256S("00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"); //! << 32
        consensus.nPowTargetTimespan = 24 * 60 * 60; // 24 hours
        consensus.nPowTargetSpacing = 60;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = false;

        /* pos specific */
        consensus.nStakeMinAge = 8 * 60 * 60; // 8 hours
        consensus.nStakeMaxAge = 45 * 24 *  60 * 60; // 45 days
        consensus.nModifierInterval = 13 * 60;

        consensus.nRuleChangeActivationThreshold = 1815; // 90% of 2016
        consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        // Deployment of BIP34.
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].bit = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].nStartTime = 1664582400; // Sat Oct 01 2022 00:00:00 GMT+0000
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].nTimeout = 1696118400; // Sun Oct 01 2023 00:00:00 GMT+0000

        // Deployment of BIP65.
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].bit = 1;
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].nStartTime = 1667260800; // Tue Nov 01 2022 00:00:00 GMT+0000
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].nTimeout = 1698796800; // Wed Nov 01 2023 00:00:00 GMT+0000

        // Deployment of BIP68, BIP112, and BIP113.
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 1669852800; // Thu Dec 01 2022 00:00:00 GMT+0000
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 1701388800; // Fri Dec 01 2023 00:00:00 GMT+0000

        // Deployment of SegWit (BIP141, BIP143, and BIP147)
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].bit = 3;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nStartTime = 1672531200; // Sun Jan 01 2023 00:00:00 GMT+0000
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nTimeout = 1704067200; // Mon Jan 01 2024 00:00:00 GMT+0000

        // Deployment of Taproot (BIPs 340-342)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 4;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation delay

        consensus.nMinimumChainWork = uint256S("0x0000000000000000000000000000000000000000000000000000000000000000");
        consensus.defaultAssumeValid = uint256S("0x0000000000000000000000000000000000000000000000000000000000000000");

        pchMessageStart[0] = 0xfe;
        pchMessageStart[1] = 0xc3;
        pchMessageStart[2] = 0xb9;
        pchMessageStart[3] = 0xde;
        nDefaultPort = 55444;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 0;

        genesis = CreateGenesisBlock(1642570147, 1642570147, 4021275, 0x1e0ffff0, 1, 10000 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("33a3d9c862e11c8c6b4ca39c9a742556960dbd3b5ccc7154ce2d4e81b3fba56b"));

        vFixedSeeds.clear();
        vSeeds.clear();

        // nodes with support for servicebits filtering should be at the top
        vSeeds.emplace_back("seed-testnet.reddcoin.com");
        vSeeds.emplace_back("dnsseed01-testnet.redd.ink");

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "trdd";

        // Reddcoin BIP44 cointype in testnet is '1'
        nExtCoinType = 1;

        vFixedSeeds.clear();

        fDefaultConsistencyChecks = false;
        fRequireStandard = false;
        m_is_test_chain = true;
        m_is_mockable_chain = false;

        checkpointData = {
            {
                {0, genesis.GetHash()},
            }
        };

        m_assumeutxo_data = MapAssumeutxo{
            // TODO to be specified in a future patch.
        };

        chainTxData = ChainTxData{
            // Data from RPC: getchaintxstats 4096 0000000000004ae2f3896ca8ecd41c460a35bf6184e145d91558cece1c688a76
            /* nTime    */ 1648523138,
            /* nTxCount */ 194412,
            /* dTxRate  */ 0.03533
        };
    }
};

/**
 * Signet: test network with an additional consensus parameter (see BIP325).
 */
class SigNetParams : public CChainParams {
public:
    explicit SigNetParams(const ArgsManager& args) {
        std::vector<uint8_t> bin;
        vSeeds.clear();

        if (!args.IsArgSet("-signetchallenge")) {
            bin = ParseHex("512103ad5e0edad18cb1f0fc0d28a3d4f1f3e445640337489abb10404f2d1e086be430210359ef5021964fe22d6f8e05b2463c9540ce96883fe3b278760f048f5189f2e6c452ae");
            vSeeds.emplace_back("178.128.221.177");
            vSeeds.emplace_back("2a01:7c8:d005:390::5");
            vSeeds.emplace_back("v7ajjeirttkbnt32wpy3c6w3emwnfr3fkla7hpxcfokr3ysd3kqtzmqd.onion:38333");

            consensus.nMinimumChainWork = uint256S("0x0000000000000000000000000000000000000000000000000000008546553c03");
            consensus.defaultAssumeValid = uint256S("0x000000187d4440e5bff91488b700a140441e089a8aaea707414982460edbfe54"); // 47200
            m_assumed_blockchain_size = 1;
            m_assumed_chain_state_size = 0;
            chainTxData = ChainTxData{
                // Data from RPC: getchaintxstats 4096 000000187d4440e5bff91488b700a140441e089a8aaea707414982460edbfe54
                /* nTime    */ 1626696658,
                /* nTxCount */ 387761,
                /* dTxRate  */ 0.04035946932424404,
            };
        } else {
            const auto signet_challenge = args.GetArgs("-signetchallenge");
            if (signet_challenge.size() != 1) {
                throw std::runtime_error(strprintf("%s: -signetchallenge cannot be multiple values.", __func__));
            }
            bin = ParseHex(signet_challenge[0]);

            consensus.nMinimumChainWork = uint256{};
            consensus.defaultAssumeValid = uint256{};
            m_assumed_blockchain_size = 0;
            m_assumed_chain_state_size = 0;
            chainTxData = ChainTxData{
                0,
                0,
                0,
            };
            LogPrintf("Signet with challenge %s\n", signet_challenge[0]);
        }

        if (args.IsArgSet("-signetseednode")) {
            vSeeds = args.GetArgs("-signetseednode");
        }

        strNetworkID = CBaseChainParams::SIGNET;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 210000;
        consensus.nCoinbaseMaturity = 30;
        consensus.BIP16Exception = uint256S("0x00000000000002dc756eebf4f49723ed8d30cc28a5f108eb94b1ba88ac4f9c22");
        consensus.BIP34Height = 227931;
        consensus.BIP34Hash = uint256S("0x000000000000024b89b42a942fe0d9fea3bb44ab7bd1b19115dd6a759c0808b8");
        consensus.BIP65Height = 388381; // 000000000000000004c2b624ed5d7756c508d90fd0da2c7c679febfa6c4735f0
        consensus.BIP66Height = 363725; // 00000000000000000379eaa19dce8c9b722d46ae6a57c2f1a988119488b50931
        consensus.nPowTargetTimespan = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = 10 * 60;
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;
        consensus.nRuleChangeActivationThreshold = 1815; // 90% of 2016
        consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing
        consensus.MinBIP9WarningHeight = 483840; // segwit activation height + miner confirmation window
        consensus.powLimit = uint256S("00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay

        // Deployment of BIP68, BIP112, and BIP113.
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;

        // Deployment of SegWit (BIP141, BIP143, and BIP147)
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].bit = 1;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;

        // Activation of Taproot (BIPs 340-342)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 709632; // Approximately November 12th, 2021

        pchMessageStart[0] = 0xfb;
        pchMessageStart[1] = 0xc0;
        pchMessageStart[2] = 0xb6;
        pchMessageStart[3] = 0xdb;
        nDefaultPort = 45444;
        nPruneAfterHeight = 100000;

        genesis = CreateGenesisBlock(1390280400, 1390280400, 222583475, 0x1e0ffff0, 1, 10000 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("b868e0d95a3c3c0e0dadc67ee587aaf9dc8acbf99e3b4b3110fad4eb74c1decc"));
        assert(genesis.hashMerkleRoot == uint256S("b502bc1dc42b07092b9187e92f70e32f9a53247feae16d821bebffa916af79ff"));

        vFixedSeeds.clear();

        vSeeds.emplace_back("seed.reddcoin.net");
        vSeeds.emplace_back("reddcoin.com");
        vSeeds.emplace_back("dnsseed01.redd.ink");
        vSeeds.emplace_back("dnsseed02.redd.ink");
        vSeeds.emplace_back("dnsseed03.redd.ink");

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,61);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,5);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,189);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        bech32_hrp = "rdd";

        // Reddcoin BIP44 cointype in testnet is '1'
        nExtCoinType = 1;

        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        m_is_test_chain = true;
        m_is_mockable_chain = false;
    }
};

/**
 * Regression test: intended for private networks only. Has minimal difficulty to ensure that
 * blocks can be found instantly.
 */
class CRegTestParams : public CChainParams {
public:
    explicit CRegTestParams(const ArgsManager& args) {
        strNetworkID =  CBaseChainParams::REGTEST;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 210000;
        consensus.nCoinbaseMaturity = 30;
        consensus.BIP16Exception = uint256S("0x00000000000002dc756eebf4f49723ed8d30cc28a5f108eb94b1ba88ac4f9c22");
        consensus.BIP34Height = 227931;
        consensus.BIP34Hash = uint256S("0x000000000000024b89b42a942fe0d9fea3bb44ab7bd1b19115dd6a759c0808b8");
        consensus.BIP65Height = 388381; // 000000000000000004c2b624ed5d7756c508d90fd0da2c7c679febfa6c4735f0
        consensus.BIP66Height = 363725; // 00000000000000000379eaa19dce8c9b722d46ae6a57c2f1a988119488b50931
        consensus.MinBIP9WarningHeight = 483840; // segwit activation height + miner confirmation window
        consensus.powLimit = uint256S("00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowTargetTimespan = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = 10 * 60;
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;
        consensus.nRuleChangeActivationThreshold = 1815; // 90% of 2016
        consensus.nMinerConfirmationWindow = 2016; // nPowTargetTimespan / nPowTargetSpacing

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].bit = 1;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 709632; // Approximately November 12th, 2021

        consensus.nMinimumChainWork = uint256S("0x00000000000000000000000000000000000000001533efd8d716a517fe2c5008");
        consensus.defaultAssumeValid = uint256S("0x0000000000000000000b9d2ec5a352ecba0592946514a92f14319dc2b367fc72"); // 654683

        pchMessageStart[0] = 0xfb;
        pchMessageStart[1] = 0xc0;
        pchMessageStart[2] = 0xb6;
        pchMessageStart[3] = 0xdb;
        nDefaultPort = 45444;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 0;

        genesis = CreateGenesisBlock(1390280400, 1390280400, 222583475, 0x1e0ffff0, 1, 10000 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("b868e0d95a3c3c0e0dadc67ee587aaf9dc8acbf99e3b4b3110fad4eb74c1decc"));
        assert(genesis.hashMerkleRoot == uint256S("b502bc1dc42b07092b9187e92f70e32f9a53247feae16d821bebffa916af79ff"));

        vSeeds.emplace_back("seed.reddcoin.net");
        vSeeds.emplace_back("reddcoin.com");
        vSeeds.emplace_back("dnsseed01.redd.ink");
        vSeeds.emplace_back("dnsseed02.redd.ink");
        vSeeds.emplace_back("dnsseed03.redd.ink");

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,61);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,5);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,189);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        bech32_hrp = "rcrt";

        // Reddcoin BIP44 cointype in testnet is '1'
        nExtCoinType = 1;

        fDefaultConsistencyChecks = true;
        fRequireStandard = true;
        m_is_test_chain = true;
        m_is_mockable_chain = true;

        checkpointData = {
            {
                {0, uint256S("0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206")},
            }
        };

        m_assumeutxo_data = MapAssumeutxo{
            {
                110,
                {AssumeutxoHash{uint256S("0x1ebbf5850204c0bdb15bf030f47c7fe91d45c44c712697e4509ba67adb01c618")}, 110},
            },
            {
                200,
                {AssumeutxoHash{uint256S("0x51c8d11d8b5c1de51543c579736e786aa2736206d1e11e627568029ce092cf62")}, 200},
            },
        };

        chainTxData = ChainTxData{
            0,
            0,
            0
        };


    }

    /**
     * Allows modifying the Version Bits regtest parameters.
     */
    void UpdateVersionBitsParameters(Consensus::DeploymentPos d, int64_t nStartTime, int64_t nTimeout, int min_activation_height)
    {
        consensus.vDeployments[d].nStartTime = nStartTime;
        consensus.vDeployments[d].nTimeout = nTimeout;
        consensus.vDeployments[d].min_activation_height = min_activation_height;
    }
    void UpdateActivationParametersFromArgs(const ArgsManager& args);
};

void CRegTestParams::UpdateActivationParametersFromArgs(const ArgsManager& args)
{
    if (!args.IsArgSet("-vbparams")) return;

    for (const std::string& strDeployment : args.GetArgs("-vbparams")) {
        std::vector<std::string> vDeploymentParams;
        boost::split(vDeploymentParams, strDeployment, boost::is_any_of(":"));
        if (vDeploymentParams.size() < 3 || 4 < vDeploymentParams.size()) {
            throw std::runtime_error("Version bits parameters malformed, expecting deployment:start:end[:min_activation_height]");
        }
        int64_t nStartTime, nTimeout;
        int min_activation_height = 0;
        if (!ParseInt64(vDeploymentParams[1], &nStartTime)) {
            throw std::runtime_error(strprintf("Invalid nStartTime (%s)", vDeploymentParams[1]));
        }
        if (!ParseInt64(vDeploymentParams[2], &nTimeout)) {
            throw std::runtime_error(strprintf("Invalid nTimeout (%s)", vDeploymentParams[2]));
        }
        if (vDeploymentParams.size() >= 4 && !ParseInt32(vDeploymentParams[3], &min_activation_height)) {
            throw std::runtime_error(strprintf("Invalid min_activation_height (%s)", vDeploymentParams[3]));
        }
        bool found = false;
        for (int j=0; j < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++j) {
            if (vDeploymentParams[0] == VersionBitsDeploymentInfo[j].name) {
                UpdateVersionBitsParameters(Consensus::DeploymentPos(j), nStartTime, nTimeout, min_activation_height);
                found = true;
                LogPrintf("Setting version bits activation parameters for %s to start=%ld, timeout=%ld, min_activation_height=%d\n", vDeploymentParams[0], nStartTime, nTimeout, min_activation_height);
                break;
            }
        }
        if (!found) {
            throw std::runtime_error(strprintf("Invalid deployment (%s)", vDeploymentParams[0]));
        }
    }
}

static std::unique_ptr<const CChainParams> globalChainParams;

const CChainParams &Params() {
    assert(globalChainParams);
    return *globalChainParams;
}

std::unique_ptr<const CChainParams> CreateChainParams(const ArgsManager& args, const std::string& chain)
{
    if (chain == CBaseChainParams::MAIN) {
        return std::unique_ptr<CChainParams>(new CMainParams());
    } else if (chain == CBaseChainParams::TESTNET) {
        return std::unique_ptr<CChainParams>(new CTestNetParams());
    } else if (chain == CBaseChainParams::SIGNET) {
        return std::unique_ptr<CChainParams>(new SigNetParams(args));
    } else if (chain == CBaseChainParams::REGTEST) {
        return std::unique_ptr<CChainParams>(new CRegTestParams(args));
    }
    throw std::runtime_error(strprintf("%s: Unknown chain %s.", __func__, chain));
}

void SelectParams(const std::string& network)
{
    SelectBaseParams(network);
    globalChainParams = CreateChainParams(gArgs, network);
}
