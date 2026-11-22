// Copyright (c) 2019-2020 The Bitcoin Core developers
// Copyright (c) 2019-2023 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_SCRIPTPUBKEYMAN_H
#define BITCOIN_WALLET_SCRIPTPUBKEYMAN_H

#include <psbt.h>
#include <script/descriptor.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <util/error.h>
#include <util/message.h>
#include <util/time.h>
#include <wallet/crypter.h>
#include <wallet/ismine.h>
#include <wallet/walletdb.h>
#include <wallet/walletutil.h>

#include <boost/signals2/signal.hpp>

#include <optional>
#include <unordered_map>

enum class OutputType;
struct bilingual_str;

// Wallet storage things that ScriptPubKeyMans need in order to be able to store things to the wallet database.
// It provides access to things that are part of the entire wallet and not specific to a ScriptPubKeyMan such as
// wallet flags, wallet version, encryption keys, encryption status, and the database itself. This allows a
// ScriptPubKeyMan to have callbacks into CWallet without causing a circular dependency.
// WalletStorage should be the same for all ScriptPubKeyMans of a wallet.
class WalletStorage
{
public:
    virtual ~WalletStorage() = default;
    virtual const std::string GetDisplayName() const = 0;
    virtual WalletDatabase& GetDatabase() const = 0;
    virtual bool IsWalletFlagSet(uint64_t) const = 0;
    virtual void UnsetBlankWalletFlag(WalletBatch&) = 0;
    //! Write the wallet's flags with this one added, through the batch. The
    //! flags in memory are not changed: the batch may be part of a transaction
    //! that fails. ConfirmWalletFlag follows once it is committed.
    virtual bool SetWalletFlagWithDB(WalletBatch&, uint64_t) = 0;
    virtual void ConfirmWalletFlag(uint64_t) = 0;
    //! The wallet's own lock is held. Asserted where a key manager, holding its
    //! key store lock, asks the wallet something that takes the wallet's lock:
    //! called without it, the two locks would be taken in the wrong order.
    virtual void AssertWalletLockHeld() const = 0;
    //! Whether the wallet is being read only to be shown (the wallet tool's dump and info): nothing may be written.
    virtual bool IsReadOnlyLoad() const = 0;
    virtual bool CanSupportFeature(enum WalletFeature) const = 0;
    virtual void SetMinVersion(enum WalletFeature, WalletBatch* = nullptr) = 0;
    virtual const CKeyingMaterial& GetEncryptionKey() const = 0;
    virtual bool HasEncryptionKeys() const = 0;
    virtual bool IsLocked() const = 0;
};

//! Default for -keypool
static const unsigned int DEFAULT_KEYPOOL_SIZE = 1000;

std::vector<CKeyID> GetAffectedKeys(const CScript& spk, const SigningProvider& provider);

/** A key from a CWallet's keypool
 *
 * The wallet holds one (for pre HD-split wallets) or several keypools. These
 * are sets of keys that have not yet been used to provide addresses or receive
 * change.
 *
 * The Bitcoin Core wallet was originally a collection of unrelated private
 * keys with their associated addresses. If a non-HD wallet generated a
 * key/address, gave that address out and then restored a backup from before
 * that key's generation, then any funds sent to that address would be
 * lost definitively.
 *
 * The keypool was implemented to avoid this scenario (commit: 10384941). The
 * wallet would generate a set of keys (100 by default). When a new public key
 * was required, either to give out as an address or to use in a change output,
 * it would be drawn from the keypool. The keypool would then be topped up to
 * maintain 100 keys. This ensured that as long as the wallet hadn't used more
 * than 100 keys since the previous backup, all funds would be safe, since a
 * restored wallet would be able to scan for all owned addresses.
 *
 * A keypool also allowed encrypted wallets to give out addresses without
 * having to be decrypted to generate a new private key.
 *
 * With the introduction of HD wallets (commit: f1902510), the keypool
 * essentially became an address look-ahead pool. Restoring old backups can no
 * longer definitively lose funds as long as the addresses used were from the
 * wallet's HD seed (since all private keys can be rederived from the seed).
 * However, if many addresses were used since the backup, then the wallet may
 * not know how far ahead in the HD chain to look for its addresses. The
 * keypool is used to implement a 'gap limit'. The keypool maintains a set of
 * keys (by default 1000) ahead of the last used key and scans for the
 * addresses of those keys.  This avoids the risk of not seeing transactions
 * involving the wallet's addresses, or of re-using the same address.
 * In the unlikely case where none of the addresses in the `gap limit` are
 * used on-chain, the look-ahead will not be incremented to keep
 * a constant size and addresses beyond this range will not be detected by an
 * old backup. For this reason, it is not recommended to decrease keypool size
 * lower than default value.
 *
 * The HD-split wallet feature added a second keypool (commit: 02592f4c). There
 * is an external keypool (for addresses to hand out) and an internal keypool
 * (for change addresses).
 *
 * Keypool keys are stored in the wallet/keystore's keymap. The keypool data is
 * stored as sets of indexes in the wallet (setInternalKeyPool,
 * setExternalKeyPool and set_pre_split_keypool), and a map from the key to the
 * index (m_pool_key_to_index). The CKeyPool object is used to
 * serialize/deserialize the pool data to/from the database.
 */
