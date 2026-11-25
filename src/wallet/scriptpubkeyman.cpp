// Copyright (c) 2019-2020 The Bitcoin Core developers
// Copyright (c) 2019-2026 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <key_io.h>
#include <logging.h>
#include <outputtype.h>
#include <script/descriptor.h>
#include <script/sign.h>
#include <util/bip32.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/system.h>
#include <util/time.h>
#include <util/translation.h>
#include <wallet/scriptpubkeyman.h>

#include <optional>

//! Value for the first BIP 32 hardened derivation. Can be used as a bit mask and as a value. See BIP 32 for more details.
const uint32_t BIP32_HARDENED_KEY_LIMIT = 0x80000000;

bool LegacyScriptPubKeyMan::GetNewDestination(const OutputType type, CTxDestination& dest, std::string& error)
{
    if (LEGACY_OUTPUT_TYPES.count(type) == 0) {
        error = _("Error: Legacy wallets only support the \"legacy\", \"p2sh-segwit\", and \"bech32\" address types").translated;
        return false;
    }
    assert(type != OutputType::BECH32M);

    LOCK(cs_KeyStore);
    error.clear();

    // Generate a new key that is added to wallet
    CPubKey new_key;
    if (!GetKeyFromPool(new_key, type)) {
        error = _("Error: Keypool ran out, please call keypoolrefill first").translated;
        return false;
    }
    LearnRelatedScripts(new_key, type);
    dest = GetDestinationForKey(new_key, type);
    return true;
}

typedef std::vector<unsigned char> valtype;

namespace {

/**
 * This is an enum that tracks the execution context of a script, similar to
 * SigVersion in script/interpreter. It is separate however because we want to
 * distinguish between top-level scriptPubKey execution and P2SH redeemScript
 * execution (a distinction that has no impact on consensus rules).
 */
enum class IsMineSigVersion
{
    TOP = 0,        //!< scriptPubKey execution
    P2SH = 1,       //!< P2SH redeemScript
    WITNESS_V0 = 2, //!< P2WSH witness script execution
};

/**
 * This is an internal representation of isminetype + invalidity.
 * Its order is significant, as we return the max of all explored
 * possibilities.
 */
enum class IsMineResult
{
    NO = 0,         //!< Not ours
    WATCH_ONLY = 1, //!< Included in watch-only balance
    SPENDABLE = 2,  //!< Included in all balances
    INVALID = 3,    //!< Not spendable by anyone (uncompressed pubkey in segwit, P2SH inside P2SH or witness, witness inside witness)
};

bool PermitsUncompressed(IsMineSigVersion sigversion)
{
    return sigversion == IsMineSigVersion::TOP || sigversion == IsMineSigVersion::P2SH;
}

bool HaveKeys(const std::vector<valtype>& pubkeys, const LegacyScriptPubKeyMan& keystore)
{
    for (const valtype& pubkey : pubkeys) {
        CKeyID keyID = CPubKey(pubkey).GetID();
        if (!keystore.HaveKey(keyID)) return false;
    }
    return true;
}

//! Recursively solve script and return spendable/watchonly/invalid status.
//!
//! @param keystore            legacy key and script store
//! @param scriptPubKey        script to solve
//! @param sigversion          script type (top-level / redeemscript / witnessscript)
//! @param recurse_scripthash  whether to recurse into nested p2sh and p2wsh
//!                            scripts or simply treat any script that has been
//!                            stored in the keystore as spendable
IsMineResult IsMineInner(const LegacyScriptPubKeyMan& keystore, const CScript& scriptPubKey, IsMineSigVersion sigversion, bool recurse_scripthash=true)
{
    IsMineResult ret = IsMineResult::NO;

    std::vector<valtype> vSolutions;
    TxoutType whichType = Solver(scriptPubKey, vSolutions);

    CKeyID keyID;
    switch (whichType) {
    case TxoutType::NONSTANDARD:
    case TxoutType::NULL_DATA:
    case TxoutType::WITNESS_UNKNOWN:
    case TxoutType::WITNESS_V1_TAPROOT:
        break;
    case TxoutType::PUBKEY:
        keyID = CPubKey(vSolutions[0]).GetID();
        if (!PermitsUncompressed(sigversion) && vSolutions[0].size() != 33) {
            return IsMineResult::INVALID;
        }
        if (keystore.HaveKey(keyID)) {
            ret = std::max(ret, IsMineResult::SPENDABLE);
        }
        break;
    case TxoutType::WITNESS_V0_KEYHASH:
    {
        if (sigversion == IsMineSigVersion::WITNESS_V0) {
            // P2WPKH inside P2WSH is invalid.
            return IsMineResult::INVALID;
        }
        if (sigversion == IsMineSigVersion::TOP && !keystore.HaveCScript(CScriptID(CScript() << OP_0 << vSolutions[0]))) {
            // We do not support bare witness outputs unless the P2SH version of it would be
            // acceptable as well. This protects against matching before segwit activates.
            // This also applies to the P2WSH case.
            break;
        }
        ret = std::max(ret, IsMineInner(keystore, GetScriptForDestination(PKHash(uint160(vSolutions[0]))), IsMineSigVersion::WITNESS_V0));
        break;
    }
    case TxoutType::PUBKEYHASH:
        keyID = CKeyID(uint160(vSolutions[0]));
        if (!PermitsUncompressed(sigversion)) {
            CPubKey pubkey;
            if (keystore.GetPubKey(keyID, pubkey) && !pubkey.IsCompressed()) {
                return IsMineResult::INVALID;
            }
        }
        if (keystore.HaveKey(keyID)) {
            ret = std::max(ret, IsMineResult::SPENDABLE);
        }
        break;
    case TxoutType::SCRIPTHASH:
    {
        if (sigversion != IsMineSigVersion::TOP) {
            // P2SH inside P2WSH or P2SH is invalid.
            return IsMineResult::INVALID;
        }
        CScriptID scriptID = CScriptID(uint160(vSolutions[0]));
        CScript subscript;
        if (keystore.GetCScript(scriptID, subscript)) {
            ret = std::max(ret, recurse_scripthash ? IsMineInner(keystore, subscript, IsMineSigVersion::P2SH) : IsMineResult::SPENDABLE);
        }
        break;
    }
    case TxoutType::WITNESS_V0_SCRIPTHASH:
    {
        if (sigversion == IsMineSigVersion::WITNESS_V0) {
            // P2WSH inside P2WSH is invalid.
            return IsMineResult::INVALID;
        }
        if (sigversion == IsMineSigVersion::TOP && !keystore.HaveCScript(CScriptID(CScript() << OP_0 << vSolutions[0]))) {
            break;
        }
        uint160 hash;
        CRIPEMD160().Write(vSolutions[0].data(), vSolutions[0].size()).Finalize(hash.begin());
        CScriptID scriptID = CScriptID(hash);
        CScript subscript;
        if (keystore.GetCScript(scriptID, subscript)) {
            ret = std::max(ret, recurse_scripthash ? IsMineInner(keystore, subscript, IsMineSigVersion::WITNESS_V0) : IsMineResult::SPENDABLE);
        }
        break;
    }

    case TxoutType::MULTISIG:
    {
        // Never treat bare multisig outputs as ours (they can still be made watchonly-though)
        if (sigversion == IsMineSigVersion::TOP) {
            break;
        }

        // Only consider transactions "mine" if we own ALL the
        // keys involved. Multi-signature transactions that are
        // partially owned (somebody else has a key that can spend
        // them) enable spend-out-from-under-you attacks, especially
        // in shared-wallet situations.
        std::vector<valtype> keys(vSolutions.begin()+1, vSolutions.begin()+vSolutions.size()-1);
        if (!PermitsUncompressed(sigversion)) {
            for (size_t i = 0; i < keys.size(); i++) {
                if (keys[i].size() != 33) {
                    return IsMineResult::INVALID;
                }
            }
        }
        if (HaveKeys(keys, keystore)) {
            ret = std::max(ret, IsMineResult::SPENDABLE);
        }
        break;
    }
    } // no default case, so the compiler can warn about missing cases

    if (ret == IsMineResult::NO && keystore.HaveWatchOnly(scriptPubKey)) {
        ret = std::max(ret, IsMineResult::WATCH_ONLY);
    }
    return ret;
}

} // namespace

isminetype LegacyScriptPubKeyMan::IsMine(const CScript& script) const
{
    switch (IsMineInner(*this, script, IsMineSigVersion::TOP)) {
    case IsMineResult::INVALID:
    case IsMineResult::NO:
        return ISMINE_NO;
    case IsMineResult::WATCH_ONLY:
        return ISMINE_WATCH_ONLY;
    case IsMineResult::SPENDABLE:
        return ISMINE_SPENDABLE;
    }
    assert(false);
}

bool LegacyScriptPubKeyMan::CheckDecryptionKey(const CKeyingMaterial& master_key, bool accept_no_keys)
{
    {
        LOCK(cs_KeyStore);
        assert(mapKeys.empty());

        bool keyPass = mapCryptedKeys.empty(); // Always pass when there are no encrypted keys
        bool keyFail = false;
        CryptedKeyMap::const_iterator mi = mapCryptedKeys.begin();
        WalletBatch batch(m_storage.GetDatabase());
        for (; mi != mapCryptedKeys.end(); ++mi)
        {
            const CPubKey &vchPubKey = (*mi).second.first;
            const std::vector<unsigned char> &vchCryptedSecret = (*mi).second.second;
            CKey key;
            if (!DecryptKey(master_key, vchCryptedSecret, vchPubKey, key))
            {
                keyFail = true;
                break;
            }
            keyPass = true;
            if (fDecryptionThoroughlyChecked)
                break;
            else {
                // Rewrite these encrypted keys with checksums
                batch.WriteCryptedKey(vchPubKey, vchCryptedSecret, mapKeyMetadata[vchPubKey.GetID()]);
            }
        }
        if (keyPass && keyFail)
        {
            LogPrintf("The wallet is probably corrupted: Some keys decrypt but not all.\n");
            throw std::runtime_error("Error unlocking wallet: some keys decrypt but not all. Your wallet file may be corrupt.");
        }
        if (keyFail || (!keyPass && !accept_no_keys))
            return false;
        fDecryptionThoroughlyChecked = true;
    }
    return true;
}

bool LegacyScriptPubKeyMan::Encrypt(const CKeyingMaterial& master_key, WalletBatch* batch)
{
    LOCK(cs_KeyStore);
    encrypted_batch = batch;
    if (!mapCryptedKeys.empty()) {
        encrypted_batch = nullptr;
        return false;
    }

    // A chain whose seed is a key of the wallet may hold a copy of that key:
    // it is put there when an unencrypted wallet is loaded. The key is about
    // to be encrypted, and the copy must not outlive that, in the active
    // chain or in one that was replaced. Derivation reads the key itself.
    const auto drop_seed_copy = [this](CHDChain& chain) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore) {
        if (!chain.vchSeed.empty() && mapKeys.count(chain.seed_id) > 0) SecureVector().swap(chain.vchSeed);
    };
    drop_seed_copy(m_hd_chain);
    for (auto& chain_pair : m_inactive_hd_chains) drop_seed_copy(chain_pair.second);

    KeyMap keys_to_encrypt;
    keys_to_encrypt.swap(mapKeys); // Clear mapKeys so AddCryptedKeyInner will succeed.
    for (const KeyMap::value_type& mKey : keys_to_encrypt)
    {
        const CKey &key = mKey.second;
        CPubKey vchPubKey = key.GetPubKey();
        CKeyingMaterial vchSecret(key.begin(), key.end());
        std::vector<unsigned char> vchCryptedSecret;
        if (!EncryptSecret(master_key, vchSecret, vchPubKey.GetHash(), vchCryptedSecret)) {
            encrypted_batch = nullptr;
            return false;
        }
        if (!AddCryptedKey(vchPubKey, vchCryptedSecret)) {
            encrypted_batch = nullptr;
            return false;
        }
    }

    // A chain that holds its seed itself carries it, and the mnemonic it came
    // from, in its record. They are encrypted with everything else: into
    // their own record, in this same transaction, and the chain is written
    // without them. The seed is kept: the mnemonic has to go on restoring the
    // wallet.
    if (ChainHoldsSecrets(m_hd_chain)) {
        const CHDSecrets secrets{m_hd_chain.vchMnemonic, m_hd_chain.vchMnemonicPassphrase, m_hd_chain.vchSeed};
        const CHDChain chain{WithoutSecrets(m_hd_chain)};
        CHDSecretsRecord record;
        if (!EncryptHDSecrets(master_key, secrets, record) ||
            !m_storage.SetWalletFlagWithDB(*batch, WALLET_FLAG_HD_SECRETS_ENCRYPTED) ||
            !batch->WriteHDSecrets(record) ||
            !batch->WriteHDChain(chain) ||
            !batch->WriteHDSecretsPending()) {
            encrypted_batch = nullptr;
            return false;
        }
        // In memory as on disk. The unlock that follows encryption decrypts
        // the record; until then nothing holds the secrets in clear.
        m_hd_chain = chain;
        m_hd_secrets.SetNull();
        m_hd_secrets_record = record;
        m_hd_secrets_state = HDSecretsState::ENCRYPTED;
        // The old chain record is in the file until it is rewritten: owed,
        // as after a move at unlock, and retried if the rewrite fails.
        m_hd_secrets_pending = true;
        m_storage.ConfirmWalletFlag(WALLET_FLAG_HD_SECRETS_ENCRYPTED);
        RefreshHDType();
    }
    encrypted_batch = nullptr;
    return true;
}

bool LegacyScriptPubKeyMan::GetReservedDestination(const OutputType type, bool internal, CTxDestination& address, int64_t& index, CKeyPool& keypool, std::string& error)
{
    if (LEGACY_OUTPUT_TYPES.count(type) == 0) {
        error = _("Error: Legacy wallets only support the \"legacy\", \"p2sh-segwit\", and \"bech32\" address types").translated;
        return false;
    }
    assert(type != OutputType::BECH32M);

    LOCK(cs_KeyStore);
    if (!CanGetAddresses(internal)) {
        error = _("Error: Keypool ran out, please call keypoolrefill first").translated;
        return false;
    }

    if (!ReserveKeyFromKeyPool(index, keypool, internal)) {
        error = _("Error: Keypool ran out, please call keypoolrefill first").translated;
        return false;
    }
    address = GetDestinationForKey(keypool.vchPubKey, type);
    return true;
}

bool LegacyScriptPubKeyMan::TopUpInactiveHDChain(const CKeyID seed_id, int64_t index, bool internal)
{
    m_storage.AssertWalletLockHeld();
    LOCK(cs_KeyStore);

    if (m_storage.IsLocked()) return false;

    auto it = m_inactive_hd_chains.find(seed_id);
    if (it == m_inactive_hd_chains.end()) {
        return false;
    }

    CHDChain& chain = it->second;

    // A chain that was put together from the metadata of its keys when the
    // wallet was loaded has a seed id and nothing to derive from: neither the
    // seed bytes nor the seed as a key of the wallet. That is what is left of
    // the chain of a wallet created from a mnemonic once sethdseed, which
    // used to be accepted on such a wallet, has put another seed in its
    // place. Deriving would throw, in whatever thread a transaction paying
    // one of the old keys happened to be handled. The keys the wallet holds
    // go on working; there will be no more of them.
    SecureVector seed;
    if (!GetHDSeed(chain, seed)) {
        // Said once for each seed, with the id getwalletinfo and
        // getaddressinfo show for it.
        if (m_inactive_seeds_not_derivable.insert(seed_id).second) {
            const std::string what{strprintf("cannot derive keys of HD seed %s: the wallet holds neither this seed nor the mnemonic it came from, "
                                             "as after sethdseed on a wallet created from a mnemonic. The keys of this seed that are in the wallet stay usable. "
                                             "No further address of it can be derived, so a payment to one beyond them will not be seen: move the funds to a new wallet",
                                             seed_id.GetHex())};
            WalletLogPrintf("%s\n", what);
        }
        return false;
    }

    // Top up key pool
    int64_t target_size = std::max(gArgs.GetArg("-keypool", DEFAULT_KEYPOOL_SIZE), (int64_t) 1);

    // "size" of the keypools. Not really the size, actually the difference between index and the chain counter
    // Since chain counter is 1 based and index is 0 based, one of them needs to be offset by 1.
    int64_t kp_size = (internal ? chain.nInternalChainCounter : chain.nExternalChainCounter) - (index + 1);

    // make sure the keypool fits the user-selected target (-keypool)
    int64_t missing = std::max(target_size - kp_size, (int64_t) 0);

    if (missing > 0) {
        WalletBatch batch(m_storage.GetDatabase());
        for (int64_t i = missing; i > 0; --i) {
            GenerateNewKey(batch, chain, internal);
        }
        if (internal) {
            WalletLogPrintf("inactive seed with id %s added %d internal keys\n", HexStr(seed_id), missing);
        } else {
            WalletLogPrintf("inactive seed with id %s added %d keys\n", HexStr(seed_id), missing);
        }
    }
    return true;
}

void LegacyScriptPubKeyMan::MarkUnusedAddresses(const CScript& script)
{
    LOCK(cs_KeyStore);
    // extract addresses and check if they match with an unused keypool key
    for (const auto& keyid : GetAffectedKeys(script, *this)) {
        std::map<CKeyID, int64_t>::const_iterator mi = m_pool_key_to_index.find(keyid);
        if (mi != m_pool_key_to_index.end()) {
            WalletLogPrintf("%s: Detected a used keypool key, mark all keypool keys up to this key as used\n", __func__);
            MarkReserveKeysAsUsed(mi->second);

            if (!TopUp()) {
                WalletLogPrintf("%s: Topping up keypool failed (locked wallet)\n", __func__);
            }
        }

        // Find the key's metadata and check if it's seed id (if it has one) is inactive, i.e. it is not the current m_hd_chain seed id.
        // If so, TopUp the inactive hd chain
        auto it = mapKeyMetadata.find(keyid);
        if (it != mapKeyMetadata.end()){
            CKeyMetadata meta = it->second;
            // A key of an HD account above 0 of the active seed: the account
            // is used, and the one after it is kept ahead.
            if (!meta.hd_seed_id.IsNull() && meta.hd_seed_id == m_hd_chain.seed_id && HDAccountsEnabled() &&
                (meta.key_origin.path.size() == 3 || meta.key_origin.path.size() == 5) && HDAccountOfKeyPath(meta.key_origin.path) != 0) {
                MarkHDAccountKeyUsed(meta);
            }
            // An inactive chain is topped up for keys of account 0 only: a
            // key of another HD account says nothing of where account 0 is.
            if (!meta.hd_seed_id.IsNull() && meta.hd_seed_id != m_hd_chain.seed_id && HDAccountOfKeyPath(meta.key_origin.path) == 0) {
                // Which chain the key is on and where: m/0'/chain'/index' for
                // a plain HD seed, m/44'/coin'/account'/chain/index for a
                // BIP44 one, where the second and third elements are the coin
                // type and the account.
                const std::vector<uint32_t>& path = meta.key_origin.path;
                bool internal = false;
                int64_t index = -1;
                if (path.size() == 3) {
                    internal = (path[1] & ~BIP32_HARDENED_KEY_LIMIT) != 0;
                    index = path[2] & ~BIP32_HARDENED_KEY_LIMIT;
                } else if (path.size() == 5) {
                    internal = path[3] != 0;
                    index = path[4];
                }

                if (index >= 0 && !TopUpInactiveHDChain(meta.hd_seed_id, index, internal)) {
                    WalletLogPrintf("%s: Adding inactive seed keys failed\n", __func__);
                }
            }
        }
    }
}

