// Copyright (c) 2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <clientversion.h>
#include <hash.h>
#include <streams.h>
#include <support/allocators/secure.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <wallet/crypter.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <wallet/walletutil.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

namespace {
// The vectors were made outside this code: the plaintext put together by hand
// and encrypted with OpenSSL (AES-256-CBC, key = the master key, IV = the
// first 16 bytes of the record's IV).
const std::string PHRASE{"abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"};
const std::string PHRASE_PASSPHRASE{"a second factor for the mnemonic 0451"};
const std::string RECORD_HEX{
    "01000000a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebff02f2174ef084eee81f414fb18f952d9ac0f842e386bf936a659ceabd051c9bb5eea0be7a1913f3a5d485a512ac917d8d93ef25f25e53f34de8d442e4da60bca86941ba28d9553c122c4e9a34f1367d20ad4cdab58274c1d251099cef7a1d7f1c4e788061ef35e24a711163c238bdd42f0b67939a422ab06ef8b07a7688bc88801e41bd74d93f300db4d193dbcd6d006fa578bb19ec5c7d21753ee8e5a1d89b72891460ddb44da301c866da1090153df1eb2d8af159f7d129f7a2378cd10529b2c27c82a31578762cb7b168a3ce9239c737cbb72720687134e1cab3d1f2c9620c61954c531b7f459e525a46ae097c14198"};
// A 32-byte seed with no phrase and no passphrase.
const std::string RECORD_SEED_ONLY_HEX{
    "01000000a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf50cd771fd7ed35b1553f37a8b0c0577fc32a61d65a7eb63fbc8d2c63f3cbd89bb5b60c0a583bf51bb6f6cc824da8b0f2e24c267dfa8e504678721097d1d4962f0b6ebed8e14f48990444821dfca27f7315"};

CKeyingMaterial MasterKey()
{
    CKeyingMaterial key(32);
    for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<unsigned char>(i);
    return key;
}

uint256 FixedIV()
{
    uint256 iv;
    for (size_t i = 0; i < iv.size(); ++i) iv.begin()[i] = static_cast<unsigned char>(0xa0 + i);
    return iv;
}

SecureVector Bytes(unsigned char first, size_t count)
{
    SecureVector bytes(count);
    for (size_t i = 0; i < count; ++i) bytes[i] = static_cast<unsigned char>(first + i);
    return bytes;
}

CHDSecrets Secrets()
{
    CHDSecrets secrets;
    secrets.vchMnemonic.assign(PHRASE.begin(), PHRASE.end());
    secrets.vchMnemonicPassphrase.assign(PHRASE_PASSPHRASE.begin(), PHRASE_PASSPHRASE.end());
    secrets.vchSeed = Bytes(0x40, 64);
    return secrets;
}

std::string Serialized(const CHDSecretsRecord& record)
{
    CDataStream stream(SER_DISK, CLIENT_VERSION);
    stream << record;
    return HexStr(stream);
}

CHDSecretsRecord Parsed(const std::string& hex)
{
    CDataStream stream(ParseHex(hex), SER_DISK, CLIENT_VERSION);
    CHDSecretsRecord record;
    stream >> record;
    BOOST_CHECK(stream.empty());
    return record;
}

//! A record whose plaintext is the given bytes, encrypted as a record is.
CHDSecretsRecord RecordOf(const CKeyingMaterial& plaintext)
{
    CHDSecretsRecord record;
    record.nIV = FixedIV();
    BOOST_REQUIRE(EncryptSecret(MasterKey(), plaintext, record.nIV, record.vchCrypted));
    return record;
}

CKeyingMaterial WithCheck(CKeyingMaterial body)
{
    const uint256 check = Hash(body);
    body.insert(body.end(), check.begin(), check.end());
    return body;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(hdsecrets_tests, BasicTestingSetup)

// The bytes of the record are fixed: both release lines must write and read these.
BOOST_AUTO_TEST_CASE(record_bytes)
{
    CHDSecretsRecord record;
    BOOST_REQUIRE(EncryptHDSecrets(MasterKey(), Secrets(), FixedIV(), record));
    BOOST_CHECK_EQUAL(record.nVersion, 1U);
    BOOST_CHECK(record.nIV == FixedIV());
    BOOST_CHECK_EQUAL(Serialized(record), RECORD_HEX);

    CHDSecrets read;
    BOOST_REQUIRE(DecryptHDSecrets(MasterKey(), Parsed(RECORD_HEX), read));
    BOOST_CHECK(read == Secrets());

    // A write that is not given an IV draws one, a new one each time.
    CHDSecretsRecord first, second;
    BOOST_REQUIRE(EncryptHDSecrets(MasterKey(), Secrets(), first));
    BOOST_REQUIRE(EncryptHDSecrets(MasterKey(), Secrets(), second));
    BOOST_CHECK(first.nIV != second.nIV);
    BOOST_CHECK(first.nIV != uint256());
    BOOST_CHECK(first.vchCrypted != second.vchCrypted);
    BOOST_REQUIRE(DecryptHDSecrets(MasterKey(), first, read));
    BOOST_CHECK(read == Secrets());
    BOOST_REQUIRE(DecryptHDSecrets(MasterKey(), second, read));
    BOOST_CHECK(read == Secrets());
}

// A seed that was given outright has no phrase and need not be 64 bytes.
BOOST_AUTO_TEST_CASE(seed_without_phrase)
{
    CHDSecrets secrets;
    secrets.vchSeed = Bytes(0x10, 32);
    CHDSecretsRecord record;
    BOOST_REQUIRE(EncryptHDSecrets(MasterKey(), secrets, FixedIV(), record));
    BOOST_CHECK_EQUAL(Serialized(record), RECORD_SEED_ONLY_HEX);
    CHDSecrets read;
    BOOST_REQUIRE(DecryptHDSecrets(MasterKey(), Parsed(RECORD_SEED_ONLY_HEX), read));
    BOOST_CHECK(read == secrets);
    BOOST_CHECK(read.vchMnemonic.empty() && read.vchMnemonicPassphrase.empty());

    // Longer than fits a one-byte size: a phrase of 300 bytes.
    secrets.vchMnemonic = SecureVector(300, 'w');
    BOOST_REQUIRE(EncryptHDSecrets(MasterKey(), secrets, record));
    BOOST_REQUIRE(DecryptHDSecrets(MasterKey(), record, read));
    BOOST_CHECK(read == secrets);

    // Nothing is encrypted for an empty seed.
    BOOST_CHECK(!EncryptHDSecrets(MasterKey(), CHDSecrets{}, record));
}

// Each rule for reading, broken in turn. The record is not readable, nothing
// is thrown, and what was passed in to receive the secrets is left alone.
BOOST_AUTO_TEST_CASE(unreadable_records)
{
    const CHDSecretsRecord good = Parsed(RECORD_HEX);
    CHDSecrets read;
    read.vchSeed = Bytes(1, 3);
    const CHDSecrets untouched = read;

    // Another version.
    CHDSecretsRecord record = good;
    record.nVersion = 2;
    BOOST_CHECK(!DecryptHDSecrets(MasterKey(), record, read));
    record.nVersion = 0;
    BOOST_CHECK(!DecryptHDSecrets(MasterKey(), record, read));

    // Does not decrypt, or decrypts to something else: a changed ciphertext,
    // a cut one, an empty one, another IV, another key.
    record = good;
    record.vchCrypted[5] ^= 1;
    BOOST_CHECK(!DecryptHDSecrets(MasterKey(), record, read));
    record = good;
    record.vchCrypted.back() ^= 1;
    BOOST_CHECK(!DecryptHDSecrets(MasterKey(), record, read));
    record = good;
    record.vchCrypted.resize(record.vchCrypted.size() - 16);
    BOOST_CHECK(!DecryptHDSecrets(MasterKey(), record, read));
    record.vchCrypted.clear();
    BOOST_CHECK(!DecryptHDSecrets(MasterKey(), record, read));
    record = good;
    record.nIV.begin()[0] ^= 1;
    BOOST_CHECK(!DecryptHDSecrets(MasterKey(), record, read));
    CKeyingMaterial other_key = MasterKey();
    other_key[31] ^= 1;
    BOOST_CHECK(!DecryptHDSecrets(other_key, good, read));

    // The three parts, as they are written.
    CKeyingMaterial body;
    body.push_back(0);
    body.push_back(0);
    body.push_back(4);
    body.insert(body.end(), {1, 2, 3, 4});
    BOOST_CHECK(DecryptHDSecrets(MasterKey(), RecordOf(WithCheck(body)), read));
    read = untouched;

    // A byte left over after the check.
    CKeyingMaterial trailing = WithCheck(body);
    trailing.push_back(0);
    BOOST_CHECK(!DecryptHDSecrets(MasterKey(), RecordOf(trailing), read));
    // A byte between the seed and the check, covered by the check.
    CKeyingMaterial extra = body;
    extra.push_back(0);
    BOOST_CHECK(!DecryptHDSecrets(MasterKey(), RecordOf(WithCheck(extra)), read));

    // A check that does not match.
    CKeyingMaterial bad_check = WithCheck(body);
    bad_check.back() ^= 1;
    BOOST_CHECK(!DecryptHDSecrets(MasterKey(), RecordOf(bad_check), read));
    // No check at all, and a part that runs past the end.
    BOOST_CHECK(!DecryptHDSecrets(MasterKey(), RecordOf(body), read));
    CKeyingMaterial overrun;
    overrun.push_back(0);
    overrun.push_back(0);
    overrun.push_back(200);
    BOOST_CHECK(!DecryptHDSecrets(MasterKey(), RecordOf(WithCheck(overrun)), read));

    // An empty seed.
    CKeyingMaterial no_seed;
    no_seed.push_back(3);
    no_seed.insert(no_seed.end(), {'a', 'b', 'c'});
    no_seed.push_back(0);
    no_seed.push_back(0);
    BOOST_CHECK(!DecryptHDSecrets(MasterKey(), RecordOf(WithCheck(no_seed)), read));

    // A size written the long way round when it fits one byte.
    CKeyingMaterial long_size;
    long_size.push_back(0);
    long_size.push_back(0);
    long_size.insert(long_size.end(), {253, 4, 0});
    long_size.insert(long_size.end(), {1, 2, 3, 4});
    BOOST_CHECK(!DecryptHDSecrets(MasterKey(), RecordOf(WithCheck(long_size)), read));

    BOOST_CHECK(read == untouched);
}

// The flag that says the secrets are in their own record is a known one, and
// mandatory; so is the one beside it that says the wallet holds HD accounts.
// A number past them is not known: a wallet carrying it is refused.
BOOST_AUTO_TEST_CASE(wallet_flag)
{
    BOOST_CHECK_EQUAL(uint64_t{WALLET_FLAG_HD_SECRETS_ENCRYPTED}, uint64_t{1} << 49);
    BOOST_CHECK_EQUAL(uint64_t{WALLET_FLAG_HD_ACCOUNTS}, uint64_t{1} << 48);
    BOOST_CHECK(KNOWN_WALLET_FLAGS & WALLET_FLAG_HD_SECRETS_ENCRYPTED);
    BOOST_CHECK(KNOWN_WALLET_FLAGS & WALLET_FLAG_HD_ACCOUNTS);
    BOOST_CHECK(!(MUTABLE_WALLET_FLAGS & WALLET_FLAG_HD_SECRETS_ENCRYPTED));
    BOOST_CHECK(!(MUTABLE_WALLET_FLAGS & WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK_EQUAL(uint64_t{WALLET_FLAG_MAP.at("hd_secrets_encrypted")}, uint64_t{WALLET_FLAG_HD_SECRETS_ENCRYPTED});

    CWallet wallet(nullptr, "", CreateMockWalletDatabase());
    BOOST_CHECK(wallet.LoadWalletFlags(WALLET_FLAG_HD_SECRETS_ENCRYPTED));
    BOOST_CHECK(wallet.IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED));
    BOOST_CHECK(wallet.LoadWalletFlags(WALLET_FLAG_HD_SECRETS_ENCRYPTED | WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK(wallet.IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS));
    BOOST_CHECK(!wallet.LoadWalletFlags(uint64_t{1} << 50));

    // Written through a batch, the flag is in memory only once it is confirmed.
    // This is how a key manager sets it, through what it sees of the wallet.
    BOOST_CHECK(wallet.LoadWalletFlags(0));
    WalletStorage& storage = wallet;
    {
        WalletBatch batch(wallet.GetDatabase());
        BOOST_CHECK(storage.SetWalletFlagWithDB(batch, WALLET_FLAG_HD_SECRETS_ENCRYPTED));
    }
    BOOST_CHECK(!wallet.IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED));
    storage.ConfirmWalletFlag(WALLET_FLAG_HD_SECRETS_ENCRYPTED);
    BOOST_CHECK(wallet.IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED));
}

BOOST_AUTO_TEST_SUITE_END()