class CKeyPool
{
public:
    //! The time at which the key was generated. Set in AddKeypoolPubKeyWithDB
    int64_t nTime;
    //! The public key
    CPubKey vchPubKey;
    //! Whether this keypool entry is in the internal keypool (for change outputs)
    bool fInternal;
    //! Whether this key was generated for a keypool before the wallet was upgraded to HD-split
    bool m_pre_split;

    CKeyPool();
    CKeyPool(const CPubKey& vchPubKeyIn, bool internalIn);

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        int nVersion = s.GetVersion();
        if (!(s.GetType() & SER_GETHASH)) {
            s << nVersion;
        }
        s << nTime << vchPubKey << fInternal << m_pre_split;
    }

    template<typename Stream>
    void Unserialize(Stream& s)
    {
        int nVersion = s.GetVersion();
        if (!(s.GetType() & SER_GETHASH)) {
            s >> nVersion;
        }
        s >> nTime >> vchPubKey;
        try {
            s >> fInternal;
        } catch (std::ios_base::failure&) {
            /* flag as external address if we can't read the internal boolean
               (this will be the case for any wallet before the HD chain split version) */
            fInternal = false;
        }
        try {
            s >> m_pre_split;
        } catch (std::ios_base::failure&) {
            /* flag as postsplit address if we can't read the m_pre_split boolean
               (this will be the case for any wallet that upgrades to HD chain split) */
            m_pre_split = false;
        }
    }
};

class KeyIDHasher
{
public:
    KeyIDHasher() {}

    size_t operator()(const CKeyID& id) const
    {
        return id.GetUint64(0);
    }
};

/*
 * A class implementing ScriptPubKeyMan manages some (or all) scriptPubKeys used in a wallet.
 * It contains the scripts and keys related to the scriptPubKeys it manages.
 * A ScriptPubKeyMan will be able to give out scriptPubKeys to be used, as well as marking
 * when a scriptPubKey has been used. It also handles when and how to store a scriptPubKey
 * and its related scripts and keys, including encryption.
 */
class ScriptPubKeyMan
{
protected:
    WalletStorage& m_storage;

public:
    explicit ScriptPubKeyMan(WalletStorage& storage) : m_storage(storage) {}
    virtual ~ScriptPubKeyMan() {};
    virtual bool GetNewDestination(const OutputType type, CTxDestination& dest, std::string& error) { return false; }
    virtual isminetype IsMine(const CScript& script) const { return ISMINE_NO; }

    //! Check that the given decryption key is valid for this ScriptPubKeyMan, i.e. it decrypts all of the keys handled by it.
    virtual bool CheckDecryptionKey(const CKeyingMaterial& master_key, bool accept_no_keys = false) { return false; }
    virtual bool Encrypt(const CKeyingMaterial& master_key, WalletBatch* batch) { return false; }

    virtual bool GetReservedDestination(const OutputType type, bool internal, CTxDestination& address, int64_t& index, CKeyPool& keypool, std::string& error) { return false; }
    virtual void KeepDestination(int64_t index, const OutputType& type) {}
    virtual void ReturnDestination(int64_t index, bool internal, const CTxDestination& addr) {}

    /** Fills internal address pool. Use within ScriptPubKeyMan implementations should be used sparingly and only
      * when something from the address pool is removed, excluding GetNewDestination and GetReservedDestination.
      * External wallet code is primarily responsible for topping up prior to fetching new addresses
      */
    virtual bool TopUp(unsigned int size = 0) { return false; }

    //! Mark unused addresses as being used
    virtual void MarkUnusedAddresses(const CScript& script) {}

    /** Sets up the key generation stuff, i.e. generates new HD seeds and sets them as active.
      * Returns false if already setup or setup fails, true if setup is successful
      * Set force=true to make it re-setup if already setup, used for upgrades
      */
    virtual bool SetupGeneration(const WalletOptions& walletoptions, bool force = false) { return false; }

    /* Returns true if HD is enabled */
    virtual bool IsHDEnabled() const { return false; }

    /* Returns true if HD bip39 is enabled */
    virtual bool IsBip39Enabled() const { return false; }

    /* Returns true if HD bip44 is enabled */
    virtual bool IsBip44Enabled() const { return false; }