void LegacyScriptPubKeyMan::UpgradeKeyMetadata()
{
    LOCK(cs_KeyStore);
    if (m_storage.IsLocked() || m_storage.IsWalletFlagSet(WALLET_FLAG_KEY_ORIGIN_METADATA)) {
        return;
    }

    std::unique_ptr<WalletBatch> batch = std::make_unique<WalletBatch>(m_storage.GetDatabase());
    for (auto& meta_pair : mapKeyMetadata) {
        CKeyMetadata& meta = meta_pair.second;
        if (!meta.hd_seed_id.IsNull() && !meta.has_key_origin && meta.hdKeypath != "s") { // If the hdKeypath is "s", that's the seed and it doesn't have a key origin
            CKey key;
            GetKey(meta.hd_seed_id, key);
            CExtKey masterKey;
            masterKey.SetSeed(key.begin(), key.size());
            // Add to map
            CKeyID master_id = masterKey.key.GetPubKey().GetID();
            std::copy(master_id.begin(), master_id.begin() + 4, meta.key_origin.fingerprint);
            if (!ParseHDKeypath(meta.hdKeypath, meta.key_origin.path)) {
                throw std::runtime_error("Invalid stored hdKeypath");
            }
            meta.has_key_origin = true;
            if (meta.nVersion < CKeyMetadata::VERSION_WITH_KEY_ORIGIN) {
                meta.nVersion = CKeyMetadata::VERSION_WITH_KEY_ORIGIN;
            }

            // Write meta to wallet
            CPubKey pubkey;
            if (GetPubKey(meta_pair.first, pubkey)) {
                batch->WriteKeyMetadata(meta, pubkey, true);
            }
        }
    }
}

bool LegacyScriptPubKeyMan::SetupGeneration(const WalletOptions& walletoptions, bool force)
{
    // A new seed in the place of the seed of a wallet that holds HD accounts
    // would leave its accounts behind.
    if (force && KeepsHDSeed()) {
        WalletLogPrintf("LegacyScriptPubKeyMan::SetupGeneration: refused, the wallet holds HD accounts and keeps its seed\n");
        return false;
    }

    if ((CanGenerateKeys() && !force) || m_storage.IsLocked()) {
        return false;
    }

    switch (walletoptions.walletType) {
    case walletType::bip32Wallet: {
        SetHDSeed(GenerateNewSeed(walletoptions));
        break;
    }
    case walletType::bip39Wallet:
    case walletType::bip44Wallet: {
        GenerateNewBip39Seed(walletoptions);
        break;
    }
    default:
        return false;
    }

    if (!NewKeyPool()) {
        return false;
    }
    return true;
}

bool LegacyScriptPubKeyMan::IsHDEnabled() const
{
    return !m_hd_chain.seed_id.IsNull();
}

bool LegacyScriptPubKeyMan::IsBip39Enabled() const
{
    LOCK(cs_KeyStore);
    return m_seed_held_by_chain;
}

bool LegacyScriptPubKeyMan::IsBip44Enabled() const
{
    LOCK(cs_KeyStore);
    return m_seed_held_by_chain && m_hd_chain.bBip44;
}

bool LegacyScriptPubKeyMan::ChainHoldsSecrets(const CHDChain& chain)
{
    return chain.nVersion == CHDChain::VERSION_HD_BIP39 && !chain.seed_id.IsNull() && !chain.vchSeed.empty();
}

CHDChain LegacyScriptPubKeyMan::WithoutSecrets(const CHDChain& chain)
{
    CHDChain result{chain};
    SecureVector().swap(result.vchMnemonic);
    SecureVector().swap(result.vchMnemonicPassphrase);
    SecureVector().swap(result.vchSeed);
    return result;
}

void LegacyScriptPubKeyMan::RefreshHDType()
{
    // Does the chain hold its seed itself, as the chain of a wallet made from
    // a mnemonic does? One rule, asked by everything that needs to know:
    // encryption, which must not replace such a seed, and sethdseed, which
    // must not either.
    // - A wallet whose secrets are kept in their own record has none in its
    //   chain to look at: there the version of the chain says it.
    // - Any other wallet is what it has always been taken for: it has a
    //   mnemonic or it has not. No wallet changes type by being loaded.
    // - A wallet whose secrets are known to be in their record is of that
    //   kind too when its flag could not be written (case D on a medium that
    //   cannot be written to).
    if (m_storage.IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED) || m_hd_secrets_state == HDSecretsState::ENCRYPTED) {
        m_seed_held_by_chain = IsHDEnabled() && m_hd_chain.nVersion == CHDChain::VERSION_HD_BIP39;
    } else {
        m_seed_held_by_chain = IsHDEnabled() && !m_hd_chain.vchMnemonic.empty();
    }

    // Keys of a seed other than the chain's, beside a chain whose record holds
    // a seed of its own: the wallet was converted by an earlier version when
    // it was encrypted, and its seed does not restore those keys. That holds
    // for a chain made from -hdseed too, which has a seed and no phrase and
    // reads as a plain wallet until its seed is moved. A plain wallet's chain
    // record holds no seed: its seed is one of its keys. Decided here, with
    // the kind, so that it holds from the move on, not from the next load.
    m_earlier_seed_keys = false;
    if (IsHDEnabled() && (m_seed_held_by_chain || ChainHoldsSecrets(m_hd_chain))) {
        for (const auto& meta_pair : mapKeyMetadata) {
            const CKeyMetadata& meta = meta_pair.second;
            if (!meta.hd_seed_id.IsNull() && meta.hd_seed_id != m_hd_chain.seed_id) {
                m_earlier_seed_keys = true;
                break;
            }
        }
    }
}

std::optional<bool> LegacyScriptPubKeyMan::KnownToHavePhrase() const
{
    AssertLockHeld(cs_KeyStore);
    // Without the passphrase this is known only while the secrets are in the
    // chain record. The encrypted record has no field that says it.
    if (m_hd_secrets_state == HDSecretsState::IN_CHAIN || m_hd_secrets_state == HDSecretsState::UNMIGRATED) {
        return !m_hd_chain.vchMnemonic.empty();
    }
    if (m_hd_secrets_state == HDSecretsState::ENCRYPTED && !m_hd_secrets.IsNull()) {
        return !m_hd_secrets.vchMnemonic.empty();
    }
    return std::nullopt;
}

bilingual_str LegacyScriptPubKeyMan::EarlierSeedWarning(bool with_advice) const
{
    AssertLockHeld(cs_KeyStore);
    const std::optional<bool> phrase = KnownToHavePhrase();
    bilingual_str what;
    if (phrase.has_value() && *phrase) {
        what = _("This wallet was converted to a mnemonic wallet by an earlier version (4.22.5 to 4.22.9.3) when it was encrypted, and you may never have been shown its mnemonic phrase. The addresses the wallet had before then come from its old seed. Its mnemonic phrase does not restore them: if this wallet were restored from the phrase alone, the funds on those addresses would not be there. Only this wallet file holds the keys to all of its addresses.");
    } else if (phrase.has_value()) {
        what = _("The HD seed of this wallet was replaced by an earlier version (4.22.5 to 4.22.9.3) when it was encrypted. This wallet has no mnemonic phrase. Its seed does not restore the addresses the wallet had before then: if this wallet were restored from the seed alone, the funds on those addresses would not be there. Only this wallet file holds the keys to all of its addresses.");
    } else {
        what = _("The HD seed of this wallet was replaced by an earlier version (4.22.5 to 4.22.9.3) when it was encrypted. Neither its seed nor its mnemonic phrase, if it has one, restores the addresses the wallet had before then: if this wallet were restored from either alone, the funds on those addresses would not be there. Only this wallet file holds the keys to all of its addresses.");
    }
    if (!with_advice) return what;
    return what + Untranslated("\n\n") + SeedDoesNotRestoreAllAdvice();
}

bilingual_str SeedDoesNotRestoreAllAdvice()
{
    return _("What to do:\n"
        "1. Recommended: create a new wallet, write down its mnemonic phrase, and send all the funds of this wallet to it. The new phrase then restores everything. Moved coins start their coin age for staking again.\n"
        "2. Otherwise, back up this wallet file now and keep that backup safe: it is the only backup that restores all of the wallet's addresses.");
}

bilingual_str LegacyScriptPubKeyMan::GetEarlierSeedWarning() const
{
    LOCK(cs_KeyStore);
    return EarlierSeedWarning(/* with_advice */ false);
}

LegacyScriptPubKeyMan::HDSecretsState LegacyScriptPubKeyMan::GetHDSecretsState() const
{
    LOCK(cs_KeyStore);
    return m_hd_secrets_state;
}

bool LegacyScriptPubKeyMan::IsHDSecretsRewritePending() const
{
    LOCK(cs_KeyStore);
    return m_hd_secrets_pending;
}

bool LegacyScriptPubKeyMan::GetHDSeed(const CHDChain& chain, SecureVector& seed) const
{
    LOCK(cs_KeyStore);
    if (&chain == &m_hd_chain) {
        if (m_hd_secrets_state == HDSecretsState::ENCRYPTED) {
            // Empty while the wallet is locked.
            if (m_hd_secrets.IsNull()) return false;
            seed = m_hd_secrets.vchSeed;
            return true;
        }
        if (m_hd_secrets_state == HDSecretsState::UNREADABLE) return false;
    }
    if (!chain.vchSeed.empty()) {
        seed = chain.vchSeed;
        return true;
    }
    // A seed that is a key of the wallet. Not readable while it is locked.
    CKey key;
    if (chain.seed_id.IsNull() || !GetKey(chain.seed_id, key)) return false;
    seed.assign(key.begin(), key.end());
    return true;
}

bool LegacyScriptPubKeyMan::GetHDSecrets(CHDSecrets& secrets) const
{
    LOCK(cs_KeyStore);
    switch (m_hd_secrets_state) {
    case HDSecretsState::ENCRYPTED:
        if (m_hd_secrets.IsNull()) return false;
        secrets = m_hd_secrets;
        return true;
    case HDSecretsState::IN_CHAIN:
    case HDSecretsState::UNMIGRATED:
        secrets = CHDSecrets{m_hd_chain.vchMnemonic, m_hd_chain.vchMnemonicPassphrase, m_hd_chain.vchSeed};
        return true;
    case HDSecretsState::NOT_HELD:
    case HDSecretsState::UNREADABLE:
        return false;
    } // no default case, so the compiler can warn about missing cases
    return false;
}

bool LegacyScriptPubKeyMan::CanDeriveFrom(const CHDChain& chain)
{
    // A wallet that is not HD makes its keys from nothing.
    if (chain.seed_id.IsNull()) return true;
    SecureVector seed;
    if (GetHDSeed(chain, seed)) return true;
    if (!m_no_seed_logged) {
        m_no_seed_logged = true;
        WalletLogPrintf("cannot derive a key: the seed of the wallet is not available. Addresses come from the key pool until it is empty\n");
    }
    return false;
}

void LegacyScriptPubKeyMan::LoadHDSecrets(const CHDSecretsRecord& record)
{
    LOCK(cs_KeyStore);
    m_hd_secrets_record = record;
}

void LegacyScriptPubKeyMan::LoadHDSecretsPending()
{
    LOCK(cs_KeyStore);
    m_hd_secrets_pending = true;
}

void LegacyScriptPubKeyMan::DecideHDSecretsState()
{
    m_storage.AssertWalletLockHeld();
    LOCK(cs_KeyStore);
    // The records that decide this (flags, chain, secrets, master key) are
    // read in the order of their keys and are not next to each other: it can
    // only be decided when all of them are in.
    const bool flagged{m_storage.IsWalletFlagSet(WALLET_FLAG_HD_SECRETS_ENCRYPTED)};
    const bool encrypted{m_storage.HasEncryptionKeys()};
    const bool chain3{m_hd_chain.nVersion == CHDChain::VERSION_HD_BIP39 && !m_hd_chain.seed_id.IsNull()};
    const bool clear{ChainHoldsSecrets(m_hd_chain)};
    const bool record{m_hd_secrets_record && m_hd_secrets_record->nVersion == CHDSecretsRecord::CURRENT_VERSION};

    if (!chain3) {
        m_hd_secrets_state = HDSecretsState::NOT_HELD;
    } else if (clear) {
        // The copy in the chain is the one to go by, whatever else is there.
        m_hd_secrets_state = encrypted ? HDSecretsState::UNMIGRATED : HDSecretsState::IN_CHAIN;
        if (flagged && !encrypted) {
            WalletLogPrintf("Warning: this wallet is marked as keeping the seed of its HD chain encrypted, but the wallet is not encrypted and the seed is in the chain record\n");
        }
    } else if (encrypted && record) {
        m_hd_secrets_state = HDSecretsState::ENCRYPTED;
        if (!flagged && m_storage.IsReadOnlyLoad()) {
            WalletLogPrintf("The seed of this wallet is kept encrypted; the wallet flag that says so is missing and is not written by this command\n");
        } else if (!flagged) {
            // The record is there and the mark is not: a tool that copies
            // records but not flags leaves a wallet so. Put the mark back, or
            // a version that does not know the record would take the wallet.
            WalletBatch batch(m_storage.GetDatabase());
            if (m_storage.SetWalletFlagWithDB(batch, WALLET_FLAG_HD_SECRETS_ENCRYPTED)) {
                m_storage.ConfirmWalletFlag(WALLET_FLAG_HD_SECRETS_ENCRYPTED);
                WalletLogPrintf("The seed of this wallet is kept encrypted; the wallet flag that says so was missing and has been set\n");
            } else {
                WalletLogPrintf("Error: the seed of this wallet is kept encrypted, and the wallet flag that says so could not be written\n");
            }
        }
    } else {
        m_hd_secrets_state = HDSecretsState::UNREADABLE;
        WalletLogPrintf("Error: the seed of this wallet cannot be read: the chain record does not hold it and %s. The keys the wallet holds work; no new keys can be made\n",
                        !encrypted ? "the wallet is not encrypted" : "there is no readable record of it");
    }
    RefreshHDType();
}

bool LegacyScriptPubKeyMan::HasEarlierSeedKeys() const
{
    LOCK(cs_KeyStore);
    return m_earlier_seed_keys;
}

std::vector<bilingual_str> LegacyScriptPubKeyMan::GetHDSecretsNotices(bool for_load) const
{
    LOCK(cs_KeyStore);
    std::vector<bilingual_str> notices;
    if (m_hd_secrets_state == HDSecretsState::UNMIGRATED) {
        notices.push_back(_("The seed of this wallet, and its mnemonic phrase if it has one, are stored without encryption in the wallet file. To remedy this, the next time the wallet is unlocked they will be encrypted and the wallet file rewritten. Until then anyone who can read this file can take its funds."));
    }
    if (m_hd_secrets_pending) {
        notices.push_back(_("The seed of this wallet is encrypted, but the wallet file could not be rewritten and may still hold the old copy. It will be tried again when the wallet is next loaded or unlocked."));
    }
    if (m_hd_secrets_state == HDSecretsState::UNREADABLE) {
        notices.push_back(_("The seed of this wallet cannot be read. The keys it already holds work, and addresses come from its key pool until that is empty; no new keys can be made. If you have this wallet's mnemonic phrase written down, restore it into a new wallet. If you do not, move the funds to a new wallet while this one still works."));
    }
    if (m_earlier_seed_keys) {
        // At load, under "Wallet <name>:", with the advice; elsewhere without it.
        notices.push_back(EarlierSeedWarning(/* with_advice */ for_load));
    }
    return notices;
}

bool LegacyScriptPubKeyMan::WriteHDSecretsEncrypted(const CKeyingMaterial& master_key, const CHDSecrets& secrets, const CHDChain& chain, bool pending, CHDSecretsRecord& record)
{
    CHDSecretsRecord encrypted;
    if (!EncryptHDSecrets(master_key, secrets, encrypted)) return false;
    {
        // The batch ends with this block: a rewrite of the file may follow,
        // and it waits for every batch to be gone.
        WalletBatch batch(m_storage.GetDatabase());
        if ((g_wallet_hd_write_fault && g_wallet_hd_write_fault("txnbegin")) || !batch.TxnBegin()) return false;
        if (!m_storage.SetWalletFlagWithDB(batch, WALLET_FLAG_HD_SECRETS_ENCRYPTED) ||
            !batch.WriteHDSecrets(encrypted) ||
            !batch.WriteHDChain(WithoutSecrets(chain)) ||
            (pending && !batch.WriteHDSecretsPending())) {
            batch.TxnAbort();
            return false;
        }
        if (g_wallet_hd_write_fault && g_wallet_hd_write_fault("txncommit")) {
            // A commit that fails leaves nothing of the transaction behind.
            batch.TxnAbort();
            return false;
        }
        if (!batch.TxnCommit()) return false;
    }
    record = std::move(encrypted);
    return true;
}

void LegacyScriptPubKeyMan::UnlockHDSecrets(const CKeyingMaterial& master_key, bool& need_rewrite)
{
    m_storage.AssertWalletLockHeld();
    LOCK(cs_KeyStore);
    try {
        if (m_hd_secrets_state == HDSecretsState::UNMIGRATED) {
            // An encrypted wallet whose secrets are still in the chain record,
            // as versions before this one left them. This is the first moment
            // the master key is at hand.
            const CHDSecrets secrets{m_hd_chain.vchMnemonic, m_hd_chain.vchMnemonicPassphrase, m_hd_chain.vchSeed};
            CHDSecretsRecord record;
            if (!WriteHDSecretsEncrypted(master_key, secrets, m_hd_chain, /* pending */ true, record)) {
                WalletLogPrintf("Error: the seed of this wallet could not be encrypted and is still stored without encryption in the wallet file. It will be tried again at the next unlock\n");
            } else {
                m_hd_chain = WithoutSecrets(m_hd_chain);
                m_hd_secrets = secrets;
                m_hd_secrets_record = record;
                m_hd_secrets_state = HDSecretsState::ENCRYPTED;
                m_hd_secrets_pending = true;
                m_storage.ConfirmWalletFlag(WALLET_FLAG_HD_SECRETS_ENCRYPTED);
                RefreshHDType();
            }
        } else if (m_hd_secrets_state == HDSecretsState::ENCRYPTED) {
            CHDSecrets secrets;
            if (m_hd_secrets_record && DecryptHDSecrets(master_key, *m_hd_secrets_record, secrets)) {
                m_hd_secrets = std::move(secrets);
            } else {
                m_hd_secrets.SetNull();
                m_hd_secrets_state = HDSecretsState::UNREADABLE;
                WalletLogPrintf("Error: the encrypted seed of this wallet cannot be read. The keys the wallet holds work; no new keys can be made\n");
            }
        }
    } catch (const std::exception& e) {
        // The wallet is unlocked whatever happened here.
        WalletLogPrintf("Error: %s while reading the seed of the wallet\n", e.what());
    }
    need_rewrite = m_hd_secrets_pending;
}

void LegacyScriptPubKeyMan::LockHDSecrets()
{
    LOCK(cs_KeyStore);
    m_hd_secrets.SetNull();
}

void LegacyScriptPubKeyMan::HDSecretsRewritten()
{
    LOCK(cs_KeyStore);
    m_hd_secrets_pending = false;
}

bool LegacyScriptPubKeyMan::WriteHDChain(WalletBatch& batch, const CHDChain& chain)
{
    // Once the secrets are in their own record, or are known to be nowhere,
    // the chain record never carries any, whatever the object holds. For
    // every other wallet it is written as it is: the chain record is then the
    // only place the seed is.
    if (m_hd_secrets_state == HDSecretsState::ENCRYPTED || m_hd_secrets_state == HDSecretsState::UNREADABLE) {
        return batch.WriteHDChain(WithoutSecrets(chain));
    }
    return batch.WriteHDChain(chain);
}

