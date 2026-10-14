// Copyright (c) 2009-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/crypter.h>

#include <crypto/aes.h>
#include <crypto/sha512.h>
#include <hash.h>
#include <random.h>
#include <util/system.h>

#include <vector>

int CCrypter::BytesToKeySHA512AES(const std::vector<unsigned char>& chSalt, const SecureString& strKeyData, int count, unsigned char *key,unsigned char *iv) const
{
    // This mimics the behavior of openssl's EVP_BytesToKey with an aes256cbc
    // cipher and sha512 message digest. Because sha512's output size (64b) is
    // greater than the aes256 block size (16b) + aes256 key size (32b),
    // there's no need to process more than once (D_0).

    if(!count || !key || !iv)
        return 0;

    unsigned char buf[CSHA512::OUTPUT_SIZE];
    CSHA512 di;

    di.Write((const unsigned char*)strKeyData.data(), strKeyData.size());
    di.Write(chSalt.data(), chSalt.size());
    di.Finalize(buf);

    for(int i = 0; i != count - 1; i++)
        di.Reset().Write(buf, sizeof(buf)).Finalize(buf);

    memcpy(key, buf, WALLET_CRYPTO_KEY_SIZE);
    memcpy(iv, buf + WALLET_CRYPTO_KEY_SIZE, WALLET_CRYPTO_IV_SIZE);
    memory_cleanse(buf, sizeof(buf));
    return WALLET_CRYPTO_KEY_SIZE;
}

bool CCrypter::SetKeyFromPassphrase(const SecureString& strKeyData, const std::vector<unsigned char>& chSalt, const unsigned int nRounds, const unsigned int nDerivationMethod)
{
    if (nRounds < 1 || chSalt.size() != WALLET_CRYPTO_SALT_SIZE)
        return false;

    int i = 0;
    if (nDerivationMethod == 0)
        i = BytesToKeySHA512AES(chSalt, strKeyData, nRounds, vchKey.data(), vchIV.data());

    if (i != (int)WALLET_CRYPTO_KEY_SIZE)
    {
        memory_cleanse(vchKey.data(), vchKey.size());
        memory_cleanse(vchIV.data(), vchIV.size());
        return false;
    }

    fKeySet = true;
    return true;
}

bool CCrypter::SetKey(const CKeyingMaterial& chNewKey, const std::vector<unsigned char>& chNewIV)
{
    if (chNewKey.size() != WALLET_CRYPTO_KEY_SIZE || chNewIV.size() != WALLET_CRYPTO_IV_SIZE)
        return false;

    memcpy(vchKey.data(), chNewKey.data(), chNewKey.size());
    memcpy(vchIV.data(), chNewIV.data(), chNewIV.size());

    fKeySet = true;
    return true;
}

bool CCrypter::Encrypt(const CKeyingMaterial& vchPlaintext, std::vector<unsigned char> &vchCiphertext) const
{
    if (!fKeySet)
        return false;

    // max ciphertext len for a n bytes of plaintext is
    // n + AES_BLOCKSIZE bytes
    vchCiphertext.resize(vchPlaintext.size() + AES_BLOCKSIZE);

    AES256CBCEncrypt enc(vchKey.data(), vchIV.data(), true);
    size_t nLen = enc.Encrypt(vchPlaintext.data(), vchPlaintext.size(), vchCiphertext.data());
    if(nLen < vchPlaintext.size())
        return false;
    vchCiphertext.resize(nLen);

    return true;
}

bool CCrypter::Decrypt(const std::vector<unsigned char>& vchCiphertext, CKeyingMaterial& vchPlaintext) const
{
    if (!fKeySet)
        return false;

    // plaintext will always be equal to or lesser than length of ciphertext
    int nLen = vchCiphertext.size();

    vchPlaintext.resize(nLen);

    AES256CBCDecrypt dec(vchKey.data(), vchIV.data(), true);
    nLen = dec.Decrypt(vchCiphertext.data(), vchCiphertext.size(), vchPlaintext.data());
    if(nLen == 0)
        return false;
    vchPlaintext.resize(nLen);
    return true;
}

bool EncryptSecret(const CKeyingMaterial& vMasterKey, const CKeyingMaterial &vchPlaintext, const uint256& nIV, std::vector<unsigned char> &vchCiphertext)
{
    CCrypter cKeyCrypter;
    std::vector<unsigned char> chIV(WALLET_CRYPTO_IV_SIZE);
    memcpy(chIV.data(), &nIV, WALLET_CRYPTO_IV_SIZE);
    if(!cKeyCrypter.SetKey(vMasterKey, chIV))
        return false;
    return cKeyCrypter.Encrypt(*((const CKeyingMaterial*)&vchPlaintext), vchCiphertext);
}

bool DecryptSecret(const CKeyingMaterial& vMasterKey, const std::vector<unsigned char>& vchCiphertext, const uint256& nIV, CKeyingMaterial& vchPlaintext)
{
    CCrypter cKeyCrypter;
    std::vector<unsigned char> chIV(WALLET_CRYPTO_IV_SIZE);
    memcpy(chIV.data(), &nIV, WALLET_CRYPTO_IV_SIZE);
    if(!cKeyCrypter.SetKey(vMasterKey, chIV))
        return false;
    return cKeyCrypter.Decrypt(vchCiphertext, vchPlaintext);
}