    /* Returns true if the wallet can give out new addresses. This means it has keys in the keypool or can generate new keys */
    virtual bool CanGetAddresses(bool internal = false) const { return false; }

    /** Upgrades the wallet to the specified version */
    virtual bool Upgrade(int prev_version, int new_version, bilingual_str& error) { return true; }

    virtual bool HavePrivateKeys() const { return false; }

    //! The action to do when the DB needs rewrite
    virtual void RewriteDB() {}

    virtual int64_t GetOldestKeyPoolTime() const { return GetTime(); }

    virtual unsigned int GetKeyPoolSize() const { return 0; }

    virtual int64_t GetTimeFirstKey() const { return 0; }

    virtual std::unique_ptr<CKeyMetadata> GetMetadata(const CTxDestination& dest) const { return nullptr; }

    virtual std::unique_ptr<SigningProvider> GetSolvingProvider(const CScript& script) const { return nullptr; }

    /** Whether this ScriptPubKeyMan can provide a SigningProvider (via GetSolvingProvider) that, combined with
      * sigdata, can produce solving data.
      */
    virtual bool CanProvide(const CScript& script, SignatureData& sigdata) { return false; }

    /** Creates new signatures and adds them to the transaction. Returns whether all inputs were signed */
    virtual bool SignTransaction(CMutableTransaction& tx, const std::map<COutPoint, Coin>& coins, int sighash, std::map<int, std::string>& input_errors) const { return false; }
    /** Sign a message with the given script */
    virtual SigningResult SignMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) const { return SigningResult::SIGNING_FAILED; };
    /** Adds script and derivation path information to a PSBT, and optionally signs it. */
    virtual TransactionError FillPSBT(PartiallySignedTransaction& psbt, const PrecomputedTransactionData& txdata, int sighash_type = 1 /* SIGHASH_ALL */, bool sign = true, bool bip32derivs = false, int* n_signed = nullptr) const { return TransactionError::INVALID_PSBT; }

    virtual uint256 GetID() const { return uint256(); }

    /** Prepends the wallet name in logging output to ease debugging in multi-wallet use cases */
    template<typename... Params>
    void WalletLogPrintf(std::string fmt, Params... parameters) const {
        LogPrintf(("%s " + fmt).c_str(), m_storage.GetDisplayName(), parameters...);
    };

    /** Watch-only address added */
    boost::signals2::signal<void (bool fHaveWatchOnly)> NotifyWatchonlyChanged;

    /** Keypool has new keys */
    boost::signals2::signal<void ()> NotifyCanGetAddressesChanged;
};

/** OutputTypes supported by the LegacyScriptPubKeyMan */
static const std::unordered_set<OutputType> LEGACY_OUTPUT_TYPES {
    OutputType::LEGACY,
    OutputType::P2SH_SEGWIT,
    OutputType::BECH32,
};

//! What to do when the seed of a wallet does not restore all of its keys: the advice given at load.
bilingual_str SeedDoesNotRestoreAllAdvice();

class LegacyScriptPubKeyMan : public ScriptPubKeyMan, public FillableSigningProvider
{
public:
    /** Where the secrets of the active chain are: its seed, and the mnemonic
     *  and mnemonic passphrase the seed came from, if any. Decided once all
     *  records of the wallet have been read, and changed only when the
     *  secrets are moved. */
    enum class HDSecretsState {
        NOT_HELD,   //!< no HD chain, or its seed is a key of the wallet
        IN_CHAIN,   //!< in the chain record, in a wallet that is not encrypted
        UNMIGRATED, //!< in the chain record although the wallet is encrypted: moved at the next unlock
        ENCRYPTED,  //!< in their own record, encrypted; in memory only while unlocked
        UNREADABLE, //!< nowhere that can be read: the keys held go on working, none can be derived
    };

private:
    //! keeps track of whether Unlock has run a thorough check before
    bool fDecryptionThoroughlyChecked = true;

    using WatchOnlySet = std::set<CScript>;
    using WatchKeyMap = std::map<CKeyID, CPubKey>;

    WalletBatch *encrypted_batch GUARDED_BY(cs_KeyStore) = nullptr;

    using CryptedKeyMap = std::map<CKeyID, std::pair<CPubKey, std::vector<unsigned char>>>;

    CryptedKeyMap mapCryptedKeys GUARDED_BY(cs_KeyStore);
    WatchOnlySet setWatchOnly GUARDED_BY(cs_KeyStore);
    WatchKeyMap mapWatchKeys GUARDED_BY(cs_KeyStore);

    int64_t nTimeFirstKey GUARDED_BY(cs_KeyStore) = 0;

    bool AddKeyPubKeyInner(const CKey& key, const CPubKey &pubkey);
    bool AddCryptedKeyInner(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret);