void LegacyScriptPubKeyMan::DecideMixedKeyPaths()
{
    LOCK(cs_KeyStore);
    // Versions 4.22.7 to 4.22.9.3 could give a wallet created with a passphrase
    // all its first keys on BIP44 paths (m/44'/coin'/0'/chain/index), from its
    // plain seed: the chain they made did not set its BIP44 switch, and the
    // version 2 record it is stored in has no field for that switch. Later
    // versions derive its new keys on the ordinary paths. The seed alone then
    // restores only those. Only a chain that is not made from a mnemonic, and
    // whose record cannot hold the switch, derives so.
    m_mixed_key_paths = false;
    if (!IsHDEnabled() || IsBip39Enabled() || m_hd_chain.nVersion >= CHDChain::VERSION_HD_BIP39) return;
    for (const auto& meta_pair : mapKeyMetadata) {
        const CKeyMetadata& meta = meta_pair.second;
        if (meta.has_key_origin && meta.hd_seed_id == m_hd_chain.seed_id && meta.key_origin.path.size() == 5) {
            m_mixed_key_paths = true;
            break;
        }
    }
}

bool LegacyScriptPubKeyMan::HasMixedKeyPaths() const
{
    LOCK(cs_KeyStore);
    return m_mixed_key_paths;
}

bool LegacyScriptPubKeyMan::CanGetAddresses(bool internal) const
{
    LOCK(cs_KeyStore);
    // Check if the keypool has keys
    bool keypool_has_keys;
    if (internal && m_storage.CanSupportFeature(FEATURE_HD_SPLIT)) {
        keypool_has_keys = setInternalKeyPool.size() > 0;
    } else {
        keypool_has_keys = KeypoolCountExternalKeys() > 0;
    }
    // If the keypool doesn't have keys, check if we can generate them
    if (!keypool_has_keys) {
        return CanGenerateKeys();
    }
    return keypool_has_keys;
}

bool LegacyScriptPubKeyMan::Upgrade(int prev_version, int new_version, bilingual_str& error)
{
    LOCK(cs_KeyStore);
    bool hd_upgrade = false;
    bool split_upgrade = false;
    if (IsFeatureSupported(new_version, FEATURE_HD) && !IsHDEnabled()) {
        WalletLogPrintf("Upgrading wallet to HD\n");
        m_storage.SetMinVersion(FEATURE_HD);

        // generate a new master key
        CPubKey masterPubKey = GenerateNewSeed();
        SetHDSeed(masterPubKey);
        hd_upgrade = true;
    }
    // Upgrade to HD chain split if necessary
    if (!IsFeatureSupported(prev_version, FEATURE_HD_SPLIT) && IsFeatureSupported(new_version, FEATURE_HD_SPLIT)) {
        WalletLogPrintf("Upgrading wallet to use HD chain split\n");
        m_storage.SetMinVersion(FEATURE_PRE_SPLIT_KEYPOOL);
        split_upgrade = FEATURE_HD_SPLIT > prev_version;
        // Upgrade the HDChain
        if (m_hd_chain.nVersion < CHDChain::VERSION_HD_CHAIN_SPLIT) {
            m_hd_chain.nVersion = CHDChain::VERSION_HD_CHAIN_SPLIT;
            WalletBatch batch(m_storage.GetDatabase());
            if (!WriteHDChain(batch, m_hd_chain)) {
                throw std::runtime_error(std::string(__func__) + ": writing chain failed");
            }
        }
    }
    // Mark all keys currently in the keypool as pre-split
    if (split_upgrade) {
        MarkPreSplitKeys();
    }
    // Regenerate the keypool if upgraded to HD
    if (hd_upgrade) {
        if (!TopUp()) {
            error = _("Unable to generate keys");
            return false;
        }
    }
    return true;
}

bool LegacyScriptPubKeyMan::HavePrivateKeys() const
{
    LOCK(cs_KeyStore);
    return !mapKeys.empty() || !mapCryptedKeys.empty();
}

void LegacyScriptPubKeyMan::RewriteDB()
{
    LOCK(cs_KeyStore);
    setInternalKeyPool.clear();
    setExternalKeyPool.clear();
    m_pool_key_to_index.clear();
    // Note: can't top-up keypool here, because wallet is locked.
    // User will be prompted to unlock wallet the next operation
    // that requires a new key.
}

static int64_t GetOldestKeyTimeInPool(const std::set<int64_t>& setKeyPool, WalletBatch& batch) {
    if (setKeyPool.empty()) {
        return GetTime();
    }

    CKeyPool keypool;
    int64_t nIndex = *(setKeyPool.begin());
    if (!batch.ReadPool(nIndex, keypool)) {
        throw std::runtime_error(std::string(__func__) + ": read oldest key in keypool failed");
    }
    assert(keypool.vchPubKey.IsValid());
    return keypool.nTime;
}

int64_t LegacyScriptPubKeyMan::GetOldestKeyPoolTime() const
{
    LOCK(cs_KeyStore);

    WalletBatch batch(m_storage.GetDatabase());

    // load oldest key from keypool, get time and return
    int64_t oldestKey = GetOldestKeyTimeInPool(setExternalKeyPool, batch);
    if (IsHDEnabled() && m_storage.CanSupportFeature(FEATURE_HD_SPLIT)) {
        oldestKey = std::max(GetOldestKeyTimeInPool(setInternalKeyPool, batch), oldestKey);
        if (!set_pre_split_keypool.empty()) {
            oldestKey = std::max(GetOldestKeyTimeInPool(set_pre_split_keypool, batch), oldestKey);
        }
    }

    return oldestKey;
}

size_t LegacyScriptPubKeyMan::KeypoolCountExternalKeys() const
{
    LOCK(cs_KeyStore);
    return setExternalKeyPool.size() + set_pre_split_keypool.size();
}

unsigned int LegacyScriptPubKeyMan::GetKeyPoolSize() const
{
    LOCK(cs_KeyStore);
    return setInternalKeyPool.size() + setExternalKeyPool.size() + set_pre_split_keypool.size();
}

int64_t LegacyScriptPubKeyMan::GetTimeFirstKey() const
{
    LOCK(cs_KeyStore);
    return nTimeFirstKey;
}

std::unique_ptr<SigningProvider> LegacyScriptPubKeyMan::GetSolvingProvider(const CScript& script) const
{
    return std::make_unique<LegacySigningProvider>(*this);
}

bool LegacyScriptPubKeyMan::CanProvide(const CScript& script, SignatureData& sigdata)
{
    IsMineResult ismine = IsMineInner(*this, script, IsMineSigVersion::TOP, /* recurse_scripthash= */ false);
    if (ismine == IsMineResult::SPENDABLE || ismine == IsMineResult::WATCH_ONLY) {
        // If ismine, it means we recognize keys or script ids in the script, or
        // are watching the script itself, and we can at least provide metadata
        // or solving information, even if not able to sign fully.
        return true;
    } else {
        // If, given the stuff in sigdata, we could make a valid sigature, then we can provide for this script
        ProduceSignature(*this, DUMMY_SIGNATURE_CREATOR, script, sigdata);
        if (!sigdata.signatures.empty()) {
            // If we could make signatures, make sure we have a private key to actually make a signature
            bool has_privkeys = false;
            for (const auto& key_sig_pair : sigdata.signatures) {
                has_privkeys |= HaveKey(key_sig_pair.first);
            }
            return has_privkeys;
        }
        return false;
    }
}

bool LegacyScriptPubKeyMan::SignTransaction(CMutableTransaction& tx, const std::map<COutPoint, Coin>& coins, int sighash, std::map<int, std::string>& input_errors) const
{
    return ::SignTransaction(tx, this, coins, sighash, input_errors);
}

SigningResult LegacyScriptPubKeyMan::SignMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) const
{
    CKey key;
    if (!GetKey(ToKeyID(pkhash), key)) {
        return SigningResult::PRIVATE_KEY_NOT_AVAILABLE;
    }

    if (MessageSign(key, message, str_sig)) {
        return SigningResult::OK;
    }
    return SigningResult::SIGNING_FAILED;
}

TransactionError LegacyScriptPubKeyMan::FillPSBT(PartiallySignedTransaction& psbtx, const PrecomputedTransactionData& txdata, int sighash_type, bool sign, bool bip32derivs, int* n_signed) const
{
    if (n_signed) {
        *n_signed = 0;
    }
    for (unsigned int i = 0; i < psbtx.tx->vin.size(); ++i) {
        const CTxIn& txin = psbtx.tx->vin[i];
        PSBTInput& input = psbtx.inputs.at(i);

        if (PSBTInputSigned(input)) {
            continue;
        }

        // Get the Sighash type
        if (sign && input.sighash_type > 0 && input.sighash_type != sighash_type) {
            return TransactionError::SIGHASH_MISMATCH;
        }

        // Check non_witness_utxo has specified prevout
        if (input.non_witness_utxo) {
            if (txin.prevout.n >= input.non_witness_utxo->vout.size()) {
                return TransactionError::MISSING_INPUTS;
            }
        } else if (input.witness_utxo.IsNull()) {
            // There's no UTXO so we can just skip this now
            continue;
        }
        SignatureData sigdata;
        input.FillSignatureData(sigdata);
        SignPSBTInput(HidingSigningProvider(this, !sign, !bip32derivs), psbtx, i, &txdata, sighash_type);

        bool signed_one = PSBTInputSigned(input);
        if (n_signed && (signed_one || !sign)) {
            // If sign is false, we assume that we _could_ sign if we get here. This
            // will never have false negatives; it is hard to tell under what i
            // circumstances it could have false positives.
            (*n_signed)++;
        }
    }

    // Fill in the bip32 keypaths and redeemscripts for the outputs so that hardware wallets can identify change
    for (unsigned int i = 0; i < psbtx.tx->vout.size(); ++i) {
        UpdatePSBTOutput(HidingSigningProvider(this, true, !bip32derivs), psbtx, i);
    }

    return TransactionError::OK;
}

std::unique_ptr<CKeyMetadata> LegacyScriptPubKeyMan::GetMetadata(const CTxDestination& dest) const
{
    LOCK(cs_KeyStore);

    CKeyID key_id = GetKeyForDestination(*this, dest);
    if (!key_id.IsNull()) {
        auto it = mapKeyMetadata.find(key_id);
        if (it != mapKeyMetadata.end()) {
            return std::make_unique<CKeyMetadata>(it->second);
        }
    }

    CScript scriptPubKey = GetScriptForDestination(dest);
    auto it = m_script_metadata.find(CScriptID(scriptPubKey));
    if (it != m_script_metadata.end()) {
        return std::make_unique<CKeyMetadata>(it->second);
    }

    return nullptr;
}

uint256 LegacyScriptPubKeyMan::GetID() const
{
    return uint256::ONE;
}

/**
 * Update wallet first key creation time. This should be called whenever keys
 * are added to the wallet, with the oldest key creation time.
 */
void LegacyScriptPubKeyMan::UpdateTimeFirstKey(int64_t nCreateTime)
{
    AssertLockHeld(cs_KeyStore);
    if (nCreateTime <= 1) {
        // Cannot determine birthday information, so set the wallet birthday to
        // the beginning of time.
        nTimeFirstKey = 1;
    } else if (!nTimeFirstKey || nCreateTime < nTimeFirstKey) {
        nTimeFirstKey = nCreateTime;
    }
}

bool LegacyScriptPubKeyMan::LoadKey(const CKey& key, const CPubKey &pubkey)
{
    return AddKeyPubKeyInner(key, pubkey);
}

bool LegacyScriptPubKeyMan::AddKeyPubKey(const CKey& secret, const CPubKey &pubkey)
{
    LOCK(cs_KeyStore);
    WalletBatch batch(m_storage.GetDatabase());
    return LegacyScriptPubKeyMan::AddKeyPubKeyWithDB(batch, secret, pubkey);
}

bool LegacyScriptPubKeyMan::AddKeyPubKeyWithDB(WalletBatch& batch, const CKey& secret, const CPubKey& pubkey)
{
    AssertLockHeld(cs_KeyStore);

    // Make sure we aren't adding private keys to private key disabled wallets
    assert(!m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS));

    // FillableSigningProvider has no concept of wallet databases, but calls AddCryptedKey
    // which is overridden below.  To avoid flushes, the database handle is
    // tunneled through to it.
    bool needsDB = !encrypted_batch;
    if (needsDB) {
        encrypted_batch = &batch;
    }
    if (!AddKeyPubKeyInner(secret, pubkey)) {
        if (needsDB) encrypted_batch = nullptr;
        return false;
    }
    if (needsDB) encrypted_batch = nullptr;

    // check if we need to remove from watch-only
    CScript script;
    script = GetScriptForDestination(PKHash(pubkey));
    if (HaveWatchOnly(script)) {
        RemoveWatchOnly(script);
    }
    script = GetScriptForRawPubKey(pubkey);
    if (HaveWatchOnly(script)) {
        RemoveWatchOnly(script);
    }

    if (!m_storage.HasEncryptionKeys()) {
        return batch.WriteKey(pubkey,
                                                 secret.GetPrivKey(),
                                                 mapKeyMetadata[pubkey.GetID()]);
    }
    m_storage.UnsetBlankWalletFlag(batch);
    return true;
}

bool LegacyScriptPubKeyMan::LoadCScript(const CScript& redeemScript)
{
    /* A sanity check was added in pull #3843 to avoid adding redeemScripts
     * that never can be redeemed. However, old wallets may still contain
     * these. Do not add them to the wallet and warn. */
    if (redeemScript.size() > MAX_SCRIPT_ELEMENT_SIZE)
    {
        std::string strAddr = EncodeDestination(ScriptHash(redeemScript));
        WalletLogPrintf("%s: Warning: This wallet contains a redeemScript of size %i which exceeds maximum size %i thus can never be redeemed. Do not use address %s.\n", __func__, redeemScript.size(), MAX_SCRIPT_ELEMENT_SIZE, strAddr);
        return true;
    }

    return FillableSigningProvider::AddCScript(redeemScript);
}

void LegacyScriptPubKeyMan::LoadKeyMetadata(const CKeyID& keyID, const CKeyMetadata& meta)
{
    LOCK(cs_KeyStore);
    UpdateTimeFirstKey(meta.nCreateTime);
    mapKeyMetadata[keyID] = meta;
}

void LegacyScriptPubKeyMan::LoadScriptMetadata(const CScriptID& script_id, const CKeyMetadata& meta)
{
    LOCK(cs_KeyStore);
    UpdateTimeFirstKey(meta.nCreateTime);
    m_script_metadata[script_id] = meta;
}

bool LegacyScriptPubKeyMan::AddKeyPubKeyInner(const CKey& key, const CPubKey &pubkey)
{
    LOCK(cs_KeyStore);
    if (!m_storage.HasEncryptionKeys()) {
        return FillableSigningProvider::AddKeyPubKey(key, pubkey);
    }

    if (m_storage.IsLocked()) {
        return false;
    }

    std::vector<unsigned char> vchCryptedSecret;
    CKeyingMaterial vchSecret(key.begin(), key.end());
    if (!EncryptSecret(m_storage.GetEncryptionKey(), vchSecret, pubkey.GetHash(), vchCryptedSecret)) {
        return false;
    }

    if (!AddCryptedKey(pubkey, vchCryptedSecret)) {
        return false;
    }
    return true;
}

bool LegacyScriptPubKeyMan::LoadCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret, bool checksum_valid)
{
    // Set fDecryptionThoroughlyChecked to false when the checksum is invalid
    if (!checksum_valid) {
        fDecryptionThoroughlyChecked = false;
    }

    return AddCryptedKeyInner(vchPubKey, vchCryptedSecret);
}

bool LegacyScriptPubKeyMan::AddCryptedKeyInner(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret)
{
    LOCK(cs_KeyStore);
    assert(mapKeys.empty());

    mapCryptedKeys[vchPubKey.GetID()] = make_pair(vchPubKey, vchCryptedSecret);
    ImplicitlyLearnRelatedKeyScripts(vchPubKey);
    return true;
}

bool LegacyScriptPubKeyMan::AddCryptedKey(const CPubKey &vchPubKey,
                            const std::vector<unsigned char> &vchCryptedSecret)
{
    if (!AddCryptedKeyInner(vchPubKey, vchCryptedSecret))
        return false;
    {
        LOCK(cs_KeyStore);
        if (encrypted_batch)
            return encrypted_batch->WriteCryptedKey(vchPubKey,
                                                        vchCryptedSecret,
                                                        mapKeyMetadata[vchPubKey.GetID()]);
        else
            return WalletBatch(m_storage.GetDatabase()).WriteCryptedKey(vchPubKey,
                                                            vchCryptedSecret,
                                                            mapKeyMetadata[vchPubKey.GetID()]);
    }
}

bool LegacyScriptPubKeyMan::HaveWatchOnly(const CScript &dest) const
{
    LOCK(cs_KeyStore);
    return setWatchOnly.count(dest) > 0;
}

bool LegacyScriptPubKeyMan::HaveWatchOnly() const
{
    LOCK(cs_KeyStore);
    return (!setWatchOnly.empty());
}

static bool ExtractPubKey(const CScript &dest, CPubKey& pubKeyOut)
{
    std::vector<std::vector<unsigned char>> solutions;
    return Solver(dest, solutions) == TxoutType::PUBKEY &&
        (pubKeyOut = CPubKey(solutions[0])).IsFullyValid();
}

bool LegacyScriptPubKeyMan::RemoveWatchOnly(const CScript &dest)
{
    {
        LOCK(cs_KeyStore);
        setWatchOnly.erase(dest);
        CPubKey pubKey;
        if (ExtractPubKey(dest, pubKey)) {
            mapWatchKeys.erase(pubKey.GetID());
        }
        // Related CScripts are not removed; having superfluous scripts around is
        // harmless (see comment in ImplicitlyLearnRelatedKeyScripts).
    }

    if (!HaveWatchOnly())
        NotifyWatchonlyChanged(false);
    if (!WalletBatch(m_storage.GetDatabase()).EraseWatchOnly(dest))
        return false;

    return true;
}

bool LegacyScriptPubKeyMan::LoadWatchOnly(const CScript &dest)
{
    return AddWatchOnlyInMem(dest);
}

bool LegacyScriptPubKeyMan::AddWatchOnlyInMem(const CScript &dest)
{
    LOCK(cs_KeyStore);
    setWatchOnly.insert(dest);
    CPubKey pubKey;
    if (ExtractPubKey(dest, pubKey)) {
        mapWatchKeys[pubKey.GetID()] = pubKey;
        ImplicitlyLearnRelatedKeyScripts(pubKey);
    }
    return true;
}

bool LegacyScriptPubKeyMan::AddWatchOnlyWithDB(WalletBatch &batch, const CScript& dest)
{
    if (!AddWatchOnlyInMem(dest))
        return false;
    const CKeyMetadata& meta = m_script_metadata[CScriptID(dest)];
    UpdateTimeFirstKey(meta.nCreateTime);
    NotifyWatchonlyChanged(true);
    if (batch.WriteWatchOnly(dest, meta)) {
        m_storage.UnsetBlankWalletFlag(batch);
        return true;
    }
    return false;
}

bool LegacyScriptPubKeyMan::AddWatchOnlyWithDB(WalletBatch &batch, const CScript& dest, int64_t create_time)
{
    m_script_metadata[CScriptID(dest)].nCreateTime = create_time;
    return AddWatchOnlyWithDB(batch, dest);
}

bool LegacyScriptPubKeyMan::AddWatchOnly(const CScript& dest)
{
    WalletBatch batch(m_storage.GetDatabase());
    return AddWatchOnlyWithDB(batch, dest);
}

bool LegacyScriptPubKeyMan::AddWatchOnly(const CScript& dest, int64_t nCreateTime)
{
    m_script_metadata[CScriptID(dest)].nCreateTime = nCreateTime;
    return AddWatchOnly(dest);
}