bool DecryptKey(const CKeyingMaterial& vMasterKey, const std::vector<unsigned char>& vchCryptedSecret, const CPubKey& vchPubKey, CKey& key)
{
    CKeyingMaterial vchSecret;
    if(!DecryptSecret(vMasterKey, vchCryptedSecret, vchPubKey.GetHash(), vchSecret))
        return false;

    if (vchSecret.size() != 32)
        return false;

    key.Set(vchSecret.begin(), vchSecret.end(), vchPubKey.IsCompressed());
    return key.VerifyPubKey(vchPubKey);
}

void CHDSecrets::SetNull()
{
    // Released, not merely emptied: the allocator cleanses what it releases.
    SecureVector().swap(vchMnemonic);
    SecureVector().swap(vchMnemonicPassphrase);
    SecureVector().swap(vchSeed);
}

namespace {
void AppendCompactSize(CKeyingMaterial& out, uint64_t size)
{
    if (size < 253) {
        out.push_back(static_cast<unsigned char>(size));
    } else if (size <= 0xffff) {
        out.push_back(253);
        out.push_back(size & 0xff);
        out.push_back((size >> 8) & 0xff);
    } else {
        out.push_back(254);
        for (int shift = 0; shift < 32; shift += 8) out.push_back((size >> shift) & 0xff);
    }
}

void AppendPart(CKeyingMaterial& out, const SecureVector& part)
{
    AppendCompactSize(out, part.size());
    out.insert(out.end(), part.begin(), part.end());
}

//! Read a compact size and that many bytes at pos. Only the encodings
//! AppendCompactSize writes are accepted.
bool ReadPart(const CKeyingMaterial& in, size_t& pos, SecureVector& part)
{
    if (pos >= in.size()) return false;
    uint64_t size = in[pos++];
    if (size == 253) {
        if (in.size() - pos < 2) return false;
        size = in[pos] | (uint64_t{in[pos + 1]} << 8);
        pos += 2;
        if (size < 253) return false;
    } else if (size == 254) {
        if (in.size() - pos < 4) return false;
        size = 0;
        for (int i = 0; i < 4; ++i) size |= uint64_t{in[pos + i]} << (8 * i);
        pos += 4;
        if (size <= 0xffff) return false;
    } else if (size == 255) {
        return false;
    }
    if (in.size() - pos < size) return false;
    part.assign(in.begin() + pos, in.begin() + pos + size);
    pos += size;
    return true;
}
} // namespace

bool EncryptHDSecrets(const CKeyingMaterial& vMasterKey, const CHDSecrets& secrets, const uint256& nIV, CHDSecretsRecord& record)
{
    if (secrets.IsNull() || secrets.vchMnemonic.size() > 0xffffffff || secrets.vchMnemonicPassphrase.size() > 0xffffffff || secrets.vchSeed.size() > 0xffffffff) return false;

    CKeyingMaterial plaintext;
    plaintext.reserve(secrets.vchMnemonic.size() + secrets.vchMnemonicPassphrase.size() + secrets.vchSeed.size() + 15 + uint256::size());
    AppendPart(plaintext, secrets.vchMnemonic);
    AppendPart(plaintext, secrets.vchMnemonicPassphrase);
    AppendPart(plaintext, secrets.vchSeed);
    const uint256 check = Hash(plaintext);
    plaintext.insert(plaintext.end(), check.begin(), check.end());

    CHDSecretsRecord result;
    result.nIV = nIV;
    if (!EncryptSecret(vMasterKey, plaintext, result.nIV, result.vchCrypted)) return false;

    // Nothing is written that has not been read back.
    CHDSecrets read_back;
    if (!DecryptHDSecrets(vMasterKey, result, read_back) || !(read_back == secrets)) return false;

    record = std::move(result);
    return true;
}

bool EncryptHDSecrets(const CKeyingMaterial& vMasterKey, const CHDSecrets& secrets, CHDSecretsRecord& record)
{
    uint256 iv;
    GetStrongRandBytes(iv.begin(), iv.size());
    return EncryptHDSecrets(vMasterKey, secrets, iv, record);
}

bool DecryptHDSecrets(const CKeyingMaterial& vMasterKey, const CHDSecretsRecord& record, CHDSecrets& secrets)
{
    if (record.nVersion != CHDSecretsRecord::CURRENT_VERSION) return false;

    CKeyingMaterial plaintext;
    if (!DecryptSecret(vMasterKey, record.vchCrypted, record.nIV, plaintext)) return false;

    CHDSecrets result;
    size_t pos = 0;
    if (!ReadPart(plaintext, pos, result.vchMnemonic) ||
        !ReadPart(plaintext, pos, result.vchMnemonicPassphrase) ||
        !ReadPart(plaintext, pos, result.vchSeed)) return false;
    if (plaintext.size() - pos != uint256::size()) return false;
    const uint256 check = Hash(Span<const unsigned char>{plaintext.data(), pos});
    if (!std::equal(check.begin(), check.end(), plaintext.begin() + pos)) return false;
    if (result.IsNull()) return false;

    secrets = std::move(result);
    return true;
}