    /**
     * Private version of AddWatchOnly method which does not accept a
     * timestamp, and which will reset the wallet's nTimeFirstKey value to 1 if
     * the watch key did not previously have a timestamp associated with it.
     * Because this is an inherited virtual method, it is accessible despite
     * being marked private, but it is marked private anyway to encourage use
     * of the other AddWatchOnly which accepts a timestamp and sets
     * nTimeFirstKey more intelligently for more efficient rescans.
     */
    bool AddWatchOnly(const CScript& dest) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);
    bool AddWatchOnlyWithDB(WalletBatch &batch, const CScript& dest) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);
    bool AddWatchOnlyInMem(const CScript &dest);
    //! Adds a watch-only address to the store, and saves it to disk.
    bool AddWatchOnlyWithDB(WalletBatch &batch, const CScript& dest, int64_t create_time) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);

    //! Adds a key to the store, and saves it to disk.
    bool AddKeyPubKeyWithDB(WalletBatch &batch,const CKey& key, const CPubKey &pubkey) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);

    void AddKeypoolPubkeyWithDB(const CPubKey& pubkey, const bool internal, WalletBatch& batch);

    //! Adds a script to the store and saves it to disk
    bool AddCScriptWithDB(WalletBatch& batch, const CScript& script);

    /** Add a KeyOriginInfo to the wallet */
    bool AddKeyOriginWithDB(WalletBatch& batch, const CPubKey& pubkey, const KeyOriginInfo& info);

    /* the HD chain data model (external chain counters) */
    CHDChain m_hd_chain;
    //! Keys of the active seed on BIP44 paths in a wallet whose chain derives on the ordinary ones.
    bool m_mixed_key_paths GUARDED_BY(cs_KeyStore){false};
    std::unordered_map<CKeyID, CHDChain, SaltedSipHasher> m_inactive_hd_chains;
    HDSecretsState m_hd_secrets_state GUARDED_BY(cs_KeyStore){HDSecretsState::NOT_HELD};
    //! The decrypted secrets, while the wallet is unlocked and they are kept encrypted.
    CHDSecrets m_hd_secrets GUARDED_BY(cs_KeyStore);
    //! The encrypted secrets as they are in the database.
    std::optional<CHDSecretsRecord> m_hd_secrets_record GUARDED_BY(cs_KeyStore);
    bool m_hd_secrets_pending GUARDED_BY(cs_KeyStore){false};
    //! The answer to "does the chain hold its seed itself", which is what a
    //! wallet made from a mnemonic is. See RefreshHDType.
    bool m_seed_held_by_chain GUARDED_BY(cs_KeyStore){false};
    bool m_no_seed_logged GUARDED_BY(cs_KeyStore){false};
    bool m_earlier_seed_keys GUARDED_BY(cs_KeyStore){false};

    //! Whether the record of this chain carries secrets: a chain of the mnemonic version with a seed in it.
    static bool ChainHoldsSecrets(const CHDChain& chain);
    //! This chain without its three secrets.
    static CHDChain WithoutSecrets(const CHDChain& chain);
    //! The one rule for the type of the wallet; called whenever the chain or the flag changes.
    void RefreshHDType() EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);
    //! Whether the wallet has a mnemonic phrase, when that can be known: secrets in the chain, or unlocked.
    std::optional<bool> KnownToHavePhrase() const EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);
    bilingual_str EarlierSeedWarning(bool with_advice) const EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);
    //! Write the chain record. Once the secrets are kept in their own record, it is written without them whatever the object holds.
    bool WriteHDChain(WalletBatch& batch, const CHDChain& chain) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);
    //! Whether a key can be derived from this chain now. Says once why not.
    bool CanDeriveFrom(const CHDChain& chain) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);
    /** In one transaction of a batch of its own: the flag, the encrypted
     *  secrets, the chain without them, and the rewrite marker if asked. On
     *  failure nothing is written and nothing in memory has changed. */
    bool WriteHDSecretsEncrypted(const CKeyingMaterial& master_key, const CHDSecrets& secrets, const CHDChain& chain, bool pending, CHDSecretsRecord& record) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);

    //! Inactive seeds found to have nothing to derive from, so that it is logged once for each.
    std::set<CKeyID> m_inactive_seeds_not_derivable GUARDED_BY(cs_KeyStore);

    /* HD derive new child key (on internal or external chain) */
    void DeriveNewChildKey(WalletBatch& batch, CKeyMetadata& metadata, CKey& secret, CHDChain& hd_chain, bool internal = false) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);

    std::set<int64_t> setInternalKeyPool GUARDED_BY(cs_KeyStore);
    std::set<int64_t> setExternalKeyPool GUARDED_BY(cs_KeyStore);
    std::set<int64_t> set_pre_split_keypool GUARDED_BY(cs_KeyStore);
    int64_t m_max_keypool_index GUARDED_BY(cs_KeyStore) = 0;
    std::map<CKeyID, int64_t> m_pool_key_to_index;
    // Tracks keypool indexes to CKeyIDs of keys that have been taken out of the keypool but may be returned to it
    std::map<int64_t, CKeyID> m_index_to_reserved_key;

    //! Fetches a key from the keypool
    bool GetKeyFromPool(CPubKey &key, const OutputType type, bool internal = false);

    /**
     * Reserves a key from the keypool and sets nIndex to its index
     *
     * @param[out] nIndex the index of the key in keypool
     * @param[out] keypool the keypool the key was drawn from, which could be the
     *     the pre-split pool if present, or the internal or external pool
     * @param fRequestedInternal true if the caller would like the key drawn
     *     from the internal keypool, false if external is preferred
     *
     * @return true if succeeded, false if failed due to empty keypool
     * @throws std::runtime_error if keypool read failed, key was invalid,
     *     was not found in the wallet, or was misclassified in the internal
     *     or external keypool
     */
    bool ReserveKeyFromKeyPool(int64_t& nIndex, CKeyPool& keypool, bool fRequestedInternal);

    /**
     * Like TopUp() but adds keys for inactive HD chains.
     * Ensures that there are at least -keypool number of keys derived after the given index.
     *
     * @param seed_id the CKeyID for the HD seed.
     * @param index the index to start generating keys from
     * @param internal whether the internal chain should be used. true for internal chain, false for external chain.
     *
     * @return true if seed was found and keys were derived. false if unable to derive seeds
     */
    bool TopUpInactiveHDChain(const CKeyID seed_id, int64_t index, bool internal);