void LegacyScriptPubKeyMan::LoadHDChain(const CHDChain& chain)
{
    LOCK(cs_KeyStore);
    m_hd_chain = chain;
    RefreshHDType();
}

void LegacyScriptPubKeyMan::AddHDChain(const CHDChain& chain)
{
    WalletBatch batch(m_storage.GetDatabase());
    AddHDChain(batch, chain);
}

void LegacyScriptPubKeyMan::AddHDChain(WalletBatch& batch, const CHDChain& chain)
{
    LOCK(cs_KeyStore);
    // Store the new chain
    if (!batch.WriteHDChain(chain)) {
        throw std::runtime_error(std::string(__func__) + ": writing chain failed");
    }
    // When there's an old chain, add it as an inactive chain as we are now rotating hd chains
    if (!m_hd_chain.seed_id.IsNull()) {
        AddInactiveHDChain(m_hd_chain);
    }

    m_hd_chain = chain;
    // A chain that is added carries what it has: one whose seed is a key of
    // the wallet, or one with its secrets in it, in a wallet that is not
    // encrypted. An encrypted wallet's mnemonic chain is not added this way.
    m_hd_secrets.SetNull();
    m_hd_secrets_record.reset();
    m_hd_secrets_state = ChainHoldsSecrets(chain) ? (m_storage.HasEncryptionKeys() ? HDSecretsState::UNMIGRATED : HDSecretsState::IN_CHAIN) : HDSecretsState::NOT_HELD;
    RefreshHDType();
}

void LegacyScriptPubKeyMan::AddInactiveHDChain(const CHDChain& chain)
{
    LOCK(cs_KeyStore);
    assert(!chain.seed_id.IsNull());
    m_inactive_hd_chains[chain.seed_id] = chain;
}

bool LegacyScriptPubKeyMan::HaveKey(const CKeyID &address) const
{
    LOCK(cs_KeyStore);
    if (!m_storage.HasEncryptionKeys()) {
        return FillableSigningProvider::HaveKey(address);
    }
    return mapCryptedKeys.count(address) > 0;
}

bool LegacyScriptPubKeyMan::GetKey(const CKeyID &address, CKey& keyOut) const
{
    LOCK(cs_KeyStore);
    if (!m_storage.HasEncryptionKeys()) {
        return FillableSigningProvider::GetKey(address, keyOut);
    }

    CryptedKeyMap::const_iterator mi = mapCryptedKeys.find(address);
    if (mi != mapCryptedKeys.end())
    {
        const CPubKey &vchPubKey = (*mi).second.first;
        const std::vector<unsigned char> &vchCryptedSecret = (*mi).second.second;
        return DecryptKey(m_storage.GetEncryptionKey(), vchCryptedSecret, vchPubKey, keyOut);
    }
    return false;
}

bool LegacyScriptPubKeyMan::GetKeyOrigin(const CKeyID& keyID, KeyOriginInfo& info) const
{
    CKeyMetadata meta;
    {
        LOCK(cs_KeyStore);
        auto it = mapKeyMetadata.find(keyID);
        if (it != mapKeyMetadata.end()) {
            meta = it->second;
        }
    }
    if (meta.has_key_origin) {
        std::copy(meta.key_origin.fingerprint, meta.key_origin.fingerprint + 4, info.fingerprint);
        info.path = meta.key_origin.path;
    } else { // Single pubkeys get the master fingerprint of themselves
        std::copy(keyID.begin(), keyID.begin() + 4, info.fingerprint);
    }
    return true;
}

bool LegacyScriptPubKeyMan::GetWatchPubKey(const CKeyID &address, CPubKey &pubkey_out) const
{
    LOCK(cs_KeyStore);
    WatchKeyMap::const_iterator it = mapWatchKeys.find(address);
    if (it != mapWatchKeys.end()) {
        pubkey_out = it->second;
        return true;
    }
    return false;
}

bool LegacyScriptPubKeyMan::GetPubKey(const CKeyID &address, CPubKey& vchPubKeyOut) const
{
    LOCK(cs_KeyStore);
    if (!m_storage.HasEncryptionKeys()) {
        if (!FillableSigningProvider::GetPubKey(address, vchPubKeyOut)) {
            return GetWatchPubKey(address, vchPubKeyOut);
        }
        return true;
    }

    CryptedKeyMap::const_iterator mi = mapCryptedKeys.find(address);
    if (mi != mapCryptedKeys.end())
    {
        vchPubKeyOut = (*mi).second.first;
        return true;
    }
    // Check for watch-only pubkeys
    return GetWatchPubKey(address, vchPubKeyOut);
}

CPubKey LegacyScriptPubKeyMan::GenerateNewKey(WalletBatch &batch, CHDChain& hd_chain, bool internal)
{
    assert(!m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS));
    assert(!m_storage.IsWalletFlagSet(WALLET_FLAG_BLANK_WALLET));
    AssertLockHeld(cs_KeyStore);
    bool fCompressed = m_storage.CanSupportFeature(FEATURE_COMPRPUBKEY); // default to compressed public keys if we want 0.6.0 wallets

    CKey secret;

    // Create new metadata
    int64_t nCreationTime = GetTime();
    CKeyMetadata metadata(nCreationTime);

    // use HD key derivation if HD was enabled during wallet creation and a seed is present
    if (IsHDEnabled()) {
        DeriveNewChildKey(batch, metadata, secret, hd_chain, (m_storage.CanSupportFeature(FEATURE_HD_SPLIT) ? internal : false));
    } else {
        secret.MakeNewKey(fCompressed);
    }

    // Compressed public keys were introduced in version 0.6.0
    if (fCompressed) {
        m_storage.SetMinVersion(FEATURE_COMPRPUBKEY);
    }

    CPubKey pubkey = secret.GetPubKey();
    assert(secret.VerifyPubKey(pubkey));

    mapKeyMetadata[pubkey.GetID()] = metadata;
    UpdateTimeFirstKey(nCreationTime);

    if (!AddKeyPubKeyWithDB(batch, secret, pubkey)) {
        throw std::runtime_error(std::string(__func__) + ": AddKey failed");
    }
    return pubkey;
}

HDAccountKey DeriveHDAccountKeyFromMaster(const CExtKey& master, bool bip44, uint32_t coin_type, uint32_t account, bool internal, uint32_t index)
{
    HDAccountKey result;
    CExtKey account_key;
    CExtKey chain_key;
    CExtKey child_key;
    if (bip44) {
        // m/44'/coin_type'/account'/chain/index
        CExtKey purpose_key;
        CExtKey coin_type_key;
        master.Derive(purpose_key, 44 | BIP32_HARDENED_KEY_LIMIT);
        purpose_key.Derive(coin_type_key, coin_type | BIP32_HARDENED_KEY_LIMIT);
        coin_type_key.Derive(account_key, account | BIP32_HARDENED_KEY_LIMIT);
        account_key.Derive(chain_key, internal ? 1 : 0);
        chain_key.Derive(child_key, index);
        result.path = {44 | BIP32_HARDENED_KEY_LIMIT, coin_type | BIP32_HARDENED_KEY_LIMIT, account | BIP32_HARDENED_KEY_LIMIT, internal ? 1U : 0U, index};
        result.path_text = strprintf("m/44'/%d'/%d'/%d/%d", coin_type, account, internal, index);
    } else {
        // m/account'/chain'/index'
        master.Derive(account_key, account | BIP32_HARDENED_KEY_LIMIT);
        account_key.Derive(chain_key, (internal ? 1 : 0) | BIP32_HARDENED_KEY_LIMIT);
        chain_key.Derive(child_key, index | BIP32_HARDENED_KEY_LIMIT);
        result.path = {account | BIP32_HARDENED_KEY_LIMIT, (internal ? 1U : 0U) | BIP32_HARDENED_KEY_LIMIT, index | BIP32_HARDENED_KEY_LIMIT};
        result.path_text = strprintf("m/%d'/%d'/%d'", account, internal, index);
    }
    result.key = child_key.key;
    return result;
}

bool LegacyScriptPubKeyMan::DeriveHDAccountKey(uint32_t account, bool internal, uint32_t index, CKey& key, CKeyMetadata& metadata)
{
    AssertLockHeld(cs_KeyStore);
    if (!IsHDEnabled() || m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) return false;
    if (internal && !m_storage.CanSupportFeature(FEATURE_HD_SPLIT)) return false;
    // Hardened, the account and an index of a plain or BIP39 chain have 31
    // bits: one beyond them would be another key's number.
    if (account >= BIP32_HARDENED_KEY_LIMIT || index >= BIP32_HARDENED_KEY_LIMIT) return false;
    if (!CanDeriveFrom(m_hd_chain)) return false;
    SecureVector seed_bytes;
    if (!GetHDSeed(m_hd_chain, seed_bytes)) return false;
    CExtKey master;
    master.SetSeed(seed_bytes.data(), seed_bytes.size());

    const HDAccountKey derived{DeriveHDAccountKeyFromMaster(master, m_hd_chain.IsBip44(), Params().ExtCoinType(), account, internal, index)};
    key = derived.key;
    metadata = CKeyMetadata(GetTime());
    metadata.hdKeypath = derived.path_text;
    metadata.key_origin.path = derived.path;
    metadata.hd_seed_id = m_hd_chain.seed_id;
    const CKeyID master_id = master.key.GetPubKey().GetID();
    std::copy(master_id.begin(), master_id.begin() + 4, metadata.key_origin.fingerprint);
    metadata.has_key_origin = true;
    return true;
}

bool LegacyScriptPubKeyMan::AddHDAccountKey(WalletBatch& batch, const CKey& key, const CKeyMetadata& metadata)
{
    AssertLockHeld(cs_KeyStore);
    const CPubKey pubkey = key.GetPubKey();
    if (HaveKey(pubkey.GetID())) return true;
    mapKeyMetadata[pubkey.GetID()] = metadata;
    UpdateTimeFirstKey(metadata.nCreateTime);
    if (!AddKeyPubKeyWithDB(batch, key, pubkey)) return false;
    // A key is handed out before it is paid, as is the case for every key of
    // an account kept ahead of use: the forms it can be paid to are known
    // from the start, segwit ones included.
    if (pubkey.IsCompressed()) {
        const CScript witprog = GetScriptForDestination(WitnessV0KeyHash(pubkey.GetID()));
        if (!AddCScriptWithDB(batch, witprog)) return false;
    }
    IndexHDAccountKey(pubkey, metadata);
    return true;
}

void LegacyScriptPubKeyMan::IndexHDAccountKey(const CPubKey& pubkey, const CKeyMetadata& metadata)
{
    AssertLockHeld(cs_KeyStore);
    if (metadata.hd_seed_id.IsNull() || metadata.hd_seed_id != m_hd_chain.seed_id) return;
    const std::vector<uint32_t>& path = metadata.key_origin.path;
    if (path.size() != 3 && path.size() != 5) return;
    const uint32_t account{HDAccountOfKeyPath(path)};
    if (account == 0) return;
    const bool internal{(path.size() == 5 ? path[3] : path[1] & ~BIP32_HARDENED_KEY_LIMIT) != 0};
    const int64_t index{path.size() == 5 ? path[4] : path[2] & ~BIP32_HARDENED_KEY_LIMIT};
    auto& [last_receive, last_change] = m_hd_account_extent.emplace(account, std::make_pair(int64_t{-1}, int64_t{-1})).first->second;
    int64_t& last = internal ? last_change : last_receive;
    last = std::max(last, index);
    m_hd_account_keys[std::make_tuple(account, internal, static_cast<uint32_t>(index))] = pubkey.GetID();
    const auto where = std::make_tuple(account, internal, static_cast<uint32_t>(index));
    m_hd_account_scripts[GetScriptForDestination(PKHash(pubkey))] = where;
    m_hd_account_scripts[GetScriptForRawPubKey(pubkey)] = where;
    if (pubkey.IsCompressed()) {
        const CScript witprog = GetScriptForDestination(WitnessV0KeyHash(pubkey.GetID()));
        m_hd_account_scripts[witprog] = where;
        m_hd_account_scripts[GetScriptForDestination(ScriptHash(witprog))] = where;
    }
}

bool LegacyScriptPubKeyMan::HDAccountsEnabled() const
{
    return m_storage.IsWalletFlagSet(WALLET_FLAG_HD_ACCOUNTS);
}

bool LegacyScriptPubKeyMan::ExtendHDAccountChain(WalletBatch& batch, uint32_t account, bool internal, int64_t last)
{
    AssertLockHeld(cs_KeyStore);
    // An account a scan holds in memory only, made the wallet's by a payment
    // whose commit failed or by an account handed out during the scan: its
    // keys are written first, or the end of the scan would take back those
    // the keys written after them follow.
    if (m_hd_discovery_accounts.count(account)) {
        if (!WriteHDDiscoveryKeys(batch, {account})) return false;
        KeepHDDiscoveryAccounts({account});
    }
    auto& extent = m_hd_account_extent.emplace(account, std::make_pair(int64_t{-1}, int64_t{-1})).first->second;
    for (int64_t index = (internal ? extent.second : extent.first) + 1; index <= last; ++index) {
        CKey key;
        CKeyMetadata metadata;
        if (!DeriveHDAccountKey(account, internal, static_cast<uint32_t>(index), key, metadata)) return false;
        if (!AddHDAccountKey(batch, key, metadata)) return false;
    }
    return true;
}

bool LegacyScriptPubKeyMan::SetupHDAccounts()
{
    m_storage.AssertWalletLockHeld();
    LOCK(cs_KeyStore);
    if (HDAccountsEnabled()) return true;
    if (!IsHDEnabled() || m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) || !m_storage.CanSupportFeature(FEATURE_HD_SPLIT)) return false;
    // A restore looking for the accounts marks the wallet itself if it finds one.
    if (!m_hd_discovery_accounts.empty()) return false;
    if (!CanDeriveFrom(m_hd_chain)) return false;
    {
        // Derived before anything is written: a seed that cannot be read
        // leaves the wallet as it was.
        CKey probe;
        CKeyMetadata probe_meta;
        if (!DeriveHDAccountKey(1, false, 0, probe, probe_meta)) return false;
    }

    WalletBatch batch(m_storage.GetDatabase());
    if (!batch.TxnBegin()) return false;
    CHDAccountMeta meta{m_hd_account_meta.value_or(CHDAccountMeta{})};
    meta.highest_derived = std::max(meta.highest_derived, uint32_t{1});
    CHDAccount pool;
    pool.state = CHDAccount::State::POOL;
    pool.nCreateTime = GetTime();
    // What is in memory before, for a transaction that does not commit.
    const std::map<uint32_t, CHDAccount> accounts_before{m_hd_accounts};
    const std::optional<CHDAccountMeta> meta_before{m_hd_account_meta};
    const auto keys_before{m_hd_account_keys};
    const auto extent_before{m_hd_account_extent};
    const bool blank_before{m_storage.IsWalletFlagSet(WALLET_FLAG_BLANK_WALLET)};
    bool ok{true};
    if (!m_hd_accounts.count(1)) ok = WriteHDAccount(batch, 1, pool);
    ok = ok && WriteHDAccountMeta(batch, meta);
    ok = ok && ExtendHDAccountChain(batch, 1, false, HD_ACCOUNT_KEY_LOOKAHEAD - 1) && ExtendHDAccountChain(batch, 1, true, HD_ACCOUNT_KEY_LOOKAHEAD - 1);
    // The flags last: adding a key writes the flags as they are in memory,
    // which have this one only once the transaction is committed. The
    // version is written here, and set in memory only once committed.
    ok = ok && m_storage.SetWalletFlagWithDB(batch, WALLET_FLAG_HD_ACCOUNTS);
    ok = ok && batch.WriteMinVersion(FEATURE_HD_ACCOUNTS);
    if (!ok || !batch.TxnCommit()) {
        batch.TxnAbort();
        // The keys added are forgotten, so that a later attempt derives and
        // writes them again.
        for (const auto& [where, key_id] : m_hd_account_keys) {
            if (keys_before.count(where)) continue;
            CPubKey pubkey;
            if (GetPubKey(key_id, pubkey)) {
                const CScript witprog{GetScriptForDestination(WitnessV0KeyHash(key_id))};
                mapScripts.erase(CScriptID(witprog));
                for (const CScript& script : {GetScriptForDestination(PKHash(pubkey)), GetScriptForRawPubKey(pubkey), witprog, GetScriptForDestination(ScriptHash(witprog))}) {
                    m_hd_account_scripts.erase(script);
                }
            }
            mapKeys.erase(key_id);
            mapCryptedKeys.erase(key_id);
            mapKeyMetadata.erase(key_id);
        }
        m_hd_account_keys = keys_before;
        m_hd_account_extent = extent_before;
        m_hd_accounts = accounts_before;
        m_hd_account_meta = meta_before;
        if (blank_before) m_storage.ConfirmWalletFlag(WALLET_FLAG_BLANK_WALLET);
        return false;
    }
    m_storage.ConfirmWalletFlag(WALLET_FLAG_HD_ACCOUNTS);
    m_storage.SetMinVersion(FEATURE_HD_ACCOUNTS);
    WalletLogPrintf("The wallet now holds HD accounts; account 1 is kept ahead of use\n");
    return true;
}

bool LegacyScriptPubKeyMan::TopUpHDAccounts()
{
    m_storage.AssertWalletLockHeld();
    LOCK(cs_KeyStore);
    if (!HDAccountsEnabled()) return true;
    CHDAccountMeta meta{m_hd_account_meta.value_or(CHDAccountMeta{})};
    // BIP44's account rule: one account is kept ahead of the highest used.
    // An account handed out beyond it, with force, is kept too.
    uint32_t highest{meta.highest_used + 1};
    for (const auto& [account, record] : m_hd_accounts) {
        if (record.state != CHDAccount::State::POOL) highest = std::max(highest, account);
    }
    if (!CanDeriveFrom(m_hd_chain) || m_storage.IsLocked()) {
        // Nothing is owed when the keys are already there, as when a locked
        // wallet sees again a payment it had seen while unlocked.
        m_hd_accounts_topup_owed = !HDAccountsLookaheadHeld(highest);
        return !m_hd_accounts_topup_owed;
    }
    WalletBatch batch(m_storage.GetDatabase());
    for (uint32_t account = 1; account <= highest; ++account) {
        auto it = m_hd_accounts.find(account);
        if (it == m_hd_accounts.end()) {
            CHDAccount pool;
            pool.state = CHDAccount::State::POOL;
            pool.nCreateTime = GetTime();
            if (!WriteHDAccount(batch, account, pool)) return false;
            it = m_hd_accounts.find(account);
        }
        // A retired account gives no new receive address, but its coins are
        // still spent, and the change of each payment goes back to it.
        const bool retired{it->second.state == CHDAccount::State::RETIRED};
        const CHDAccount record{it->second};
        if ((!retired && !ExtendHDAccountChain(batch, account, false, int64_t{record.next_receive} + HD_ACCOUNT_KEY_LOOKAHEAD - 1)) ||
            !ExtendHDAccountChain(batch, account, true, int64_t{record.next_change} + HD_ACCOUNT_KEY_LOOKAHEAD - 1)) {
            m_hd_accounts_topup_owed = true;
            return false;
        }
    }
    if (highest > meta.highest_derived || !m_hd_account_meta) {
        meta.highest_derived = std::max(meta.highest_derived, highest);
        if (!WriteHDAccountMeta(batch, meta)) return false;
    }
    m_hd_accounts_topup_owed = false;
    return true;
}

