// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_CRYPTER_H
#define BITCOIN_WALLET_CRYPTER_H

#include <serialize.h>
#include <support/allocators/secure.h>
#include <script/signingprovider.h>
#include <uint256.h>


const unsigned int WALLET_CRYPTO_KEY_SIZE = 32;
const unsigned int WALLET_CRYPTO_SALT_SIZE = 8;
const unsigned int WALLET_CRYPTO_IV_SIZE = 16;

/**
 * Private key encryption is done based on a CMasterKey,
 * which holds a salt and random encryption key.
 *
 * CMasterKeys are encrypted using AES-256-CBC using a key
 * derived using derivation method nDerivationMethod
 * (0 == EVP_sha512()) and derivation iterations nDeriveIterations.
 * vchOtherDerivationParameters is provided for alternative algorithms
 * which may require more parameters (such as scrypt).
 *
 * Wallet Private Keys are then encrypted using AES-256-CBC
 * with the double-sha256 of the public key as the IV, and the
 * master key's key as the encryption key (see keystore.[ch]).
 */

/** Master key for wallet encryption */
class CMasterKey
{
public:
    std::vector<unsigned char> vchCryptedKey;
    std::vector<unsigned char> vchSalt;
    //! 0 = EVP_sha512()
    //! 1 = scrypt()
    unsigned int nDerivationMethod;
    unsigned int nDeriveIterations;
    //! Use this for more parameters to key derivation,
    //! such as the various parameters to scrypt
    std::vector<unsigned char> vchOtherDerivationParameters;

    SERIALIZE_METHODS(CMasterKey, obj)
    {
        READWRITE(obj.vchCryptedKey, obj.vchSalt, obj.nDerivationMethod, obj.nDeriveIterations, obj.vchOtherDerivationParameters);
    }

    CMasterKey()
    {
        // 25000 rounds is just under 0.1 seconds on a 1.86 GHz Pentium M
        // ie slightly lower than the lowest hardware we need bother supporting
        nDeriveIterations = 25000;
        nDerivationMethod = 0;
        vchOtherDerivationParameters = std::vector<unsigned char>(0);
    }
};

typedef std::vector<unsigned char, secure_allocator<unsigned char> > CKeyingMaterial;

namespace wallet_crypto_tests
{
    class TestCrypter;
}

/** Encryption/decryption context with key information */
class CCrypter
{
friend class wallet_crypto_tests::TestCrypter; // for test access to chKey/chIV
private:
    std::vector<unsigned char, secure_allocator<unsigned char>> vchKey;
    std::vector<unsigned char, secure_allocator<unsigned char>> vchIV;
    bool fKeySet;

    int BytesToKeySHA512AES(const std::vector<unsigned char>& chSalt, const SecureString& strKeyData, int count, unsigned char *key,unsigned char *iv) const;

public:
    bool SetKeyFromPassphrase(const SecureString &strKeyData, const std::vector<unsigned char>& chSalt, const unsigned int nRounds, const unsigned int nDerivationMethod);
    bool Encrypt(const CKeyingMaterial& vchPlaintext, std::vector<unsigned char> &vchCiphertext) const;
    bool Decrypt(const std::vector<unsigned char>& vchCiphertext, CKeyingMaterial& vchPlaintext) const;
    bool SetKey(const CKeyingMaterial& chNewKey, const std::vector<unsigned char>& chNewIV);

    void CleanKey()
    {
        memory_cleanse(vchKey.data(), vchKey.size());
        memory_cleanse(vchIV.data(), vchIV.size());
        fKeySet = false;
    }

    CCrypter()
    {
        fKeySet = false;
        vchKey.resize(WALLET_CRYPTO_KEY_SIZE);
        vchIV.resize(WALLET_CRYPTO_IV_SIZE);
    }

    ~CCrypter()
    {
        CleanKey();
    }
};

bool EncryptSecret(const CKeyingMaterial& vMasterKey, const CKeyingMaterial &vchPlaintext, const uint256& nIV, std::vector<unsigned char> &vchCiphertext);
bool DecryptSecret(const CKeyingMaterial& vMasterKey, const std::vector<unsigned char>& vchCiphertext, const uint256& nIV, CKeyingMaterial& vchPlaintext);
bool DecryptKey(const CKeyingMaterial& vMasterKey, const std::vector<unsigned char>& vchCryptedSecret, const CPubKey& vchPubKey, CKey& key);

/** The secrets of an HD chain that holds its own seed: a chain made from a
 *  mnemonic, or from a seed given outright. */
struct CHDSecrets {
    SecureVector vchMnemonic;           //!< may be empty
    SecureVector vchMnemonicPassphrase; //!< may be empty
    SecureVector vchSeed;               //!< never empty

    bool IsNull() const { return vchSeed.empty(); }
    void SetNull();
    friend bool operator==(const CHDSecrets& a, const CHDSecrets& b)
    {
        return a.vchMnemonic == b.vchMnemonic && a.vchMnemonicPassphrase == b.vchMnemonicPassphrase && a.vchSeed == b.vchSeed;
    }
};

/** The "hdsecrets" record: the secrets of the wallet's chain, encrypted with
 *  the wallet's master key. */
class CHDSecretsRecord
{
public:
    static const uint32_t CURRENT_VERSION = 1;
    uint32_t nVersion{CURRENT_VERSION};
    //! Drawn anew for every write of the record. Encryption uses its first 16 bytes.
    uint256 nIV;
    std::vector<unsigned char> vchCrypted;

    SERIALIZE_METHODS(CHDSecretsRecord, obj) { READWRITE(obj.nVersion, obj.nIV, obj.vchCrypted); }
};

/** Encrypt the secrets with the master key and the given IV. What is encrypted
 *  is one byte string: each of the three as a compact size and its bytes, then
 *  the double SHA-256 of all of that. The result is decrypted again and
 *  compared before it is returned. False if the seed is empty or anything fails. */
bool EncryptHDSecrets(const CKeyingMaterial& vMasterKey, const CHDSecrets& secrets, const uint256& nIV, CHDSecretsRecord& record);
/** The same with a new random IV. */
bool EncryptHDSecrets(const CKeyingMaterial& vMasterKey, const CHDSecrets& secrets, CHDSecretsRecord& record);
/** Decrypt a record. True only if its version is known, it decrypts, it parses
 *  with no byte left over, its check matches and its seed is not empty. */
bool DecryptHDSecrets(const CKeyingMaterial& vMasterKey, const CHDSecretsRecord& record, CHDSecrets& secrets);

#endif // BITCOIN_WALLET_CRYPTER_H