public:
    using ScriptPubKeyMan::ScriptPubKeyMan;

    bool GetNewDestination(const OutputType type, CTxDestination& dest, std::string& error) override;
    isminetype IsMine(const CScript& script) const override;

    bool CheckDecryptionKey(const CKeyingMaterial& master_key, bool accept_no_keys = false) override;
    bool Encrypt(const CKeyingMaterial& master_key, WalletBatch* batch) override;

    bool GetReservedDestination(const OutputType type, bool internal, CTxDestination& address, int64_t& index, CKeyPool& keypool, std::string& error) override;
    void KeepDestination(int64_t index, const OutputType& type) override;
    void ReturnDestination(int64_t index, bool internal, const CTxDestination&) override;

    bool TopUp(unsigned int size = 0) override;

    void MarkUnusedAddresses(const CScript& script) override;

    //! Upgrade stored CKeyMetadata objects to store key origin info as KeyOriginInfo
    void UpgradeKeyMetadata();

    bool IsHDEnabled() const override;

    bool IsBip39Enabled() const override;

    bool IsBip44Enabled() const override;

    //! Decide, once the records are read, whether the wallet has keys of its active seed on
    //! BIP44 paths while its chain derives new keys on the ordinary paths.
    void DecideMixedKeyPaths();
    bool HasMixedKeyPaths() const;

    bool SetupGeneration(const WalletOptions& walletoptions, bool force = false) override;

    bool Upgrade(int prev_version, int new_version, bilingual_str& error) override;

    bool HavePrivateKeys() const override;

    void RewriteDB() override;

    int64_t GetOldestKeyPoolTime() const override;
    size_t KeypoolCountExternalKeys() const;
    unsigned int GetKeyPoolSize() const override;

    int64_t GetTimeFirstKey() const override;

    std::unique_ptr<CKeyMetadata> GetMetadata(const CTxDestination& dest) const override;

    bool CanGetAddresses(bool internal = false) const override;

    std::unique_ptr<SigningProvider> GetSolvingProvider(const CScript& script) const override;

    bool CanProvide(const CScript& script, SignatureData& sigdata) override;

    bool SignTransaction(CMutableTransaction& tx, const std::map<COutPoint, Coin>& coins, int sighash, std::map<int, std::string>& input_errors) const override;
    SigningResult SignMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) const override;
    TransactionError FillPSBT(PartiallySignedTransaction& psbt, const PrecomputedTransactionData& txdata, int sighash_type = 1 /* SIGHASH_ALL */, bool sign = true, bool bip32derivs = false, int* n_signed = nullptr) const override;

    uint256 GetID() const override;

    // Map from Key ID to key metadata.
    std::map<CKeyID, CKeyMetadata> mapKeyMetadata GUARDED_BY(cs_KeyStore);

    // Map from Script ID to key metadata (for watch-only keys).
    std::map<CScriptID, CKeyMetadata> m_script_metadata GUARDED_BY(cs_KeyStore);

    //! Adds a key to the store, and saves it to disk.
    bool AddKeyPubKey(const CKey& key, const CPubKey &pubkey) override;
    //! Adds a key to the store, without saving it to disk (used by LoadWallet)
    bool LoadKey(const CKey& key, const CPubKey &pubkey);
    //! Adds an encrypted key to the store, and saves it to disk.
    bool AddCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret);
    //! Adds an encrypted key to the store, without saving it to disk (used by LoadWallet)
    bool LoadCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret, bool checksum_valid);
    void UpdateTimeFirstKey(int64_t nCreateTime) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);
    //! Adds a CScript to the store
    bool LoadCScript(const CScript& redeemScript);
    //! Load metadata (used by LoadWallet)
    void LoadKeyMetadata(const CKeyID& keyID, const CKeyMetadata &metadata);
    void LoadScriptMetadata(const CScriptID& script_id, const CKeyMetadata &metadata);
    //! Generate a new key
    CPubKey GenerateNewKey(WalletBatch& batch, CHDChain& hd_chain, bool internal = false) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);

    /* Set the HD chain model (chain child index counters) and writes it to the database */
    void AddHDChain(const CHDChain& chain);
    //! The same, written through a batch the caller holds.
    void AddHDChain(WalletBatch& batch, const CHDChain& chain);
    //! Load a HD chain model (used by LoadWallet)
    void LoadHDChain(const CHDChain& chain);
    const CHDChain& GetHDChain() const { return m_hd_chain; }

    HDSecretsState GetHDSecretsState() const;
    //! Whether secrets were moved out of the chain record and the file has not been rewritten since.
    bool IsHDSecretsRewritePending() const;
    //! The seed of a chain, if it is available now. Derivation asks here and nowhere else.
    bool GetHDSeed(const CHDChain& chain, SecureVector& seed) const;
    //! The secrets of the active chain, if it holds its seed itself and they are available now.
    bool GetHDSecrets(CHDSecrets& secrets) const;
    //! Take the encrypted secrets and the rewrite marker as they are read from the database.
    void LoadHDSecrets(const CHDSecretsRecord& record);
    void LoadHDSecretsPending();
    //! Decide where the secrets are, once every record has been read.
    void DecideHDSecretsState();
    /** The wallet has been unlocked with this master key: decrypt the secrets,
     *  or, if they are still in the chain record of an encrypted wallet, move
     *  them. need_rewrite is set when the file has to be rewritten. Never throws. */
    void UnlockHDSecrets(const CKeyingMaterial& master_key, bool& need_rewrite);
    //! The wallet is being locked: forget the decrypted secrets.
    void LockHDSecrets();
    //! The file has been rewritten since the secrets were moved.
    void HDSecretsRewritten();
    /** Whether the chain holds its seed itself and the wallet also has keys of
     *  another seed. A wallet made from a mnemonic has none: this is a wallet
     *  with a plain seed that an earlier version turned into a mnemonic wallet
     *  when it was encrypted. Its mnemonic does not restore the earlier keys. */
    bool HasEarlierSeedKeys() const;
    //! What the owner of the wallet has to be told about where its seed is, if anything.
    //! What the owner has to be told about the seed. for_load: the form given at load, with advice
    //! in the notice given when the wallet is loaded (the earlier-seed warning there carries the advice).
    std::vector<bilingual_str> GetHDSecretsNotices(bool for_load) const;
    //! The earlier-seed warning as the commands give it: W1 (a phrase), W2 (no phrase) or W3 (not known: moved and locked).
    bilingual_str GetEarlierSeedWarning() const;
    //! The chains of seeds that were replaced, by seed id.
    const std::unordered_map<CKeyID, CHDChain, SaltedSipHasher>& GetInactiveHDChains() const { return m_inactive_hd_chains; }
    void AddInactiveHDChain(const CHDChain& chain);

    //! Adds a watch-only address to the store, without saving it to disk (used by LoadWallet)
    bool LoadWatchOnly(const CScript &dest);
    //! Returns whether the watch-only script is in the wallet
    bool HaveWatchOnly(const CScript &dest) const;
    //! Returns whether there are any watch-only things in the wallet
    bool HaveWatchOnly() const;
    //! Remove a watch only script from the keystore
    bool RemoveWatchOnly(const CScript &dest);
    bool AddWatchOnly(const CScript& dest, int64_t nCreateTime) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);

    //! Fetches a pubkey from mapWatchKeys if it exists there
    bool GetWatchPubKey(const CKeyID &address, CPubKey &pubkey_out) const;

    /* SigningProvider overrides */
    bool HaveKey(const CKeyID &address) const override;
    bool GetKey(const CKeyID &address, CKey& keyOut) const override;
    bool GetPubKey(const CKeyID &address, CPubKey& vchPubKeyOut) const override;
    bool AddCScript(const CScript& redeemScript) override;
    bool GetKeyOrigin(const CKeyID& keyid, KeyOriginInfo& info) const override;

    //! Load a keypool entry
    void LoadKeyPool(int64_t nIndex, const CKeyPool &keypool);
    bool NewKeyPool();
    void MarkPreSplitKeys() EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);

    bool ImportScripts(const std::set<CScript> scripts, int64_t timestamp) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);
    bool ImportPrivKeys(const std::map<CKeyID, CKey>& privkey_map, const int64_t timestamp) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);
    bool ImportPubKeys(const std::vector<CKeyID>& ordered_pubkeys, const std::map<CKeyID, CPubKey>& pubkey_map, const std::map<CKeyID, std::pair<CPubKey, KeyOriginInfo>>& key_origins, const bool add_keypool, const bool internal, const int64_t timestamp) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);
    bool ImportScriptPubKeys(const std::set<CScript>& script_pub_keys, const bool have_solving_data, const int64_t timestamp) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);

    /* Returns true if the wallet can generate new keys */
    bool CanGenerateKeys() const;

    /* Generates a new HD seed (will not be activated) */
    CPubKey GenerateNewSeed();

    /* Generates a new HD seed (will not be activated) */
    CPubKey GenerateNewSeed(const WalletOptions& walletoptions);

    /* Generates a new bip39 HD seed (will not be activated) */
    CPubKey GenerateNewBip39Seed(const WalletOptions& walletoptions);

    /* Derives a new HD seed (will not be activated) */
    CPubKey DeriveNewSeed(const CKey& key);

    /* Set the current HD seed (will reset the chain child index counters)
       Sets the seed's version based on the current wallet version (so the
       caller must ensure the current wallet version is correct before calling
       this function). */
    void SetHDSeed(const CPubKey& key);

    /**
     * Explicitly make the wallet learn the related scripts for outputs to the
     * given key. This is purely to make the wallet file compatible with older
     * software, as FillableSigningProvider automatically does this implicitly for all
     * keys now.
     */
    void LearnRelatedScripts(const CPubKey& key, OutputType);

    /**
     * Same as LearnRelatedScripts, but when the OutputType is not known (and could
     * be anything).
     */
    void LearnAllRelatedScripts(const CPubKey& key);

    /**
     * Marks all keys in the keypool up to and including reserve_key as used.
     */
    void MarkReserveKeysAsUsed(int64_t keypool_id) EXCLUSIVE_LOCKS_REQUIRED(cs_KeyStore);
    const std::map<CKeyID, int64_t>& GetAllReserveKeys() const { return m_pool_key_to_index; }

    std::set<CKeyID> GetKeys() const override;
};