bool LegacyScriptPubKeyMan::HDAccountsLookaheadHeld(uint32_t highest) const
{
    AssertLockHeld(cs_KeyStore);
    if (!m_hd_account_meta || highest > m_hd_account_meta->highest_derived) return false;
    for (uint32_t account = 1; account <= highest; ++account) {
        const auto it = m_hd_accounts.find(account);
        if (it == m_hd_accounts.end()) return false;
        const auto extent = m_hd_account_extent.find(account);
        if (extent == m_hd_account_extent.end()) return false;
        const bool retired{it->second.state == CHDAccount::State::RETIRED};
        if (!retired && extent->second.first < int64_t{it->second.next_receive} + HD_ACCOUNT_KEY_LOOKAHEAD - 1) return false;
        if (extent->second.second < int64_t{it->second.next_change} + HD_ACCOUNT_KEY_LOOKAHEAD - 1) return false;
    }
    return true;
}

bool LegacyScriptPubKeyMan::IsHDAccountsTopUpOwed() const
{
    LOCK(cs_KeyStore);
    return m_hd_accounts_topup_owed;
}

void LegacyScriptPubKeyMan::MarkHDAccountKeyUsed(const CKeyMetadata& metadata)
{
    AssertLockHeld(cs_KeyStore);
    const std::vector<uint32_t>& path = metadata.key_origin.path;
    const uint32_t account{HDAccountOfKeyPath(path)};
    const bool internal{(path.size() == 5 ? path[3] : path[1] & ~BIP32_HARDENED_KEY_LIMIT) != 0};
    const uint32_t index{path.size() == 5 ? path[4] : path[2] & ~BIP32_HARDENED_KEY_LIMIT};
    CHDAccount record{m_hd_accounts.count(account) ? m_hd_accounts.at(account) : CHDAccount{}};
    const CHDAccount before{record};
    if (record.state == CHDAccount::State::POOL) record.state = CHDAccount::State::ACTIVE;
    uint32_t& next = internal ? record.next_change : record.next_receive;
    next = std::max(next, index + 1);
    CHDAccountMeta meta{m_hd_account_meta.value_or(CHDAccountMeta{})};
    const CHDAccountMeta meta_before{meta};
    meta.highest_used = std::max(meta.highest_used, account);
    meta.highest_derived = std::max(meta.highest_derived, account);
    WalletBatch batch(m_storage.GetDatabase());
    if (!m_hd_accounts.count(account) || !(record == before)) WriteHDAccount(batch, account, record);
    if (!m_hd_account_meta || !(meta == meta_before)) WriteHDAccountMeta(batch, meta);
    if (!TopUpHDAccounts()) {
        WalletLogPrintf("%s: HD account %u was paid while its seed cannot be read; the account after it is derived at the next unlock\n", __func__, account);
    }
}

uint32_t LegacyScriptPubKeyMan::GetHDAccountOfScript(const CScript& script) const
{
    LOCK(cs_KeyStore);
    const auto it = m_hd_account_scripts.find(script);
    return it == m_hd_account_scripts.end() ? 0 : std::get<0>(it->second);
}

bool LegacyScriptPubKeyMan::GetHDAccountKeyOfScript(const CScript& script, uint32_t& account, bool& internal, uint32_t& index) const
{
    LOCK(cs_KeyStore);
    const auto it = m_hd_account_scripts.find(script);
    if (it == m_hd_account_scripts.end()) return false;
    std::tie(account, internal, index) = it->second;
    return true;
}

bool LegacyScriptPubKeyMan::UpdateHDAccount(uint32_t account, const CHDAccount& record)
{
    LOCK(cs_KeyStore);
    WalletBatch batch(m_storage.GetDatabase());
    return WriteHDAccount(batch, account, record);
}

std::pair<int64_t, int64_t> LegacyScriptPubKeyMan::GetHDAccountExtent(uint32_t account) const
{
    LOCK(cs_KeyStore);
    const auto it = m_hd_account_extent.find(account);
    return it == m_hd_account_extent.end() ? std::make_pair(int64_t{-1}, int64_t{-1}) : it->second;
}

bool LegacyScriptPubKeyMan::HasHDAccountKeys() const
{
    LOCK(cs_KeyStore);
    return !m_hd_account_scripts.empty();
}

bool LegacyScriptPubKeyMan::GetHDAccountPubKey(uint32_t account, bool internal, uint32_t index, CPubKey& pubkey) const
{
    LOCK(cs_KeyStore);
    const auto it = m_hd_account_keys.find(std::make_tuple(account, internal, index));
    return it != m_hd_account_keys.end() && GetPubKey(it->second, pubkey);
}

bool LegacyScriptPubKeyMan::HandOutHDAccountKey(uint32_t account, bool internal, uint32_t index)
{
    m_storage.AssertWalletLockHeld();
    LOCK(cs_KeyStore);
    auto it = m_hd_accounts.find(account);
    if (it == m_hd_accounts.end()) return false;
    if (index >= (internal ? it->second.next_change : it->second.next_receive)) {
        CHDAccount record{it->second};
        (internal ? record.next_change : record.next_receive) = index + 1;
        WalletBatch batch(m_storage.GetDatabase());
        if (!WriteHDAccount(batch, account, record)) return false;
    }
    // Owed at the next unlock when the wallet is locked.
    TopUpHDAccounts();
    return true;
}

bool LegacyScriptPubKeyMan::HasHDAccountsInconsistency() const
{
    LOCK(cs_KeyStore);
    return m_hd_accounts_inconsistent;
}

bool LegacyScriptPubKeyMan::KeepHDAccountInMemory(uint32_t account)
{
    AssertLockHeld(cs_KeyStore);
    // Held already: in memory, or written.
    if (m_hd_discovery_accounts.count(account) || m_hd_account_extent.count(account)) return true;
    std::vector<DiscoveryKey> keys;
    for (const bool internal : {false, true}) {
        for (uint32_t index = 0; index < HD_ACCOUNT_KEY_LOOKAHEAD; ++index) {
            DiscoveryKey entry;
            if (!DeriveHDAccountKey(account, internal, index, entry.key, entry.metadata)) return false;
            entry.pubkey = entry.key.GetPubKey();
            if (m_storage.HasEncryptionKeys()) {
                const CKeyingMaterial secret(entry.key.begin(), entry.key.end());
                if (!EncryptSecret(m_storage.GetEncryptionKey(), secret, entry.pubkey.GetHash(), entry.crypted)) return false;
            }
            keys.push_back(std::move(entry));
        }
    }
    // Into the key store, as the loader adds keys: nothing is written.
    for (const DiscoveryKey& entry : keys) {
        if (HaveKey(entry.pubkey.GetID())) continue;
        const bool added{m_storage.HasEncryptionKeys() ? AddCryptedKeyInner(entry.pubkey, entry.crypted) : FillableSigningProvider::AddKeyPubKey(entry.key, entry.pubkey)};
        if (!added) return false;
        mapKeyMetadata[entry.pubkey.GetID()] = entry.metadata;
        if (entry.pubkey.IsCompressed()) FillableSigningProvider::AddCScript(GetScriptForDestination(WitnessV0KeyHash(entry.pubkey.GetID())));
        IndexHDAccountKey(entry.pubkey, entry.metadata);
        m_hd_discovery_keys.push_back(entry);
    }
    m_hd_discovery_accounts.insert(account);
    return true;
}

bool LegacyScriptPubKeyMan::BeginHDAccountDiscovery(uint32_t count)
{
    LOCK(cs_KeyStore);
    if (!m_hd_discovery_accounts.empty()) return false;
    if (!IsHDEnabled() || m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) || !m_storage.CanSupportFeature(FEATURE_HD_SPLIT)) return false;
    if (!CanDeriveFrom(m_hd_chain)) return false;
    m_hd_discovery_count = std::max(count, uint32_t{1});
    // BIP44's account discovery: the accounts past the highest used. A
    // wallet that holds accounts keeps the first of them written already.
    const uint32_t highest_used{HDAccountsEnabled() && m_hd_account_meta ? m_hd_account_meta->highest_used : 0};
    for (uint32_t account = 1; account <= highest_used + m_hd_discovery_count; ++account) {
        if (!KeepHDAccountInMemory(account)) {
            EndHDAccountDiscovery();
            return false;
        }
    }
    if (!m_hd_discovery_accounts.empty()) {
        WalletLogPrintf("Looking for HD accounts: accounts %u to %u are kept ahead in memory\n", *m_hd_discovery_accounts.begin(), *m_hd_discovery_accounts.rbegin());
    }
    return true;
}

bool LegacyScriptPubKeyMan::IsHDAccountDiscoveryPending() const
{
    LOCK(cs_KeyStore);
    return !m_hd_discovery_accounts.empty();
}

bool LegacyScriptPubKeyMan::CommitHDAccountDiscovery(uint32_t paid)
{
    m_storage.AssertWalletLockHeld();
    LOCK(cs_KeyStore);
    if (m_hd_discovery_accounts.empty() || paid == 0) return true;
    // The accounts past the one paid are looked at as well: kept in memory
    // first, so that the one after it is written below and not taken as
    // held by the look-ahead that follows the payment.
    for (uint32_t account = paid + 1; account <= paid + m_hd_discovery_count; ++account) {
        if (!KeepHDAccountInMemory(account)) {
            WalletLogPrintf("%s: HD account %u cannot be derived now; it is derived at the next unlock\n", __func__, account);
            break;
        }
    }
    std::set<uint32_t> written;
    for (const uint32_t account : m_hd_discovery_accounts) {
        if (account <= paid + 1) written.insert(account);
    }
    if (written.empty()) return true;

    WalletBatch batch(m_storage.GetDatabase());
    if (!batch.TxnBegin()) return false;
    bool ok{WriteHDDiscoveryKeys(batch, written)};
    CHDAccount pool;
    pool.state = CHDAccount::State::POOL;
    pool.nCreateTime = GetTime();
    for (const uint32_t account : written) {
        if (!m_hd_accounts.count(account)) ok = ok && batch.WriteHDAccount(account, pool);
    }
    CHDAccountMeta meta{m_hd_account_meta.value_or(CHDAccountMeta{})};
    meta.highest_derived = std::max(meta.highest_derived, *written.rbegin());
    ok = ok && batch.WriteHDAccountMeta(meta);
    const bool mark{!HDAccountsEnabled()};
    if (mark) {
        ok = ok && m_storage.SetWalletFlagWithDB(batch, WALLET_FLAG_HD_ACCOUNTS);
        // Set in memory only once committed.
        ok = ok && batch.WriteMinVersion(FEATURE_HD_ACCOUNTS);
    }
    if (!ok || !batch.TxnCommit()) {
        batch.TxnAbort();
        WalletLogPrintf("%s: the HD accounts found could not be written to the wallet; they are looked for again at the next scan\n", __func__);
        return false;
    }
    for (const uint32_t account : written) {
        if (!m_hd_accounts.count(account)) m_hd_accounts[account] = pool;
    }
    KeepHDDiscoveryAccounts(written);
    m_hd_account_meta = meta;
    if (mark) {
        m_storage.ConfirmWalletFlag(WALLET_FLAG_HD_ACCOUNTS);
        m_storage.SetMinVersion(FEATURE_HD_ACCOUNTS);
        WalletLogPrintf("The wallet holds HD accounts: an account above 0 was found used\n");
    }
    return true;
}

bool LegacyScriptPubKeyMan::WriteHDDiscoveryKeys(WalletBatch& batch, const std::set<uint32_t>& accounts)
{
    AssertLockHeld(cs_KeyStore);
    for (const DiscoveryKey& entry : m_hd_discovery_keys) {
        if (!accounts.count(HDAccountOfKeyPath(entry.metadata.key_origin.path))) continue;
        if (m_storage.HasEncryptionKeys()) {
            if (entry.crypted.empty() || !batch.WriteCryptedKey(entry.pubkey, entry.crypted, entry.metadata)) return false;
        } else {
            if (!batch.WriteKey(entry.pubkey, entry.key.GetPrivKey(), entry.metadata)) return false;
        }
        if (entry.pubkey.IsCompressed()) {
            const CScript witprog{GetScriptForDestination(WitnessV0KeyHash(entry.pubkey.GetID()))};
            if (!batch.WriteCScript(Hash160(witprog), witprog)) return false;
        }
    }
    return true;
}

void LegacyScriptPubKeyMan::KeepHDDiscoveryAccounts(const std::set<uint32_t>& accounts)
{
    AssertLockHeld(cs_KeyStore);
    std::vector<DiscoveryKey> kept;
    for (DiscoveryKey& entry : m_hd_discovery_keys) {
        if (accounts.count(HDAccountOfKeyPath(entry.metadata.key_origin.path))) {
            UpdateTimeFirstKey(entry.metadata.nCreateTime);
        } else {
            kept.push_back(std::move(entry));
        }
    }
    m_hd_discovery_keys = std::move(kept);
    for (const uint32_t account : accounts) m_hd_discovery_accounts.erase(account);
}

void LegacyScriptPubKeyMan::EndHDAccountDiscovery()
{
    LOCK(cs_KeyStore);
    if (m_hd_discovery_accounts.empty()) return;
    for (const DiscoveryKey& entry : m_hd_discovery_keys) {
        const CKeyID id{entry.pubkey.GetID()};
        mapKeys.erase(id);
        mapCryptedKeys.erase(id);
        mapKeyMetadata.erase(id);
        const CScript witprog{GetScriptForDestination(WitnessV0KeyHash(id))};
        mapScripts.erase(CScriptID(witprog));
        for (const CScript& script : {GetScriptForDestination(PKHash(entry.pubkey)), GetScriptForRawPubKey(entry.pubkey), witprog, GetScriptForDestination(ScriptHash(witprog))}) {
            m_hd_account_scripts.erase(script);
        }
        const std::vector<uint32_t>& path = entry.metadata.key_origin.path;
        const bool internal{(path.size() == 5 ? path[3] : path[1] & ~BIP32_HARDENED_KEY_LIMIT) != 0};
        const uint32_t index{path.size() == 5 ? path[4] : path[2] & ~BIP32_HARDENED_KEY_LIMIT};
        m_hd_account_keys.erase(std::make_tuple(HDAccountOfKeyPath(path), internal, index));
    }
    for (const uint32_t account : m_hd_discovery_accounts) m_hd_account_extent.erase(account);
    m_hd_discovery_keys.clear();
    m_hd_discovery_accounts.clear();
    if (HDAccountsEnabled()) {
        WalletLogPrintf("The HD accounts looked at past the highest used were not found used, and are not kept\n");
    } else {
        WalletLogPrintf("No HD account was found used: the wallet holds account 0 only\n");
    }
}

bool LegacyScriptPubKeyMan::KeepsHDSeed() const
{
    LOCK(cs_KeyStore);
    // Flagged; or holding keys of accounts the flag is still to cover, as an
    // earlier wallet tool's salvage leaves them; or a scan holding accounts
    // in memory: with another seed they would be keys of account 0.
    return HDAccountsEnabled() || m_hd_accounts_inconsistent || m_hd_account_repair_owed || !m_hd_discovery_accounts.empty();
}

bool LegacyScriptPubKeyMan::HoldsInactiveSeedKeys() const
{
    LOCK(cs_KeyStore);
    for (const auto& entry : mapKeyMetadata) {
        const auto& meta = entry.second;
        if (!meta.hd_seed_id.IsNull() && meta.hd_seed_id != m_hd_chain.seed_id) return true;
    }
    return false;
}

bool LegacyScriptPubKeyMan::IsHDAccountRepairOwed() const
{
    LOCK(cs_KeyStore);
    return m_hd_account_repair_owed;
}

bool LegacyScriptPubKeyMan::RepairHDAccountKeys(std::vector<uint32_t>& found)
{
    m_storage.AssertWalletLockHeld();
    LOCK(cs_KeyStore);
    found.clear();
    if (!m_hd_account_repair_owed) return true;
    if (!IsHDEnabled() || m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        m_hd_account_repair_owed = false;
        return true;
    }
    // Not while a scan keeps keys in memory: they would be taken as the wallet's.
    if (!m_hd_discovery_accounts.empty() || !CanDeriveFrom(m_hd_chain) || m_storage.IsLocked()) return false;
    SecureVector seed_bytes;
    if (!GetHDSeed(m_hd_chain, seed_bytes)) return false;
    CExtKey master;
    master.SetSeed(seed_bytes.data(), seed_bytes.size());
    const CKeyID master_id{master.key.GetPubKey().GetID()};

    struct Match {
        CPubKey pubkey;
        CKeyMetadata metadata;
        bool lost;
    };
    std::vector<Match> matches;
    // The accounts of a wallet follow each other: one with no key in the
    // wallet ends them. On each chain the keys run from index 0, and a gap
    // as long as the look-ahead ends the chain.
    for (uint32_t account = 1;; ++account) {
        bool held{false};
        for (const bool internal : {false, true}) {
            uint32_t misses{0};
            for (uint32_t index = 0; misses < HD_ACCOUNT_KEY_LOOKAHEAD; ++index) {
                const HDAccountKey derived{DeriveHDAccountKeyFromMaster(master, m_hd_chain.IsBip44(), Params().ExtCoinType(), account, internal, index)};
                const CPubKey pubkey{derived.key.GetPubKey()};
                if (!HaveKey(pubkey.GetID())) {
                    ++misses;
                    continue;
                }
                misses = 0;
                held = true;
                Match match{pubkey, {}, false};
                const auto it = mapKeyMetadata.find(pubkey.GetID());
                if (it != mapKeyMetadata.end() && it->second.has_key_origin && it->second.hd_seed_id == m_hd_chain.seed_id) {
                    match.metadata = it->second;
                } else {
                    // When the key was made is not known.
                    match.metadata = CKeyMetadata(1);
                    match.metadata.hdKeypath = derived.path_text;
                    match.metadata.key_origin.path = derived.path;
                    match.metadata.hd_seed_id = m_hd_chain.seed_id;
                    std::copy(master_id.begin(), master_id.begin() + 4, match.metadata.key_origin.fingerprint);
                    match.metadata.has_key_origin = true;
                    match.lost = true;
                }
                matches.push_back(std::move(match));
            }
        }
        if (!held) break;
        found.push_back(account);
    }
    // The other keys without metadata are of no account above 0: given the
    // metadata of a key whose creation time is not known, so that the wallet
    // is not asked again at its next load.
    std::set<CKeyID> matched;
    for (const Match& match : matches) matched.insert(match.pubkey.GetID());
    std::vector<CPubKey> unknown;
    const auto collect = [&](const CKeyID& key_id) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore) {
        CPubKey pubkey;
        if (!mapKeyMetadata.count(key_id) && !matched.count(key_id) && GetPubKey(key_id, pubkey)) unknown.push_back(pubkey);
    };
    for (const auto& entry : mapKeys) collect(entry.first);
    for (const auto& entry : mapCryptedKeys) collect(entry.first);
    if (found.empty() && unknown.empty()) {
        m_hd_account_repair_owed = false;
        return true;
    }

    WalletBatch batch(m_storage.GetDatabase());
    if (!batch.TxnBegin()) return false;
    bool ok{true};
    const CKeyMetadata unknown_meta{1};
    for (const CPubKey& pubkey : unknown) ok = ok && batch.WriteKeyMetadata(unknown_meta, pubkey, /* overwrite */ true);
    // The witness script of each key is learned with the key when it is
    // loaded: only the metadata is to be put back.
    for (const Match& match : matches) {
        if (match.lost) ok = ok && batch.WriteKeyMetadata(match.metadata, match.pubkey, /* overwrite */ true);
    }
    CHDAccount pool;
    pool.state = CHDAccount::State::POOL;
    pool.nCreateTime = GetTime();
    std::vector<uint32_t> new_records;
    CHDAccountMeta meta{m_hd_account_meta.value_or(CHDAccountMeta{})};
    const bool mark{!found.empty() && !HDAccountsEnabled()};
    if (!found.empty()) {
        for (const uint32_t account : found) {
            if (m_hd_accounts.count(account)) continue;
            ok = ok && batch.WriteHDAccount(account, pool);
            new_records.push_back(account);
        }
        meta.highest_derived = std::max(meta.highest_derived, found.back());
        ok = ok && batch.WriteHDAccountMeta(meta);
    }
    if (mark) {
        ok = ok && m_storage.SetWalletFlagWithDB(batch, WALLET_FLAG_HD_ACCOUNTS);
        ok = ok && batch.WriteMinVersion(FEATURE_HD_ACCOUNTS);
    }
    if (!ok || !batch.TxnCommit()) {
        batch.TxnAbort();
        WalletLogPrintf("%s: the keys of HD accounts found could not be written to the wallet; tried again at the next load\n", __func__);
        m_hd_account_repair_owed = false;
        return false;
    }
    for (const CPubKey& pubkey : unknown) {
        mapKeyMetadata[pubkey.GetID()] = unknown_meta;
        UpdateTimeFirstKey(unknown_meta.nCreateTime);
    }
    m_hd_account_repair_owed = false;
    if (found.empty()) {
        WalletLogPrintf("No keys of HD accounts were found in the wallet; %u keys without metadata were given it\n", unknown.size());
        return true;
    }
    size_t lost{0};
    for (const Match& match : matches) {
        if (match.lost) {
            mapKeyMetadata[match.pubkey.GetID()] = match.metadata;
            UpdateTimeFirstKey(match.metadata.nCreateTime);
            ++lost;
        }
        IndexHDAccountKey(match.pubkey, match.metadata);
    }
    for (const uint32_t account : new_records) m_hd_accounts[account] = pool;
    m_hd_account_meta = meta;
    if (mark) {
        m_storage.ConfirmWalletFlag(WALLET_FLAG_HD_ACCOUNTS);
        m_storage.SetMinVersion(FEATURE_HD_ACCOUNTS);
    }
    m_hd_accounts_inconsistent = false;
    WalletLogPrintf("The keys of %u HD accounts were found in the wallet (the metadata of %u keys put back from the seed)%s\n",
        found.size(), lost, mark ? "; the wallet is marked as holding HD accounts" : "");
    return true;
}

