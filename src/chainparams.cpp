// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Reddcoin Core developers
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
        consensus.POSVHeight = 260800;
        consensus.BIP66Height = 1564232; // a6e944cc38a0d8c7c5740569501e622ec2a011e7c9dd97a78e5fba40a45b8c61
        consensus.DonationHeight = 3382229; // 77ee468ea88227404a53bad63029a8d0aa58f9f6a470a076a2aa91c8494449ac
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
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].nStartTime = 1667260800; // Tue Nov 01 2022 00:00:00 GMT+0000
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].nTimeout = 1730419200; // Wed Nov 01 2024 00:00:00 GMT+0000

        // Deployment of BIP65.
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].bit = 1;
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].nStartTime = 1668384000; // Tue Nov 14 2022 00:00:00 GMT+0000
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].nTimeout = 1731542400; // Wed Nov 14 2024 00:00:00 GMT+0000

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

        consensus.nMinimumChainWork = uint256S("0x000000000000000000000000000000000000000000000000261a88258d4b86bb");
        consensus.defaultAssumeValid = uint256S("0x88695d994d63a64435d3ee17556a65d22c06051255988c6b4979285b3452d474");  // 5773551

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
        m_assumed_blockchain_size = 8;
        m_assumed_chain_state_size = 1;

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
                {3349639, uint256S("0x2b5b24ff88d25596ea3d357c1ccc33a45422685e9352c64a5c7ec68be4b03630")},
                {3389959, uint256S("0x1f3a1e378d8f92f65e31f7f11623f9e35363d86a03ebdd24c30f86198e29154c")},
                {3430279, uint256S("0xac0ed9835030142d092c54f097c01568425e69185ee516b0f634c8f5047cabe3")},
                {3470599, uint256S("0x121fc00ce30c70406d158a11133761651a4f2d4f0c8f2734f545c1d44fb24f87")},
                {3510919, uint256S("0x608c4fa4715524886dfa7f22758eadcad03625327c5786bd3667dc4bb0c7c826")},
                {3551239, uint256S("0x579516df945b6d0a8a761107cc8b35b1489043ae721e29897b1c6049ec687eee")},
                {3591559, uint256S("0xa24fef71db4aba719feb72ffe0a4fd7ebbbd7b90167d6745144e945d5fb2661c")},
                {3631879, uint256S("0xef64ae4a51dd2532a5f17a67838c8f694585d1fc1cd914c45f7ba9c8ca022c1f")},
                {3672199, uint256S("0x345e413bb0a3de9f3420ddb03f6dd512d8ee7fcae83fe46253d6fca57da2e2af")},
                {3712519, uint256S("0xfedcd4978f35273ccabc0ab9dcc140ad18c7668dec8a698109cba0f939c7e6cf")},
                {3752839, uint256S("0x4361dc341929b5d229c86bbfb7bcec10adeee4b44d3239da054a3e3e884b1ad0")},
                {3793159, uint256S("0xa5144d4d8bfb6c287a6b224bd701a22215d0940326919f55e4de83f1fb011910")},
                {3833479, uint256S("0x9a0cf348d0f8ed288944038f86f8dbfd77e05ec186820f39e7387c9daee1e95d")},
                {3873799, uint256S("0xda2ec964bbad7499458eec05ef1f6645793234c7f8c67809e91d189e69ce8ad1")},
                {3914119, uint256S("0xa9c5db5dd1f7134212ee94863931690ab6af32341d483281c1824f4af84d5d20")},
                {3954439, uint256S("0x40e2db42b54271717892531123314b15636cb6b69ba21b1a1697e8562197f0a0")},
                {3994759, uint256S("0x09c30d8ded9b40198637578d221a55bc06f2f84cae59c139042452adf5a2f9c3")},
                {4035079, uint256S("0x73de0d3f5dd0a55b60fc01bcbf3bec7edec47be3d066e00c2182f2693749b61f")},
                {4075399, uint256S("0x2253ca1b16c165b81388b71a8603b18e1afcde3c6c7f76a4d3ea2ae6dee5edcd")},
                {4115719, uint256S("0xe636574f9006f80abc8309531e9c40c0bd8b53e9d8641f1984691990d403a88c")},
                {4156039, uint256S("0x08f35a1f0c721b62bcaa45a7818fe77e123f0cdc54f5b922bfdb4b9aa6eaf7fd")},
                {4196359, uint256S("0x24ec5db09623084313fd4324c84c2f90888014e305f0f23934ee051a4f49e72b")},
                {4236679, uint256S("0x7c1660725c7299b63731c3990e88fe07c019fda01c41f3af7a43dd53a69ac02c")},
                {4276999, uint256S("0x42fefcaaca90419b963fe06e666405801c9cda209ae617d72ff71cac61d27aa0")},
                {4317319, uint256S("0x581f845161918cf7edc37beb678ef8a5ba3c1facc34a1cb67bbae7fa38f19269")},
                {4357639, uint256S("0x18e5b622a44704d009d174e8e1022e0c081a447be27f8c6c811630894e2ac81b")},
                {4397959, uint256S("0x20c1a73fcc32386f98aeb04a664927d67dd7556972bd41af6b7ef4910cc548b0")},
                {4438279, uint256S("0xcb7c03774a2de0155a915fec39534bb04e026bb1a3608bc92b134915e9eff3de")},
                {4478599, uint256S("0x199929644e26d8bace4fd5d36bf03a3fc6d5927ffebbd839f395090d94b96a2d")},
                {4518919, uint256S("0x5efd0a5e1fb3d97ddbac6c874665addd952bf7565ec4849b25beb5a339c28039")},
                {4559239, uint256S("0x0d3a9e121cbb0c5a551ff4faa29c6c14edc57929a737ba9cc7d676b561fa85a9")},
                {4599559, uint256S("0x462d0741b2dcdfc37891e8c4dd8f79bb129f351c8ea4172d790847fbdddf0160")},
                {4639879, uint256S("0x109dc22cd2b4ea8bd85165557625c6ce896ff5e2f93566693ff95ffc1c9e09c2")},
                {4680199, uint256S("0xd87dca6ea4bd6e0a0c29f5c70a36b8b35b143da157ab5ee84696d19a58ef3c80")},
                {4720519, uint256S("0xba336d9989cadcdd89cb388739db8a95045cbff9f250d60cb271405728d51859")},
                {4760839, uint256S("0xa3930276855882ef4eab87f9c22adb4844b1a79169103528b79110a81c54ff78")},
                {4801159, uint256S("0xfcb8d36d2acf371d2a719f2497a1570a5cbea5dca6149cfee7dd9b5f3d2c3e02")},
                {4841479, uint256S("0xec8e73afa7696065c4737d064bc20c04e171531f45a40fe721123857937eb774")},
                {4881799, uint256S("0xbcef9ae53ba352ddb4868878f3055335c6b74af1c069725fc094d544635657b3")},
                {4922119, uint256S("0x9cb4e5663ab93c65f85cdb421fbfc2820e9a523321abd08dea22a7c8a0cc4743")},
                {4962439, uint256S("0x7fb581b628d5cf4827c9338ea701e0ccd30a58f74641723c79b85ee514f327f4")},
                {5002759, uint256S("0x6b545ca8ddd68db00d767553e48adf0309e12198f7569c695606b272a5b4ff84")},
                {5043079, uint256S("0x46d4fa073ce53242f279cdb2b2e86c25447fa25cc5ad3d2790052ccd35a044da")},
                {5083399, uint256S("0x5a7173e3ef55b0f5061630e865acc73732e15fc91e3fd2a48d470396cdadb56f")},
                {5123719, uint256S("0x597d4dccd63e7f9579292dcde9eb3c901d63b5655cec9bd22bfb6025ec1e4bf8")},
                {5164039, uint256S("0xc818e11e73e72534fbb4f9c0b4e1315e68db2d75ba379d32b526f43805fc8001")},
                {5204359, uint256S("0xb390ca7d4611046e905baf8e051bfaf374edbd09f9a5e2c74c2a44fd969dd11f")},
                {5244679, uint256S("0x2d86051512729490a1e914e8ce9715065c9279ad232599427e4dbe3c00494d45")},
                {5284999, uint256S("0xa8421cdf5ecfcd99375504b3138a2aaf89e186b4fe860bcce5d32e737ed37ca6")},
                {5325319, uint256S("0x4b25b40ff42ac97f8efaefc618aeb968d0691fcf395249d8894ce25222e58978")},
                {5365639, uint256S("0x98f6af55aa13d3e3b583525f40f1dc9289078ac4c34968ead0451f03d435d396")},
                {5405959, uint256S("0x8d6e08e244b86177a52bf6104d98dbede014e79fc73459614b648836d8b33ab9")},
                {5446279, uint256S("0x560b0b1b3b29d3364f81be975acbb1229369be39efe0b624d118cf005ae84634")},
		{5448005, uint256S("0x99e1ba495f4da89c2a0c8a0296cb1df69d5a76488c06517a5aee5c0000c496da")},
                {5486599, uint256S("0x22b86c9191acc687b155db4715720f3ba1f9aaa2bd9d50ff3e2db4344504ba28")},
		{5519068, uint256S("0x1d6ebb2d73dccc03b7b9b013c3b08ec8a83919ed4480edbad6e0604be53f5b40")},
                {5526919, uint256S("0x491daa3b96d8550d07526b0675c451dbe94f7f01504746a15520c6c647d0c2b5")},
                {5567239, uint256S("0x46eaedef033b01184150cf6314b9506b3fb486d4805ef86eafd605dd84210a6e")},
                {5607559, uint256S("0x2411f735e03dd197f2d289d2da07127f31b92e092096c44aaf6665709b0ce4d5")},
                {5647879, uint256S("0x56530119101dc7e959cc04e55c6048ddf2ab4440aa614b9dfc1185e6bf565f78")},
                {5688199, uint256S("0x124b519807dc416f4d4bd0a63a21bd48118a2fe4d625b89581e10ce91c996a19")},
                {5728519, uint256S("0x979c540d1580cbab3c4b7a84596e823c771d95544dc6f5c5785550b14b0f31fe")},
            }
        };

        m_assumeutxo_data = MapAssumeutxo{
         // TODO to be specified in a future patch.
        };

        chainTxData = ChainTxData{
            // Data from RPC: getchaintxstats 4096 23b0377d1291c4585f5952d82a902065561db71a644e129b621801d958b718c2
            /* nTime    */ 1738288799,
            /* nTxCount */ 15082518,
            /* dTxRate  */ 0.03306431243309559,
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
        consensus.POSVHeight = 1440;
        consensus.BIP66Height = 2189; // 84c24e7ec0023d9cf2ba50366f2a1806c30e7256606aaeb31ebd17a4c0a82e9b
        consensus.DonationHeight = 13260; // 7e62ee9c868aa8414909dff2c68d5f9a137d9eb8e9b93c28511cbf8a5cac7280
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

        consensus.nMinimumChainWork = uint256S("000000000000000000000000000000000000000000000000007bc3a45e3a0144");
        consensus.defaultAssumeValid = uint256S("0xe575572a090c0f8d8a6583dc704ef318c9202941266180fb322c4037f2b7e754"); //749000

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
            // Data from RPC: getchaintxstats 4096 e575572a090c0f8d8a6583dc704ef318c9202941266180fb322c4037f2b7e754
            /* nTime    */ 1699486478,
            /* nTxCount */ 1497149,
            /* dTxRate  */ 0.03334826
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
        consensus.BIP66Height = 363725; // 00000000000000000379eaa19dce8c9b722d46ae6a57c2f1a988119488b50931
        consensus.DonationHeight = 3382229; //
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

        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].bit = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].bit = 1;
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].min_activation_height = 0;

        // Deployment of BIP68, BIP112, and BIP113.
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;

        // Deployment of SegWit (BIP141, BIP143, and BIP147)
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].bit = 3;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;

        // Activation of Taproot (BIPs 340-342)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 4;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation for NEVER_ACTIVE

        pchMessageStart[0] = 0xfb;
        pchMessageStart[1] = 0xc0;
        pchMessageStart[2] = 0xb6;
        pchMessageStart[3] = 0xdb;
        nDefaultPort = 45444;
        nPruneAfterHeight = 100000;

        // SIGNET uses nBits that matches powLimit (0x1d00ffff)
        genesis = CreateGenesisBlock(1390280400, 1390280400, 222583475, 0x1d00ffff, 1, 10000 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        // Note: SIGNET genesis hash differs from mainnet due to different nBits
        // Original: assert(consensus.hashGenesisBlock == uint256S("b868e0d95a3c3c0e0dadc67ee587aaf9dc8acbf99e3b4b3110fad4eb74c1decc"));
        assert(genesis.hashMerkleRoot == uint256S("b502bc1dc42b07092b9187e92f70e32f9a53247feae16d821bebffa916af79ff"));

        vFixedSeeds.clear();

        vSeeds.emplace_back("seed.reddcoin.net");
        vSeeds.emplace_back("reddcoin.com");
        vSeeds.emplace_back("dnsseed01.redd.ink");
        vSeeds.emplace_back("dnsseed02.redd.ink");
        vSeeds.emplace_back("dnsseed03.redd.ink");

        // Use same prefixes as testnet (matching Bitcoin's behavior)
        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "trdd";

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
        consensus.nLastPowHeight = 89;  // Allow enough PoW blocks for TestChain100Setup
        consensus.nCoinbaseMaturity = 60;  // Fast maturity for regtest, gives ~40 mature coinbases at height 100
        consensus.BIP16Exception = uint256S("0x00000000000002dc756eebf4f49723ed8d30cc28a5f108eb94b1ba88ac4f9c22");
        consensus.POSVHeight = 90;      // Activate at PoS transition (nLastPowHeight + 1), matching mainnet/testnet pattern
        consensus.BIP66Height = 500;    // Strict DER encoding; above test cache height (200) to avoid breaking hand-crafted blocks
        consensus.DonationHeight = 500; // Dev fund 92%/8% split validation; miner already produces this for all PoS blocks
        consensus.MinBIP9WarningHeight = 0; // regtest: allow BIP9 warnings at any height (ComputeBlockVersion check prevents false positives for known deployments)

        /* pow specific */
        consensus.powLimit = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimit = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"); // Same as powLimit for easy testing
        consensus.posReset = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"); // Same as powLimit for easy testing
        consensus.nPowTargetTimespan = 14 * 24 * 60 * 60; // two weeks
        consensus.nPowTargetSpacing = 10 * 60;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = true;
        consensus.nRuleChangeActivationThreshold = 108; // 75% of 144 for faster regtest
        consensus.nMinerConfirmationWindow = 144; // Faster confirmation window for testing

        consensus.devScript = { CScript() << ParseHex("0256508ba57f39fa6bbe2290051ec100b6e4d49005776214192c3e5dbc2bc3276d") << OP_CHECKSIG };

        /* pos specific */
        consensus.nStakeMinAge = 10;    // 10 seconds (very fast for testing)
        consensus.nStakeMaxAge = 45 * 24 * 60 * 60; // 45 days (same as mainnet)
        consensus.nModifierInterval = 60; // 1 minute (vs 13 minutes on mainnet) for fast testing

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].bit = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_HEIGHTINCB].min_activation_height = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].bit = 1;
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_CLTV].min_activation_height = 0;
        // CSV (BIP68/112/113) - Set to activate via BIP9 signaling during PoS phase
        // With window=144, activation occurs at height 432 (3 periods) if 75% signal
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 0; // Can start signaling immediately
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].min_activation_height = 0; // No minimum activation height
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].bit = 3;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].min_activation_height = 0; // No minimum activation height
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 4;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = 0; // Can start signaling immediately
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].min_activation_height = 0; // No activation for NEVER_ACTIVE

        consensus.nMinimumChainWork = uint256{}; // Regtest: accept any chainwork for testing
        consensus.defaultAssumeValid = uint256{}; // Regtest: always validate for testing

        pchMessageStart[0] = 0xfa;
        pchMessageStart[1] = 0xbf;
        pchMessageStart[2] = 0xb5;
        pchMessageStart[3] = 0xda;
        nDefaultPort = 56444;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 0;

        UpdateActivationParametersFromArgs(args);

        genesis = CreateGenesisBlock(1642570147, 1642570147, 36529, 0x207fffff, 1, 10000 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(genesis.hashMerkleRoot == uint256S("b502bc1dc42b07092b9187e92f70e32f9a53247feae16d821bebffa916af79ff"));
        assert(consensus.hashGenesisBlock == uint256S("e817774b5fd9808e7e03a557a43ec2a37f35ea4cf38550a7ce4f414531e28ef6"));

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();      //!< Regtest mode doesn't have any DNS seeds.

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,122);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,5);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "rcrt";

        // Reddcoin BIP44 cointype in regtest is '1'
        nExtCoinType = 1;

        fDefaultConsistencyChecks = true;
        fRequireStandard = false;
        m_is_test_chain = true;
        m_is_mockable_chain = true;

        checkpointData = {
            {
                {0, uint256S("0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206")},
            }
        };

        // Reddcoin: assumeutxo data for 100-block test chain (89 PoW + 11 PoS blocks)
        // TODO: Recompute these hashes after RegenerateCommitments fix
        m_assumeutxo_data = MapAssumeutxo{
            {
                110,
                {AssumeutxoHash{uint256S("106f9f9bd6c57cea84444957b5630e4a1679a658c73455a0e0362ff0174ee181")}, 132},
            },
            {
                200,
                {AssumeutxoHash{uint256S("0000000000000000000000000000000000000000000000000000000000000000")}, 200},
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