/** Wraps a LegacyScriptPubKeyMan so that it can be returned in a new unique_ptr. Does not provide privkeys */
class LegacySigningProvider : public SigningProvider
{
private:
    const LegacyScriptPubKeyMan& m_spk_man;
public:
    explicit LegacySigningProvider(const LegacyScriptPubKeyMan& spk_man) : m_spk_man(spk_man) {}

    bool GetCScript(const CScriptID &scriptid, CScript& script) const override { return m_spk_man.GetCScript(scriptid, script); }
    bool HaveCScript(const CScriptID &scriptid) const override { return m_spk_man.HaveCScript(scriptid); }
    bool GetPubKey(const CKeyID &address, CPubKey& pubkey) const override { return m_spk_man.GetPubKey(address, pubkey); }
    bool GetKey(const CKeyID &address, CKey& key) const override { return false; }
    bool HaveKey(const CKeyID &address) const override { return false; }
    bool GetKeyOrigin(const CKeyID& keyid, KeyOriginInfo& info) const override { return m_spk_man.GetKeyOrigin(keyid, info); }
};

class DescriptorScriptPubKeyMan : public ScriptPubKeyMan
{
private:
    using ScriptPubKeyMap = std::map<CScript, int32_t>; // Map of scripts to descriptor range index
    using PubKeyMap = std::map<CPubKey, int32_t>; // Map of pubkeys involved in scripts to descriptor range index
    using CryptedKeyMap = std::map<CKeyID, std::pair<CPubKey, std::vector<unsigned char>>>;
    using KeyMap = std::map<CKeyID, CKey>;