void LegacyScriptPubKeyMan::DecideHDAccounts()
{
    LOCK(cs_KeyStore);
    m_hd_account_extent.clear();
    m_hd_account_scripts.clear();
    m_hd_account_keys.clear();
    for (const auto& [key_id, metadata] : mapKeyMetadata) {
        CPubKey pubkey;
        if (!GetPubKey(key_id, pubkey)) continue;
        IndexHDAccountKey(pubkey, metadata);
    }
    m_hd_accounts_inconsistent = !HDAccountsEnabled() && !m_hd_account_extent.empty();
    if (m_hd_accounts_inconsistent) {
        WalletLogPrintf("The wallet holds keys of %u HD accounts but not the flag that says it holds accounts, as an earlier wallet tool's salvage leaves it\n", m_hd_account_extent.size());
    }
    // An earlier wallet tool's salvage keeps the keys and drops their
    // metadata, the flags and the version: keys of accounts then look like
    // keys of account 0. Matched against the seed's accounts on the
    // wallet's worker, once the seed can be read.
    m_hd_account_repair_owed = false;
    if (!HDAccountsEnabled() && IsHDEnabled() && !m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        m_hd_account_repair_owed = m_hd_accounts_inconsistent;
        for (const auto& entry : mapKeys) {
            const auto& key_id = entry.first;
            if (m_hd_account_repair_owed) break;
            m_hd_account_repair_owed = !mapKeyMetadata.count(key_id);
        }
        for (const auto& entry : mapCryptedKeys) {
            const auto& key_id = entry.first;
            if (m_hd_account_repair_owed) break;
            m_hd_account_repair_owed = !mapKeyMetadata.count(key_id);
        }
    }
}

void LegacyScriptPubKeyMan::LoadHDAccount(uint32_t account, const CHDAccount& record)
{
    LOCK(cs_KeyStore);
    m_hd_accounts[account] = record;
}

void LegacyScriptPubKeyMan::LoadHDAccountMeta(const CHDAccountMeta& meta)
{
    LOCK(cs_KeyStore);
    m_hd_account_meta = meta;
}

std::map<uint32_t, CHDAccount> LegacyScriptPubKeyMan::GetHDAccounts() const
{
    LOCK(cs_KeyStore);
    return m_hd_accounts;
}

std::optional<CHDAccountMeta> LegacyScriptPubKeyMan::GetHDAccountMeta() const
{
    LOCK(cs_KeyStore);
    return m_hd_account_meta;
}

bool LegacyScriptPubKeyMan::WriteHDAccount(WalletBatch& batch, uint32_t account, const CHDAccount& record)
{
    LOCK(cs_KeyStore);
    if (!batch.WriteHDAccount(account, record)) return false;
    m_hd_accounts[account] = record;
    return true;
}

bool LegacyScriptPubKeyMan::WriteHDAccountMeta(WalletBatch& batch, const CHDAccountMeta& meta)
{
    LOCK(cs_KeyStore);
    if (!batch.WriteHDAccountMeta(meta)) return false;
    m_hd_account_meta = meta;
    return true;
}

std::pair<std::vector<uint32_t>, bool> LegacyScriptPubKeyMan::RebuildHDAccounts(const std::map<std::pair<CKeyID, uint32_t>, std::pair<int64_t, int64_t>>& account_keys)
{
    LOCK(cs_KeyStore);
    std::vector<uint32_t> rebuilt;
    if (m_hd_chain.seed_id.IsNull()) return {rebuilt, false};
    uint32_t highest{0};
    for (const auto& [seed_account, last] : account_keys) {
        const auto& [seed_id, account] = seed_account;
        if (seed_id != m_hd_chain.seed_id) continue;
        highest = std::max(highest, account);
        if (m_hd_accounts.count(account)) continue;
        CHDAccount record;
        record.state = CHDAccount::State::ACTIVE;
        record.nCreateTime = GetTime();
        // The keys of a chain run HD_ACCOUNT_KEY_LOOKAHEAD past its next
        // index, which is where the gap of a new address is measured from.
        record.next_receive = static_cast<uint32_t>(std::max<int64_t>(last.first + 1 - HD_ACCOUNT_KEY_LOOKAHEAD, 0));
        record.next_change = static_cast<uint32_t>(std::max<int64_t>(last.second + 1 - HD_ACCOUNT_KEY_LOOKAHEAD, 0));
        m_hd_accounts[account] = record;
        rebuilt.push_back(account);
    }
    if (highest == 0) return {rebuilt, false};
    CHDAccountMeta meta{m_hd_account_meta.value_or(CHDAccountMeta{})};
    const CHDAccountMeta before{meta};
    meta.highest_derived = std::max(meta.highest_derived, highest);
    // An account put together from its keys may have been used: the accounts
    // kept ahead of use reach past it.
    for (const uint32_t account : rebuilt) meta.highest_used = std::max(meta.highest_used, account);
    const bool meta_changed{!m_hd_account_meta || !(meta == before)};
    m_hd_account_meta = meta;
    return {rebuilt, meta_changed};
}

void LegacyScriptPubKeyMan::DeriveNewChildKey(WalletBatch &batch, CKeyMetadata& metadata, CKey& secret, CHDChain& hd_chain, bool internal)
{
    // for now we use a fixed keypath scheme of m/0'/0'/k
    CExtKey masterKey;             //hd master key

    // The callers have asked whether the seed is available. This is the last resort.
    SecureVector seed_bytes;
    if (!GetHDSeed(hd_chain, seed_bytes))
        throw std::runtime_error(std::string(__func__) + ": seed not found");
    masterKey.SetSeed(seed_bytes.data(), seed_bytes.size());

    // The keys every wallet has always had are those of account 0.
    const uint32_t nAccountIndex = 0;

    uint32_t& nChildIndex = internal ? hd_chain.nInternalChainCounter : hd_chain.nExternalChainCounter;

    assert(internal ? m_storage.CanSupportFeature(FEATURE_HD_SPLIT) : true);
    HDAccountKey derived;
    do {
        // An index that is already taken is passed over, and the next one
        // tried: the path is that of the key that is kept, not of every try.
        derived = DeriveHDAccountKeyFromMaster(masterKey, hd_chain.IsBip44(), Params().ExtCoinType(), nAccountIndex, internal, nChildIndex);
        metadata.hdKeypath = derived.path_text;
        metadata.key_origin.path = derived.path;

        // increment childkey index
        nChildIndex++;

    } while (HaveKey(derived.key.GetPubKey().GetID()));
    secret = derived.key;
    metadata.hd_seed_id = hd_chain.seed_id;
    CKeyID master_id = masterKey.key.GetPubKey().GetID();
    std::copy(master_id.begin(), master_id.begin() + 4, metadata.key_origin.fingerprint);
    metadata.has_key_origin = true;
    // update the chain model in the database
    // The active chain is this object. Its seed id says nothing: every chain
    // made from a mnemonic has the same one.
    if (&hd_chain == &m_hd_chain && !WriteHDChain(batch, hd_chain))
        throw std::runtime_error(std::string(__func__) + ": writing HD chain model failed");
}

void LegacyScriptPubKeyMan::LoadKeyPool(int64_t nIndex, const CKeyPool &keypool)
{
    LOCK(cs_KeyStore);
    if (keypool.m_pre_split) {
        set_pre_split_keypool.insert(nIndex);
    } else if (keypool.fInternal) {
        setInternalKeyPool.insert(nIndex);
    } else {
        setExternalKeyPool.insert(nIndex);
    }
    m_max_keypool_index = std::max(m_max_keypool_index, nIndex);
    m_pool_key_to_index[keypool.vchPubKey.GetID()] = nIndex;

    // If no metadata exists yet, create a default with the pool key's
    // creation time. Note that this may be overwritten by actually
    // stored metadata for that key later, which is fine.
    CKeyID keyid = keypool.vchPubKey.GetID();
    if (mapKeyMetadata.count(keyid) == 0)
        mapKeyMetadata[keyid] = CKeyMetadata(keypool.nTime);
}

bool LegacyScriptPubKeyMan::CanGenerateKeys() const
{
    // A wallet can generate keys if it has an HD seed (IsHDEnabled) or it is a non-HD wallet (pre FEATURE_HD)
    LOCK(cs_KeyStore);
    return IsHDEnabled() || !m_storage.CanSupportFeature(FEATURE_HD);
}

CPubKey LegacyScriptPubKeyMan::GenerateNewSeed()
{
    assert(!m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS));
    CKey key;
    key.MakeNewKey(true);
    return DeriveNewSeed(key);
}

CPubKey LegacyScriptPubKeyMan::GenerateNewSeed(const WalletOptions& walletoptions)
{
    assert(!m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS));

    const SecureString& masterkey = walletoptions.ssMasterKey;
    CKey key;

    if (!masterkey.empty()) {
        key = DecodeSecret(masterkey.c_str());
        if (!key.IsValid()) {
            throw std::runtime_error(std::string(__func__) + ": Invalid private key");
        }
    } else {
        key.MakeNewKey(true);
    }


    return DeriveNewSeed(key);
}

CPubKey LegacyScriptPubKeyMan::GenerateNewBip39Seed(const WalletOptions& walletoptions)
{
    assert(!m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS));

    const SecureString& seed0 = walletoptions.ssMnemonic;
    const SecureString& pass0 = walletoptions.ssMnemonicPassphrase;

    CHDChain newHdChain;
    newHdChain.SetBip44(walletoptions.walletType == walletType::bip44Wallet);

    // The seed always comes from the wallet's own mnemonic (an empty one is
    // generated). -hdseed, -mnemonic and -mnemonicpassphrase are not read: see
    // WalletInit::ParameterInteraction.
    SecureVector& vchSeed = newHdChain.vchSeed;
    if (!newHdChain.SetMnemonic(seed0, pass0, vchSeed, walletoptions.bits, walletoptions.language))
	    throw std::runtime_error(std::string(__func__) + ": SetMnemonic failed");

    CPubKey seed(vchSeed.begin(), vchSeed.end());

    newHdChain.seed_id = seed.GetID();

    LOCK(cs_KeyStore);

    newHdChain.nVersion = m_storage.CanSupportFeature(FEATURE_HD_SPLIT) ? CHDChain::VERSION_HD_BIP39 : CHDChain::VERSION_HD_BASE;
    if (m_storage.HasEncryptionKeys() && ChainHoldsSecrets(newHdChain)) {
        // The wallet is already encrypted, as one created with a passphrase
        // is by now. The secrets go straight into their encrypted record, in
        // one transaction with the flag and the chain: they are never written
        // in clear, so there is nothing to erase afterwards.
        if (m_storage.IsLocked()) throw std::runtime_error(std::string(__func__) + ": the wallet is locked");
        const CHDSecrets secrets{newHdChain.vchMnemonic, newHdChain.vchMnemonicPassphrase, newHdChain.vchSeed};
        CHDSecretsRecord record;
        if (!WriteHDSecretsEncrypted(m_storage.GetEncryptionKey(), secrets, newHdChain, /* pending */ false, record)) {
            throw std::runtime_error(std::string(__func__) + ": writing the encrypted seed failed");
        }
        if (!m_hd_chain.seed_id.IsNull()) {
            AddInactiveHDChain(m_hd_chain);
        }
        m_hd_chain = WithoutSecrets(newHdChain);
        m_hd_secrets = secrets;
        m_hd_secrets_record = record;
        m_hd_secrets_state = HDSecretsState::ENCRYPTED;
        m_storage.ConfirmWalletFlag(WALLET_FLAG_HD_SECRETS_ENCRYPTED);
        RefreshHDType();
    } else {
        AddHDChain(newHdChain);
    }
    NotifyCanGetAddressesChanged();
    WalletBatch batch(m_storage.GetDatabase());
    m_storage.UnsetBlankWalletFlag(batch);

    return seed;
}


CPubKey LegacyScriptPubKeyMan::DeriveNewSeed(const CKey& key)
{
    int64_t nCreationTime = GetTime();
    CKeyMetadata metadata(nCreationTime);

    // calculate the seed
    CPubKey seed = key.GetPubKey();
    assert(key.VerifyPubKey(seed));

    // set the hd keypath to "s" -> Seed, refers the seed to itself
    metadata.hdKeypath     = "s";
    metadata.has_key_origin = false;
    metadata.hd_seed_id = seed.GetID();

    {
        LOCK(cs_KeyStore);

        // mem store the metadata
        mapKeyMetadata[seed.GetID()] = metadata;

        // write the key&metadata to the database
        if (!AddKeyPubKey(key, seed))
            throw std::runtime_error(std::string(__func__) + ": AddKeyPubKey failed");
    }

    return seed;
}

void LegacyScriptPubKeyMan::SetHDSeed(const CPubKey& seed)
{
    LOCK(cs_KeyStore);
    // store the keyid (hash160) together with
    // the child index counter in the database
    // as a hdchain object
    CHDChain newHdChain;
    newHdChain.nVersion = m_storage.CanSupportFeature(FEATURE_HD_SPLIT) ? CHDChain::VERSION_HD_CHAIN_SPLIT : CHDChain::VERSION_HD_BASE;
    newHdChain.seed_id = seed.GetID();
    AddHDChain(newHdChain);
    NotifyCanGetAddressesChanged();
    WalletBatch batch(m_storage.GetDatabase());
    m_storage.UnsetBlankWalletFlag(batch);
}

/**
 * Mark old keypool keys as used,
 * and generate all new keys
 */
bool LegacyScriptPubKeyMan::NewKeyPool()
{
    if (m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        return false;
    }
    {
        LOCK(cs_KeyStore);
        WalletBatch batch(m_storage.GetDatabase());

        for (const int64_t nIndex : setInternalKeyPool) {
            batch.ErasePool(nIndex);
        }
        setInternalKeyPool.clear();

        for (const int64_t nIndex : setExternalKeyPool) {
            batch.ErasePool(nIndex);
        }
        setExternalKeyPool.clear();

        for (const int64_t nIndex : set_pre_split_keypool) {
            batch.ErasePool(nIndex);
        }
        set_pre_split_keypool.clear();

        m_pool_key_to_index.clear();

        if (!TopUp()) {
            return false;
        }
        WalletLogPrintf("LegacyScriptPubKeyMan::NewKeyPool rewrote keypool\n");
    }
    return true;
}

bool LegacyScriptPubKeyMan::TopUp(unsigned int kpSize)
{
    m_storage.AssertWalletLockHeld();
    if (!CanGenerateKeys()) {
        return false;
    }
    {
        LOCK(cs_KeyStore);

        if (m_storage.IsLocked()) return false;
        // Nothing to derive from: the pool stays as it is.
        if (!CanDeriveFrom(m_hd_chain)) return false;

        // Top up key pool
        unsigned int nTargetSize;
        if (kpSize > 0)
            nTargetSize = kpSize;
        else
            nTargetSize = std::max(gArgs.GetArg("-keypool", DEFAULT_KEYPOOL_SIZE), (int64_t) 0);

        // count amount of available keys (internal, external)
        // make sure the keypool of external and internal keys fits the user selected target (-keypool)
        int64_t missingExternal = std::max(std::max((int64_t) nTargetSize, (int64_t) 1) - (int64_t)setExternalKeyPool.size(), (int64_t) 0);
        int64_t missingInternal = std::max(std::max((int64_t) nTargetSize, (int64_t) 1) - (int64_t)setInternalKeyPool.size(), (int64_t) 0);

        if (!IsHDEnabled() || !m_storage.CanSupportFeature(FEATURE_HD_SPLIT))
        {
            // don't create extra internal keys
            missingInternal = 0;
        }
        bool internal = false;
        WalletBatch batch(m_storage.GetDatabase());
        for (int64_t i = missingInternal + missingExternal; i--;)
        {
            if (i < missingInternal) {
                internal = true;
            }

            CPubKey pubkey(GenerateNewKey(batch, m_hd_chain, internal));
            AddKeypoolPubkeyWithDB(pubkey, internal, batch);
        }
        if (missingInternal + missingExternal > 0) {
            WalletLogPrintf("keypool added %d keys (%d internal), size=%u (%u internal)\n", missingInternal + missingExternal, missingInternal, setInternalKeyPool.size() + setExternalKeyPool.size() + set_pre_split_keypool.size(), setInternalKeyPool.size());
        }
    }
    NotifyCanGetAddressesChanged();
    return true;
}

void LegacyScriptPubKeyMan::AddKeypoolPubkeyWithDB(const CPubKey& pubkey, const bool internal, WalletBatch& batch)
{
    LOCK(cs_KeyStore);
    assert(m_max_keypool_index < std::numeric_limits<int64_t>::max()); // How in the hell did you use so many keys?
    int64_t index = ++m_max_keypool_index;
    if (!batch.WritePool(index, CKeyPool(pubkey, internal))) {
        throw std::runtime_error(std::string(__func__) + ": writing imported pubkey failed");
    }
    if (internal) {
        setInternalKeyPool.insert(index);
    } else {
        setExternalKeyPool.insert(index);
    }
    m_pool_key_to_index[pubkey.GetID()] = index;
}

void LegacyScriptPubKeyMan::KeepDestination(int64_t nIndex, const OutputType& type)
{
    assert(type != OutputType::BECH32M);
    // Remove from key pool
    WalletBatch batch(m_storage.GetDatabase());
    batch.ErasePool(nIndex);
    CPubKey pubkey;
    bool have_pk = GetPubKey(m_index_to_reserved_key.at(nIndex), pubkey);
    assert(have_pk);
    LearnRelatedScripts(pubkey, type);
    m_index_to_reserved_key.erase(nIndex);
    WalletLogPrintf("keypool keep %d\n", nIndex);
}