    ScriptPubKeyMap m_map_script_pub_keys GUARDED_BY(cs_desc_man);
    PubKeyMap m_map_pubkeys GUARDED_BY(cs_desc_man);
    int32_t m_max_cached_index = -1;

    KeyMap m_map_keys GUARDED_BY(cs_desc_man);
    CryptedKeyMap m_map_crypted_keys GUARDED_BY(cs_desc_man);

    //! keeps track of whether Unlock has run a thorough check before
    bool m_decryption_thoroughly_checked = false;

    bool AddDescriptorKeyWithDB(WalletBatch& batch, const CKey& key, const CPubKey &pubkey) EXCLUSIVE_LOCKS_REQUIRED(cs_desc_man);

    KeyMap GetKeys() const EXCLUSIVE_LOCKS_REQUIRED(cs_desc_man);

    // Fetch the SigningProvider for the given script and optionally include private keys
    std::unique_ptr<FlatSigningProvider> GetSigningProvider(const CScript& script, bool include_private = false) const;
    // Fetch the SigningProvider for the given pubkey and always include private keys. This should only be called by signing code.
    std::unique_ptr<FlatSigningProvider> GetSigningProvider(const CPubKey& pubkey) const;
    // Fetch the SigningProvider for a given index and optionally include private keys. Called by the above functions.
    std::unique_ptr<FlatSigningProvider> GetSigningProvider(int32_t index, bool include_private = false) const EXCLUSIVE_LOCKS_REQUIRED(cs_desc_man);

protected:
  WalletDescriptor m_wallet_descriptor GUARDED_BY(cs_desc_man);

public:
    DescriptorScriptPubKeyMan(WalletStorage& storage, WalletDescriptor& descriptor)
        :   ScriptPubKeyMan(storage),
            m_wallet_descriptor(descriptor)
        {}
    DescriptorScriptPubKeyMan(WalletStorage& storage)
        :   ScriptPubKeyMan(storage)
        {}