void LegacyScriptPubKeyMan::ReturnDestination(int64_t nIndex, bool fInternal, const CTxDestination&)
{
    // Return to key pool
    {
        LOCK(cs_KeyStore);
        if (fInternal) {
            setInternalKeyPool.insert(nIndex);
        } else if (!set_pre_split_keypool.empty()) {
            set_pre_split_keypool.insert(nIndex);
        } else {
            setExternalKeyPool.insert(nIndex);
        }
        CKeyID& pubkey_id = m_index_to_reserved_key.at(nIndex);
        m_pool_key_to_index[pubkey_id] = nIndex;
        m_index_to_reserved_key.erase(nIndex);
        NotifyCanGetAddressesChanged();
    }
    WalletLogPrintf("keypool return %d\n", nIndex);
}

bool LegacyScriptPubKeyMan::GetKeyFromPool(CPubKey& result, const OutputType type, bool internal)
{
    m_storage.AssertWalletLockHeld();
    assert(type != OutputType::BECH32M);
    if (!CanGetAddresses(internal)) {
        return false;
    }

    CKeyPool keypool;
    {
        LOCK(cs_KeyStore);
        int64_t nIndex;
        if (!ReserveKeyFromKeyPool(nIndex, keypool, internal) && !m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
            if (m_storage.IsLocked()) return false;
            if (!CanDeriveFrom(m_hd_chain)) return false;
            WalletBatch batch(m_storage.GetDatabase());
            result = GenerateNewKey(batch, m_hd_chain, internal);
            return true;
        }
        KeepDestination(nIndex, type);
        result = keypool.vchPubKey;
    }
    return true;
}

bool LegacyScriptPubKeyMan::ReserveKeyFromKeyPool(int64_t& nIndex, CKeyPool& keypool, bool fRequestedInternal)
{
    nIndex = -1;
    keypool.vchPubKey = CPubKey();
    {
        LOCK(cs_KeyStore);

        bool fReturningInternal = fRequestedInternal;
        fReturningInternal &= (IsHDEnabled() && m_storage.CanSupportFeature(FEATURE_HD_SPLIT)) || m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
        bool use_split_keypool = set_pre_split_keypool.empty();
        std::set<int64_t>& setKeyPool = use_split_keypool ? (fReturningInternal ? setInternalKeyPool : setExternalKeyPool) : set_pre_split_keypool;

        // Get the oldest key
        if (setKeyPool.empty()) {
            return false;
        }

        WalletBatch batch(m_storage.GetDatabase());

        auto it = setKeyPool.begin();
        nIndex = *it;
        setKeyPool.erase(it);
        if (!batch.ReadPool(nIndex, keypool)) {
            throw std::runtime_error(std::string(__func__) + ": read failed");
        }
        CPubKey pk;
        if (!GetPubKey(keypool.vchPubKey.GetID(), pk)) {
            throw std::runtime_error(std::string(__func__) + ": unknown key in key pool");
        }
        // If the key was pre-split keypool, we don't care about what type it is
        if (use_split_keypool && keypool.fInternal != fReturningInternal) {
            throw std::runtime_error(std::string(__func__) + ": keypool entry misclassified");
        }
        if (!keypool.vchPubKey.IsValid()) {
            throw std::runtime_error(std::string(__func__) + ": keypool entry invalid");
        }

        assert(m_index_to_reserved_key.count(nIndex) == 0);
        m_index_to_reserved_key[nIndex] = keypool.vchPubKey.GetID();
        m_pool_key_to_index.erase(keypool.vchPubKey.GetID());
        WalletLogPrintf("keypool reserve %d\n", nIndex);
    }
    NotifyCanGetAddressesChanged();
    return true;
}

void LegacyScriptPubKeyMan::LearnRelatedScripts(const CPubKey& key, OutputType type)
{
    assert(type != OutputType::BECH32M);
    if (key.IsCompressed() && (type == OutputType::P2SH_SEGWIT || type == OutputType::BECH32)) {
        CTxDestination witdest = WitnessV0KeyHash(key.GetID());
        CScript witprog = GetScriptForDestination(witdest);
        // Make sure the resulting program is solvable.
        assert(IsSolvable(*this, witprog));
        AddCScript(witprog);
    }
}

void LegacyScriptPubKeyMan::LearnAllRelatedScripts(const CPubKey& key)
{
    // OutputType::P2SH_SEGWIT always adds all necessary scripts for all types.
    LearnRelatedScripts(key, OutputType::P2SH_SEGWIT);
}

void LegacyScriptPubKeyMan::MarkReserveKeysAsUsed(int64_t keypool_id)
{
    AssertLockHeld(cs_KeyStore);
    bool internal = setInternalKeyPool.count(keypool_id);
    if (!internal) assert(setExternalKeyPool.count(keypool_id) || set_pre_split_keypool.count(keypool_id));
    std::set<int64_t> *setKeyPool = internal ? &setInternalKeyPool : (set_pre_split_keypool.empty() ? &setExternalKeyPool : &set_pre_split_keypool);
    auto it = setKeyPool->begin();

    WalletBatch batch(m_storage.GetDatabase());
    while (it != std::end(*setKeyPool)) {
        const int64_t& index = *(it);
        if (index > keypool_id) break; // set*KeyPool is ordered

        CKeyPool keypool;
        if (batch.ReadPool(index, keypool)) { //TODO: This should be unnecessary
            m_pool_key_to_index.erase(keypool.vchPubKey.GetID());
        }
        LearnAllRelatedScripts(keypool.vchPubKey);
        batch.ErasePool(index);
        WalletLogPrintf("keypool index %d removed\n", index);
        it = setKeyPool->erase(it);
    }
}

std::vector<CKeyID> GetAffectedKeys(const CScript& spk, const SigningProvider& provider)
{
    std::vector<CScript> dummy;
    FlatSigningProvider out;
    InferDescriptor(spk, provider)->Expand(0, DUMMY_SIGNING_PROVIDER, dummy, out);
    std::vector<CKeyID> ret;
    for (const auto& entry : out.pubkeys) {
        ret.push_back(entry.first);
    }
    return ret;
}

void LegacyScriptPubKeyMan::MarkPreSplitKeys()
{
    WalletBatch batch(m_storage.GetDatabase());
    for (auto it = setExternalKeyPool.begin(); it != setExternalKeyPool.end();) {
        int64_t index = *it;
        CKeyPool keypool;
        if (!batch.ReadPool(index, keypool)) {
            throw std::runtime_error(std::string(__func__) + ": read keypool entry failed");
        }
        keypool.m_pre_split = true;
        if (!batch.WritePool(index, keypool)) {
            throw std::runtime_error(std::string(__func__) + ": writing modified keypool entry failed");
        }
        set_pre_split_keypool.insert(index);
        it = setExternalKeyPool.erase(it);
    }
}

bool LegacyScriptPubKeyMan::AddCScript(const CScript& redeemScript)
{
    WalletBatch batch(m_storage.GetDatabase());
    return AddCScriptWithDB(batch, redeemScript);
}

bool LegacyScriptPubKeyMan::AddCScriptWithDB(WalletBatch& batch, const CScript& redeemScript)
{
    if (!FillableSigningProvider::AddCScript(redeemScript))
        return false;
    if (batch.WriteCScript(Hash160(redeemScript), redeemScript)) {
        m_storage.UnsetBlankWalletFlag(batch);
        return true;
    }
    return false;
}

bool LegacyScriptPubKeyMan::AddKeyOriginWithDB(WalletBatch& batch, const CPubKey& pubkey, const KeyOriginInfo& info)
{
    LOCK(cs_KeyStore);
    std::copy(info.fingerprint, info.fingerprint + 4, mapKeyMetadata[pubkey.GetID()].key_origin.fingerprint);
    mapKeyMetadata[pubkey.GetID()].key_origin.path = info.path;
    mapKeyMetadata[pubkey.GetID()].has_key_origin = true;
    mapKeyMetadata[pubkey.GetID()].hdKeypath = WriteHDKeypath(info.path);
    return batch.WriteKeyMetadata(mapKeyMetadata[pubkey.GetID()], pubkey, true);
}

bool LegacyScriptPubKeyMan::ImportScripts(const std::set<CScript> scripts, int64_t timestamp)
{
    WalletBatch batch(m_storage.GetDatabase());
    for (const auto& entry : scripts) {
        CScriptID id(entry);
        if (HaveCScript(id)) {
            WalletLogPrintf("Already have script %s, skipping\n", HexStr(entry));
            continue;
        }
        if (!AddCScriptWithDB(batch, entry)) {
            return false;
        }

        if (timestamp > 0) {
            m_script_metadata[CScriptID(entry)].nCreateTime = timestamp;
        }
    }
    if (timestamp > 0) {
        UpdateTimeFirstKey(timestamp);
    }

    return true;
}

bool LegacyScriptPubKeyMan::ImportPrivKeys(const std::map<CKeyID, CKey>& privkey_map, const int64_t timestamp)
{
    WalletBatch batch(m_storage.GetDatabase());
    for (const auto& entry : privkey_map) {
        const CKey& key = entry.second;
        CPubKey pubkey = key.GetPubKey();
        const CKeyID& id = entry.first;
        assert(key.VerifyPubKey(pubkey));
        // Skip if we already have the key
        if (HaveKey(id)) {
            WalletLogPrintf("Already have key with pubkey %s, skipping\n", HexStr(pubkey));
            continue;
        }
        mapKeyMetadata[id].nCreateTime = timestamp;
        // If the private key is not present in the wallet, insert it.
        if (!AddKeyPubKeyWithDB(batch, key, pubkey)) {
            return false;
        }
        UpdateTimeFirstKey(timestamp);
    }
    return true;
}

bool LegacyScriptPubKeyMan::ImportPubKeys(const std::vector<CKeyID>& ordered_pubkeys, const std::map<CKeyID, CPubKey>& pubkey_map, const std::map<CKeyID, std::pair<CPubKey, KeyOriginInfo>>& key_origins, const bool add_keypool, const bool internal, const int64_t timestamp)
{
    WalletBatch batch(m_storage.GetDatabase());
    for (const auto& entry : key_origins) {
        AddKeyOriginWithDB(batch, entry.second.first, entry.second.second);
    }
    for (const CKeyID& id : ordered_pubkeys) {
        auto entry = pubkey_map.find(id);
        if (entry == pubkey_map.end()) {
            continue;
        }
        const CPubKey& pubkey = entry->second;
        CPubKey temp;
        if (GetPubKey(id, temp)) {
            // Already have pubkey, skipping
            WalletLogPrintf("Already have pubkey %s, skipping\n", HexStr(temp));
            continue;
        }
        if (!AddWatchOnlyWithDB(batch, GetScriptForRawPubKey(pubkey), timestamp)) {
            return false;
        }
        mapKeyMetadata[id].nCreateTime = timestamp;

        // Add to keypool only works with pubkeys
        if (add_keypool) {
            AddKeypoolPubkeyWithDB(pubkey, internal, batch);
            NotifyCanGetAddressesChanged();
        }
    }
    return true;
}

bool LegacyScriptPubKeyMan::ImportScriptPubKeys(const std::set<CScript>& script_pub_keys, const bool have_solving_data, const int64_t timestamp)
{
    WalletBatch batch(m_storage.GetDatabase());
    for (const CScript& script : script_pub_keys) {
        if (!have_solving_data || !IsMine(script)) { // Always call AddWatchOnly for non-solvable watch-only, so that watch timestamp gets updated
            if (!AddWatchOnlyWithDB(batch, script, timestamp)) {
                return false;
            }
        }
    }
    return true;
}

std::set<CKeyID> LegacyScriptPubKeyMan::GetKeys() const
{
    LOCK(cs_KeyStore);
    if (!m_storage.HasEncryptionKeys()) {
        return FillableSigningProvider::GetKeys();
    }
    std::set<CKeyID> set_address;
    for (const auto& mi : mapCryptedKeys) {
        set_address.insert(mi.first);
    }
    return set_address;
}

bool DescriptorScriptPubKeyMan::GetNewDestination(const OutputType type, CTxDestination& dest, std::string& error)
{
    // Returns true if this descriptor supports getting new addresses. Conditions where we may be unable to fetch them (e.g. locked) are caught later
    if (!CanGetAddresses()) {
        error = "No addresses available";
        return false;
    }
    {
        LOCK(cs_desc_man);
        assert(m_wallet_descriptor.descriptor->IsSingleType()); // This is a combo descriptor which should not be an active descriptor
        std::optional<OutputType> desc_addr_type = m_wallet_descriptor.descriptor->GetOutputType();
        assert(desc_addr_type);
        if (type != *desc_addr_type) {
            throw std::runtime_error(std::string(__func__) + ": Types are inconsistent");
        }

        TopUp();

        // Get the scriptPubKey from the descriptor
        FlatSigningProvider out_keys;
        std::vector<CScript> scripts_temp;
        if (m_wallet_descriptor.range_end <= m_max_cached_index && !TopUp(1)) {
            // We can't generate anymore keys
            error = "Error: Keypool ran out, please call keypoolrefill first";
            return false;
        }
        if (!m_wallet_descriptor.descriptor->ExpandFromCache(m_wallet_descriptor.next_index, m_wallet_descriptor.cache, scripts_temp, out_keys)) {
            // We can't generate anymore keys
            error = "Error: Keypool ran out, please call keypoolrefill first";
            return false;
        }

        std::optional<OutputType> out_script_type = m_wallet_descriptor.descriptor->GetOutputType();
        if (out_script_type && out_script_type == type) {
            ExtractDestination(scripts_temp[0], dest);
        } else {
            throw std::runtime_error(std::string(__func__) + ": Types are inconsistent. Stored type does not match type of newly generated address");
        }
        m_wallet_descriptor.next_index++;
        WalletBatch(m_storage.GetDatabase()).WriteDescriptor(GetID(), m_wallet_descriptor);
        return true;
    }
}

isminetype DescriptorScriptPubKeyMan::IsMine(const CScript& script) const
{
    LOCK(cs_desc_man);
    if (m_map_script_pub_keys.count(script) > 0) {
        return ISMINE_SPENDABLE;
    }
    return ISMINE_NO;
}

bool DescriptorScriptPubKeyMan::CheckDecryptionKey(const CKeyingMaterial& master_key, bool accept_no_keys)
{
    LOCK(cs_desc_man);
    if (!m_map_keys.empty()) {
        return false;
    }

    bool keyPass = m_map_crypted_keys.empty(); // Always pass when there are no encrypted keys
    bool keyFail = false;
    for (const auto& mi : m_map_crypted_keys) {
        const CPubKey &pubkey = mi.second.first;
        const std::vector<unsigned char> &crypted_secret = mi.second.second;
        CKey key;
        if (!DecryptKey(master_key, crypted_secret, pubkey, key)) {
            keyFail = true;
            break;
        }
        keyPass = true;
        if (m_decryption_thoroughly_checked)
            break;
    }
    if (keyPass && keyFail) {
        LogPrintf("The wallet is probably corrupted: Some keys decrypt but not all.\n");
        throw std::runtime_error("Error unlocking wallet: some keys decrypt but not all. Your wallet file may be corrupt.");
    }
    if (keyFail || (!keyPass && !accept_no_keys)) {
        return false;
    }
    m_decryption_thoroughly_checked = true;
    return true;
}

bool DescriptorScriptPubKeyMan::Encrypt(const CKeyingMaterial& master_key, WalletBatch* batch)
{
    LOCK(cs_desc_man);
    if (!m_map_crypted_keys.empty()) {
        return false;
    }

    for (const KeyMap::value_type& key_in : m_map_keys)
    {
        const CKey &key = key_in.second;
        CPubKey pubkey = key.GetPubKey();
        CKeyingMaterial secret(key.begin(), key.end());
        std::vector<unsigned char> crypted_secret;
        if (!EncryptSecret(master_key, secret, pubkey.GetHash(), crypted_secret)) {
            return false;
        }
        m_map_crypted_keys[pubkey.GetID()] = make_pair(pubkey, crypted_secret);
        batch->WriteCryptedDescriptorKey(GetID(), pubkey, crypted_secret);
    }
    m_map_keys.clear();
    return true;
}

bool DescriptorScriptPubKeyMan::GetReservedDestination(const OutputType type, bool internal, CTxDestination& address, int64_t& index, CKeyPool& keypool, std::string& error)
{
    LOCK(cs_desc_man);
    bool result = GetNewDestination(type, address, error);
    index = m_wallet_descriptor.next_index - 1;
    return result;
}

void DescriptorScriptPubKeyMan::ReturnDestination(int64_t index, bool internal, const CTxDestination& addr)
{
    LOCK(cs_desc_man);
    // Only return when the index was the most recent
    if (m_wallet_descriptor.next_index - 1 == index) {
        m_wallet_descriptor.next_index--;
    }
    WalletBatch(m_storage.GetDatabase()).WriteDescriptor(GetID(), m_wallet_descriptor);
    NotifyCanGetAddressesChanged();
}

std::map<CKeyID, CKey> DescriptorScriptPubKeyMan::GetKeys() const
{
    AssertLockHeld(cs_desc_man);
    if (m_storage.HasEncryptionKeys() && !m_storage.IsLocked()) {
        KeyMap keys;
        for (auto key_pair : m_map_crypted_keys) {
            const CPubKey& pubkey = key_pair.second.first;
            const std::vector<unsigned char>& crypted_secret = key_pair.second.second;
            CKey key;
            DecryptKey(m_storage.GetEncryptionKey(), crypted_secret, pubkey, key);
            keys[pubkey.GetID()] = key;
        }
        return keys;
    }
    return m_map_keys;
}

bool DescriptorScriptPubKeyMan::TopUp(unsigned int size)
{
    LOCK(cs_desc_man);
    unsigned int target_size;
    if (size > 0) {
        target_size = size;
    } else {
        target_size = std::max(gArgs.GetArg("-keypool", DEFAULT_KEYPOOL_SIZE), (int64_t) 1);
    }

    // Calculate the new range_end
    int32_t new_range_end = std::max(m_wallet_descriptor.next_index + (int32_t)target_size, m_wallet_descriptor.range_end);

    // If the descriptor is not ranged, we actually just want to fill the first cache item
    if (!m_wallet_descriptor.descriptor->IsRange()) {
        new_range_end = 1;
        m_wallet_descriptor.range_end = 1;
        m_wallet_descriptor.range_start = 0;
    }

    FlatSigningProvider provider;
    provider.keys = GetKeys();

    WalletBatch batch(m_storage.GetDatabase());
    uint256 id = GetID();
    for (int32_t i = m_max_cached_index + 1; i < new_range_end; ++i) {
        FlatSigningProvider out_keys;
        std::vector<CScript> scripts_temp;
        DescriptorCache temp_cache;
        // Maybe we have a cached xpub and we can expand from the cache first
        if (!m_wallet_descriptor.descriptor->ExpandFromCache(i, m_wallet_descriptor.cache, scripts_temp, out_keys)) {
            if (!m_wallet_descriptor.descriptor->Expand(i, provider, scripts_temp, out_keys, &temp_cache)) return false;
        }
        // Add all of the scriptPubKeys to the scriptPubKey set
        for (const CScript& script : scripts_temp) {
            m_map_script_pub_keys[script] = i;
        }
        for (const auto& pk_pair : out_keys.pubkeys) {
            const CPubKey& pubkey = pk_pair.second;
            if (m_map_pubkeys.count(pubkey) != 0) {
                // We don't need to give an error here.
                // It doesn't matter which of many valid indexes the pubkey has, we just need an index where we can derive it and it's private key
                continue;
            }
            m_map_pubkeys[pubkey] = i;
        }
        // Merge and write the cache
        DescriptorCache new_items = m_wallet_descriptor.cache.MergeAndDiff(temp_cache);
        if (!batch.WriteDescriptorCacheItems(id, new_items)) {
            throw std::runtime_error(std::string(__func__) + ": writing cache items failed");
        }
        m_max_cached_index++;
    }
    m_wallet_descriptor.range_end = new_range_end;
    batch.WriteDescriptor(GetID(), m_wallet_descriptor);

    // By this point, the cache size should be the size of the entire range
    assert(m_wallet_descriptor.range_end - 1 == m_max_cached_index);

    NotifyCanGetAddressesChanged();
    return true;
}

void DescriptorScriptPubKeyMan::MarkUnusedAddresses(const CScript& script)
{
    LOCK(cs_desc_man);
    if (IsMine(script)) {
        int32_t index = m_map_script_pub_keys[script];
        if (index >= m_wallet_descriptor.next_index) {
            WalletLogPrintf("%s: Detected a used keypool item at index %d, mark all keypool items up to this item as used\n", __func__, index);
            m_wallet_descriptor.next_index = index + 1;
        }
        if (!TopUp()) {
            WalletLogPrintf("%s: Topping up keypool failed (locked wallet)\n", __func__);
        }
    }
}

void DescriptorScriptPubKeyMan::AddDescriptorKey(const CKey& key, const CPubKey &pubkey)
{
    LOCK(cs_desc_man);
    WalletBatch batch(m_storage.GetDatabase());
    if (!AddDescriptorKeyWithDB(batch, key, pubkey)) {
        throw std::runtime_error(std::string(__func__) + ": writing descriptor private key failed");
    }
}

bool DescriptorScriptPubKeyMan::AddDescriptorKeyWithDB(WalletBatch& batch, const CKey& key, const CPubKey &pubkey)
{
    AssertLockHeld(cs_desc_man);
    assert(!m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS));

    // Check if provided key already exists
    if (m_map_keys.find(pubkey.GetID()) != m_map_keys.end() ||
        m_map_crypted_keys.find(pubkey.GetID()) != m_map_crypted_keys.end()) {
        return true;
    }

    if (m_storage.HasEncryptionKeys()) {
        if (m_storage.IsLocked()) {
            return false;
        }

        std::vector<unsigned char> crypted_secret;
        CKeyingMaterial secret(key.begin(), key.end());
        if (!EncryptSecret(m_storage.GetEncryptionKey(), secret, pubkey.GetHash(), crypted_secret)) {
            return false;
        }

        m_map_crypted_keys[pubkey.GetID()] = make_pair(pubkey, crypted_secret);
        return batch.WriteCryptedDescriptorKey(GetID(), pubkey, crypted_secret);
    } else {
        m_map_keys[pubkey.GetID()] = key;
        return batch.WriteDescriptorKey(GetID(), pubkey, key.GetPrivKey());
    }
}

bool DescriptorScriptPubKeyMan::SetupDescriptorGeneration(const CExtKey& master_key, OutputType addr_type, bool internal)
{
    if (addr_type == OutputType::BECH32M) {
        // Don't allow setting up taproot descriptors yet
        // TODO: Allow setting up taproot descriptors
        return false;
    }

    LOCK(cs_desc_man);
    assert(m_storage.IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS));

    // Ignore when there is already a descriptor
    if (m_wallet_descriptor.descriptor) {
        return false;
    }

    int64_t creation_time = GetTime();

    std::string xpub = EncodeExtPubKey(master_key.Neuter());

    // Build descriptor string
    std::string desc_prefix;
    std::string desc_suffix = "/*)";
    switch (addr_type) {
    case OutputType::LEGACY: {
        desc_prefix = "pkh(" + xpub + "/44'";
        break;
    }
    case OutputType::P2SH_SEGWIT: {
        desc_prefix = "sh(wpkh(" + xpub + "/49'";
        desc_suffix += ")";
        break;
    }
    case OutputType::BECH32: {
        desc_prefix = "wpkh(" + xpub + "/84'";
        break;
    }
    case OutputType::BECH32M: assert(false); // TODO: Setup taproot descriptor
    } // no default case, so the compiler can warn about missing cases
    assert(!desc_prefix.empty());

    // Derive at Reddcoin's registered SLIP-0044 coin type, the same one legacy
    // HD wallets use: 4' on mainnet, 1' on the test chains.
    desc_prefix += strprintf("/%d'", Params().ExtCoinType());

    std::string internal_path = internal ? "/1" : "/0";
    std::string desc_str = desc_prefix + "/0'" + internal_path + desc_suffix;

    // Make the descriptor
    FlatSigningProvider keys;
    std::string error;
    std::unique_ptr<Descriptor> desc = Parse(desc_str, keys, error, false);
    WalletDescriptor w_desc(std::move(desc), creation_time, 0, 0, 0);
    m_wallet_descriptor = w_desc;

    // Store the master private key, and descriptor
    WalletBatch batch(m_storage.GetDatabase());
    if (!AddDescriptorKeyWithDB(batch, master_key.key, master_key.key.GetPubKey())) {
        throw std::runtime_error(std::string(__func__) + ": writing descriptor master private key failed");
    }
    if (!batch.WriteDescriptor(GetID(), m_wallet_descriptor)) {
        throw std::runtime_error(std::string(__func__) + ": writing descriptor failed");
    }

    // TopUp
    TopUp();

    m_storage.UnsetBlankWalletFlag(batch);
    return true;
}

bool DescriptorScriptPubKeyMan::IsHDEnabled() const
{
    LOCK(cs_desc_man);
    return m_wallet_descriptor.descriptor->IsRange();
}

bool DescriptorScriptPubKeyMan::CanGetAddresses(bool internal) const
{
    // We can only give out addresses from descriptors that are single type (not combo), ranged,
    // and either have cached keys or can generate more keys (ignoring encryption)
    LOCK(cs_desc_man);
    return m_wallet_descriptor.descriptor->IsSingleType() &&
           m_wallet_descriptor.descriptor->IsRange() &&
           (HavePrivateKeys() || m_wallet_descriptor.next_index < m_wallet_descriptor.range_end);
}

bool DescriptorScriptPubKeyMan::HavePrivateKeys() const
{
    LOCK(cs_desc_man);
    return m_map_keys.size() > 0 || m_map_crypted_keys.size() > 0;
}

int64_t DescriptorScriptPubKeyMan::GetOldestKeyPoolTime() const
{
    // This is only used for getwalletinfo output and isn't relevant to descriptor wallets.
    // The magic number 0 indicates that it shouldn't be displayed so that's what we return.
    return 0;
}


unsigned int DescriptorScriptPubKeyMan::GetKeyPoolSize() const
{
    LOCK(cs_desc_man);
    return m_wallet_descriptor.range_end - m_wallet_descriptor.next_index;
}

int64_t DescriptorScriptPubKeyMan::GetTimeFirstKey() const
{
    LOCK(cs_desc_man);
    return m_wallet_descriptor.creation_time;
}

std::unique_ptr<FlatSigningProvider> DescriptorScriptPubKeyMan::GetSigningProvider(const CScript& script, bool include_private) const
{
    LOCK(cs_desc_man);

    // Find the index of the script
    auto it = m_map_script_pub_keys.find(script);
    if (it == m_map_script_pub_keys.end()) {
        return nullptr;
    }
    int32_t index = it->second;

    return GetSigningProvider(index, include_private);
}

std::unique_ptr<FlatSigningProvider> DescriptorScriptPubKeyMan::GetSigningProvider(const CPubKey& pubkey) const
{
    LOCK(cs_desc_man);

    // Find index of the pubkey
    auto it = m_map_pubkeys.find(pubkey);
    if (it == m_map_pubkeys.end()) {
        return nullptr;
    }
    int32_t index = it->second;

    // Always try to get the signing provider with private keys. This function should only be called during signing anyways
    return GetSigningProvider(index, true);
}

std::unique_ptr<FlatSigningProvider> DescriptorScriptPubKeyMan::GetSigningProvider(int32_t index, bool include_private) const
{
    AssertLockHeld(cs_desc_man);
    // Get the scripts, keys, and key origins for this script
    std::unique_ptr<FlatSigningProvider> out_keys = std::make_unique<FlatSigningProvider>();
    std::vector<CScript> scripts_temp;
    if (!m_wallet_descriptor.descriptor->ExpandFromCache(index, m_wallet_descriptor.cache, scripts_temp, *out_keys)) return nullptr;

    if (HavePrivateKeys() && include_private) {
        FlatSigningProvider master_provider;
        master_provider.keys = GetKeys();
        m_wallet_descriptor.descriptor->ExpandPrivate(index, master_provider, *out_keys);
    }

    return out_keys;
}

std::unique_ptr<SigningProvider> DescriptorScriptPubKeyMan::GetSolvingProvider(const CScript& script) const
{
    return GetSigningProvider(script, false);
}

bool DescriptorScriptPubKeyMan::CanProvide(const CScript& script, SignatureData& sigdata)
{
    return IsMine(script);
}

bool DescriptorScriptPubKeyMan::SignTransaction(CMutableTransaction& tx, const std::map<COutPoint, Coin>& coins, int sighash, std::map<int, std::string>& input_errors) const
{
    std::unique_ptr<FlatSigningProvider> keys = std::make_unique<FlatSigningProvider>();
    for (const auto& coin_pair : coins) {
        std::unique_ptr<FlatSigningProvider> coin_keys = GetSigningProvider(coin_pair.second.out.scriptPubKey, true);
        if (!coin_keys) {
            continue;
        }
        *keys = Merge(*keys, *coin_keys);
    }

    return ::SignTransaction(tx, keys.get(), coins, sighash, input_errors);
}

SigningResult DescriptorScriptPubKeyMan::SignMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) const
{
    std::unique_ptr<FlatSigningProvider> keys = GetSigningProvider(GetScriptForDestination(pkhash), true);
    if (!keys) {
        return SigningResult::PRIVATE_KEY_NOT_AVAILABLE;
    }

    CKey key;
    if (!keys->GetKey(ToKeyID(pkhash), key)) {
        return SigningResult::PRIVATE_KEY_NOT_AVAILABLE;
    }

    if (!MessageSign(key, message, str_sig)) {
        return SigningResult::SIGNING_FAILED;
    }
    return SigningResult::OK;
}

TransactionError DescriptorScriptPubKeyMan::FillPSBT(PartiallySignedTransaction& psbtx, const PrecomputedTransactionData& txdata, int sighash_type, bool sign, bool bip32derivs, int* n_signed) const
{
    if (n_signed) {
        *n_signed = 0;
    }
    for (unsigned int i = 0; i < psbtx.tx->vin.size(); ++i) {
        const CTxIn& txin = psbtx.tx->vin[i];
        PSBTInput& input = psbtx.inputs.at(i);

        if (PSBTInputSigned(input)) {
            continue;
        }

        // Get the Sighash type
        if (sign && input.sighash_type > 0 && input.sighash_type != sighash_type) {
            return TransactionError::SIGHASH_MISMATCH;
        }

        // Get the scriptPubKey to know which SigningProvider to use
        CScript script;
        if (!input.witness_utxo.IsNull()) {
            script = input.witness_utxo.scriptPubKey;
        } else if (input.non_witness_utxo) {
            if (txin.prevout.n >= input.non_witness_utxo->vout.size()) {
                return TransactionError::MISSING_INPUTS;
            }
            script = input.non_witness_utxo->vout[txin.prevout.n].scriptPubKey;
        } else {
            // There's no UTXO so we can just skip this now
            continue;
        }
        SignatureData sigdata;
        input.FillSignatureData(sigdata);

        std::unique_ptr<FlatSigningProvider> keys = std::make_unique<FlatSigningProvider>();
        std::unique_ptr<FlatSigningProvider> script_keys = GetSigningProvider(script, sign);
        if (script_keys) {
            *keys = Merge(*keys, *script_keys);
        } else {
            // Maybe there are pubkeys listed that we can sign for
            script_keys = std::make_unique<FlatSigningProvider>();
            for (const auto& pk_pair : input.hd_keypaths) {
                const CPubKey& pubkey = pk_pair.first;
                std::unique_ptr<FlatSigningProvider> pk_keys = GetSigningProvider(pubkey);
                if (pk_keys) {
                    *keys = Merge(*keys, *pk_keys);
                }
            }
        }

        SignPSBTInput(HidingSigningProvider(keys.get(), !sign, !bip32derivs), psbtx, i, &txdata, sighash_type);

        bool signed_one = PSBTInputSigned(input);
        if (n_signed && (signed_one || !sign)) {
            // If sign is false, we assume that we _could_ sign if we get here. This
            // will never have false negatives; it is hard to tell under what i
            // circumstances it could have false positives.
            (*n_signed)++;
        }
    }

    // Fill in the bip32 keypaths and redeemscripts for the outputs so that hardware wallets can identify change
    for (unsigned int i = 0; i < psbtx.tx->vout.size(); ++i) {
        std::unique_ptr<SigningProvider> keys = GetSolvingProvider(psbtx.tx->vout.at(i).scriptPubKey);
        if (!keys) {
            continue;
        }
        UpdatePSBTOutput(HidingSigningProvider(keys.get(), true, !bip32derivs), psbtx, i);
    }

    return TransactionError::OK;
}

std::unique_ptr<CKeyMetadata> DescriptorScriptPubKeyMan::GetMetadata(const CTxDestination& dest) const
{
    std::unique_ptr<SigningProvider> provider = GetSigningProvider(GetScriptForDestination(dest));
    if (provider) {
        KeyOriginInfo orig;
        CKeyID key_id = GetKeyForDestination(*provider, dest);
        if (provider->GetKeyOrigin(key_id, orig)) {
            LOCK(cs_desc_man);
            std::unique_ptr<CKeyMetadata> meta = std::make_unique<CKeyMetadata>();
            meta->key_origin = orig;
            meta->has_key_origin = true;
            meta->nCreateTime = m_wallet_descriptor.creation_time;
            return meta;
        }
    }
    return nullptr;
}

uint256 DescriptorScriptPubKeyMan::GetID() const
{
    LOCK(cs_desc_man);
    std::string desc_str = m_wallet_descriptor.descriptor->ToString();
    uint256 id;
    CSHA256().Write((unsigned char*)desc_str.data(), desc_str.size()).Finalize(id.begin());
    return id;
}

void DescriptorScriptPubKeyMan::SetCache(const DescriptorCache& cache)
{
    LOCK(cs_desc_man);
    m_wallet_descriptor.cache = cache;
    for (int32_t i = m_wallet_descriptor.range_start; i < m_wallet_descriptor.range_end; ++i) {
        FlatSigningProvider out_keys;
        std::vector<CScript> scripts_temp;
        if (!m_wallet_descriptor.descriptor->ExpandFromCache(i, m_wallet_descriptor.cache, scripts_temp, out_keys)) {
            throw std::runtime_error("Error: Unable to expand wallet descriptor from cache");
        }
        // Add all of the scriptPubKeys to the scriptPubKey set
        for (const CScript& script : scripts_temp) {
            if (m_map_script_pub_keys.count(script) != 0) {
                throw std::runtime_error(strprintf("Error: Already loaded script at index %d as being at index %d", i, m_map_script_pub_keys[script]));
            }
            m_map_script_pub_keys[script] = i;
        }
        for (const auto& pk_pair : out_keys.pubkeys) {
            const CPubKey& pubkey = pk_pair.second;
            if (m_map_pubkeys.count(pubkey) != 0) {
                // We don't need to give an error here.
                // It doesn't matter which of many valid indexes the pubkey has, we just need an index where we can derive it and it's private key
                continue;
            }
            m_map_pubkeys[pubkey] = i;
        }
        m_max_cached_index++;
    }
}

bool DescriptorScriptPubKeyMan::AddKey(const CKeyID& key_id, const CKey& key)
{
    LOCK(cs_desc_man);
    m_map_keys[key_id] = key;
    return true;
}

bool DescriptorScriptPubKeyMan::AddCryptedKey(const CKeyID& key_id, const CPubKey& pubkey, const std::vector<unsigned char>& crypted_key)
{
    LOCK(cs_desc_man);
    if (!m_map_keys.empty()) {
        return false;
    }

    m_map_crypted_keys[key_id] = make_pair(pubkey, crypted_key);
    return true;
}

bool DescriptorScriptPubKeyMan::HasWalletDescriptor(const WalletDescriptor& desc) const
{
    LOCK(cs_desc_man);
    return m_wallet_descriptor.descriptor != nullptr && desc.descriptor != nullptr && m_wallet_descriptor.descriptor->ToString() == desc.descriptor->ToString();
}

void DescriptorScriptPubKeyMan::WriteDescriptor()
{
    LOCK(cs_desc_man);
    WalletBatch batch(m_storage.GetDatabase());
    if (!batch.WriteDescriptor(GetID(), m_wallet_descriptor)) {
        throw std::runtime_error(std::string(__func__) + ": writing descriptor failed");
    }
}

const WalletDescriptor DescriptorScriptPubKeyMan::GetWalletDescriptor() const
{
    return m_wallet_descriptor;
}

const std::vector<CScript> DescriptorScriptPubKeyMan::GetScriptPubKeys() const
{
    LOCK(cs_desc_man);
    std::vector<CScript> script_pub_keys;
    script_pub_keys.reserve(m_map_script_pub_keys.size());

    for (auto const& script_pub_key: m_map_script_pub_keys) {
        script_pub_keys.push_back(script_pub_key.first);
    }
    return script_pub_keys;
}

bool DescriptorScriptPubKeyMan::GetDescriptorString(std::string& out) const
{
    LOCK(cs_desc_man);

    FlatSigningProvider provider;
    provider.keys = GetKeys();

    return m_wallet_descriptor.descriptor->ToNormalizedString(provider, out, &m_wallet_descriptor.cache);
}

void DescriptorScriptPubKeyMan::UpgradeDescriptorCache()
{
    LOCK(cs_desc_man);
    if (m_storage.IsLocked() || m_storage.IsWalletFlagSet(WALLET_FLAG_LAST_HARDENED_XPUB_CACHED)) {
        return;
    }

    // Skip if we have the last hardened xpub cache
    if (m_wallet_descriptor.cache.GetCachedLastHardenedExtPubKeys().size() > 0) {
        return;
    }

    // Expand the descriptor
    FlatSigningProvider provider;
    provider.keys = GetKeys();
    FlatSigningProvider out_keys;
    std::vector<CScript> scripts_temp;
    DescriptorCache temp_cache;
    if (!m_wallet_descriptor.descriptor->Expand(0, provider, scripts_temp, out_keys, &temp_cache)){
        throw std::runtime_error("Unable to expand descriptor");
    }

    // Cache the last hardened xpubs
    DescriptorCache diff = m_wallet_descriptor.cache.MergeAndDiff(temp_cache);
    if (!WalletBatch(m_storage.GetDatabase()).WriteDescriptorCacheItems(GetID(), diff)) {
        throw std::runtime_error(std::string(__func__) + ": writing cache items failed");
    }
}

void DescriptorScriptPubKeyMan::UpdateWalletDescriptor(WalletDescriptor& descriptor)
{
    LOCK(cs_desc_man);
    std::string error;
    if (!CanUpdateToWalletDescriptor(descriptor, error)) {
        throw std::runtime_error(std::string(__func__) + ": " + error);
    }

    m_map_pubkeys.clear();
    m_map_script_pub_keys.clear();
    m_max_cached_index = -1;
    m_wallet_descriptor = descriptor;
}

bool DescriptorScriptPubKeyMan::CanUpdateToWalletDescriptor(const WalletDescriptor& descriptor, std::string& error)
{
    LOCK(cs_desc_man);
    if (!HasWalletDescriptor(descriptor)) {
        error = "can only update matching descriptor";
        return false;
    }

    if (descriptor.range_start > m_wallet_descriptor.range_start ||
        descriptor.range_end < m_wallet_descriptor.range_end) {
        // Use inclusive range for error
        error = strprintf("new range must include current range = [%d,%d]",
                          m_wallet_descriptor.range_start,
                          m_wallet_descriptor.range_end - 1);
        return false;
    }

    return true;
}