    mutable RecursiveMutex cs_desc_man;

    bool GetNewDestination(const OutputType type, CTxDestination& dest, std::string& error) override;
    isminetype IsMine(const CScript& script) const override;

    bool CheckDecryptionKey(const CKeyingMaterial& master_key, bool accept_no_keys = false) override;
    bool Encrypt(const CKeyingMaterial& master_key, WalletBatch* batch) override;

    bool GetReservedDestination(const OutputType type, bool internal, CTxDestination& address, int64_t& index, CKeyPool& keypool, std::string& error) override;
    void ReturnDestination(int64_t index, bool internal, const CTxDestination& addr) override;

    // Tops up the descriptor cache and m_map_script_pub_keys. The cache is stored in the wallet file
    // and is used to expand the descriptor in GetNewDestination. DescriptorScriptPubKeyMan relies
    // more on ephemeral data than LegacyScriptPubKeyMan. For wallets using unhardened derivation
    // (with or without private keys), the "keypool" is a single xpub.
    bool TopUp(unsigned int size = 0) override;

    void MarkUnusedAddresses(const CScript& script) override;

    bool IsHDEnabled() const override;

    //! Setup descriptors based on the given CExtkey
    bool SetupDescriptorGeneration(const CExtKey& master_key, OutputType addr_type, bool internal);

    /** Provide a descriptor at setup time
    * Returns false if already setup or setup fails, true if setup is successful
    */
    bool SetupDescriptor(std::unique_ptr<Descriptor>desc);

    bool HavePrivateKeys() const override;

    int64_t GetOldestKeyPoolTime() const override;
    unsigned int GetKeyPoolSize() const override;

    int64_t GetTimeFirstKey() const override;

    std::unique_ptr<CKeyMetadata> GetMetadata(const CTxDestination& dest) const override;

    bool CanGetAddresses(bool internal = false) const override;

    std::unique_ptr<SigningProvider> GetSolvingProvider(const CScript& script) const override;

    bool CanProvide(const CScript& script, SignatureData& sigdata) override;

    bool SignTransaction(CMutableTransaction& tx, const std::map<COutPoint, Coin>& coins, int sighash, std::map<int, std::string>& input_errors) const override;
    SigningResult SignMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) const override;
    TransactionError FillPSBT(PartiallySignedTransaction& psbt, const PrecomputedTransactionData& txdata, int sighash_type = 1 /* SIGHASH_ALL */, bool sign = true, bool bip32derivs = false, int* n_signed = nullptr) const override;

    uint256 GetID() const override;

    void SetCache(const DescriptorCache& cache);

    bool AddKey(const CKeyID& key_id, const CKey& key);
    bool AddCryptedKey(const CKeyID& key_id, const CPubKey& pubkey, const std::vector<unsigned char>& crypted_key);

    bool HasWalletDescriptor(const WalletDescriptor& desc) const;
    void UpdateWalletDescriptor(WalletDescriptor& descriptor);
    bool CanUpdateToWalletDescriptor(const WalletDescriptor& descriptor, std::string& error);
    void AddDescriptorKey(const CKey& key, const CPubKey &pubkey);
    void WriteDescriptor();

    const WalletDescriptor GetWalletDescriptor() const EXCLUSIVE_LOCKS_REQUIRED(cs_desc_man);
    const std::vector<CScript> GetScriptPubKeys() const;

    bool GetDescriptorString(std::string& out) const;

    void UpgradeDescriptorCache();
};

#endif // BITCOIN_WALLET_SCRIPTPUBKEYMAN_H
