// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <fs.h>
#include <streams.h>
#include <util/translation.h>
#include <wallet/bdb.h>
#include <wallet/salvage.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <optional>

/* End of headers, beginning of key/value data */
static const char *HEADER_END = "HEADER=END";
/* End of key/value data */
static const char *DATA_END = "DATA=END";
typedef std::pair<std::vector<unsigned char>, std::vector<unsigned char> > KeyValPair;

static bool KeyFilter(const std::string& type)
{
    // The encrypted secrets of an HD chain belong with the chain: when they
    // are kept in their own record the chain record has no seed in it, and a
    // wallet salvaged without them could derive no key.
    // The metadata of a key says which HD account it is of, and the records
    // of the accounts say what the keys cannot: without them the keys of an
    // account would be taken for keys of account 0 and spent with them. The
    // witness scripts of a key are how a segwit payment to it is recognised,
    // and a restore scan still owed is still owed.
    return WalletBatch::IsKeyType(type) || type == DBKeys::HDCHAIN || type == DBKeys::HDSECRETS ||
           type == DBKeys::KEYMETA || type == DBKeys::CSCRIPT ||
           type == DBKeys::HDACCOUNT || type == DBKeys::HDACCOUNTMETA || type == DBKeys::HDRESTORESCAN || type == DBKeys::HDACCOUNTSDECLINED;
}

bool RecoverDatabaseFile(const fs::path& file_path, bilingual_str& error, std::vector<bilingual_str>& warnings)
{
    DatabaseOptions options;
    DatabaseStatus status;
    options.require_existing = true;
    options.verify = false;
    options.require_format = DatabaseFormat::BERKELEY;
    std::unique_ptr<WalletDatabase> database = MakeDatabase(file_path, options, status, error);
    if (!database) return false;

    BerkeleyDatabase& berkeley_database = static_cast<BerkeleyDatabase&>(*database);
    std::string filename = berkeley_database.Filename();
    std::shared_ptr<BerkeleyEnvironment> env = berkeley_database.env;

    if (!env->Open(error)) {
        return false;
    }

    // Recovery procedure:
    // move wallet file to walletfilename.timestamp.bak
    // Call Salvage with fAggressive=true to
    // get as much data as possible.
    // Rewrite salvaged data to fresh wallet file
    // Set -rescan so any missing transactions will be
    // found.
    int64_t now = GetTime();
    std::string newFilename = strprintf("%s.%d.bak", filename, now);

    int result = env->dbenv->dbrename(nullptr, filename.c_str(), nullptr,
                                       newFilename.c_str(), DB_AUTO_COMMIT);
    if (result != 0)
    {
        error = strprintf(Untranslated("Failed to rename %s to %s"), filename, newFilename);
        return false;
    }

    /**
     * Salvage data from a file. The DB_AGGRESSIVE flag is being used (see berkeley DB->verify() method documentation).
     * key/value pairs are appended to salvagedData which are then written out to a new wallet file.
     * NOTE: reads the entire database into memory, so cannot be used
     * for huge databases.
     */
    std::vector<KeyValPair> salvagedData;

    std::stringstream strDump;

    Db db(env->dbenv.get(), 0);
    result = db.verify(newFilename.c_str(), nullptr, &strDump, DB_SALVAGE | DB_AGGRESSIVE);
    if (result == DB_VERIFY_BAD) {
        warnings.push_back(Untranslated("Salvage: Database salvage found errors, all data may not be recoverable."));
    }
    if (result != 0 && result != DB_VERIFY_BAD) {
        error = strprintf(Untranslated("Salvage: Database salvage failed with result %d."), result);
        return false;
    }

    // Format of bdb dump is ascii lines:
    // header lines...
    // HEADER=END
    //  hexadecimal key
    //  hexadecimal value
    //  ... repeated
    // DATA=END

    std::string strLine;
    while (!strDump.eof() && strLine != HEADER_END)
        getline(strDump, strLine); // Skip past header

    std::string keyHex, valueHex;
    while (!strDump.eof() && keyHex != DATA_END) {
        getline(strDump, keyHex);
        if (keyHex != DATA_END) {
            if (strDump.eof())
                break;
            getline(strDump, valueHex);
            if (valueHex == DATA_END) {
                warnings.push_back(Untranslated("Salvage: WARNING: Number of keys in data does not match number of values."));
                break;
            }
            salvagedData.push_back(make_pair(ParseHex(keyHex), ParseHex(valueHex)));
        }
    }

    bool fSuccess;
    if (keyHex != DATA_END) {
        warnings.push_back(Untranslated("Salvage: WARNING: Unexpected end of file while reading salvage output."));
        fSuccess = false;
    } else {
        fSuccess = (result == 0);
    }

    if (salvagedData.empty())
    {
        error = strprintf(Untranslated("Salvage(aggressive) found no records in %s."), newFilename);
        return false;
    }

    // The new file is written through an environment of its own, whose cache
    // never held a page of the old file: see BerkeleyScratchEnvironment.
    BerkeleyScratchEnvironment scratch(env->Directory());
    if (!scratch.IsOpen()) {
        error = strprintf(Untranslated("Cannot create database file %s"), filename);
        return false;
    }
    std::unique_ptr<Db> pdbCopy = std::make_unique<Db>(scratch.Get(), 0);
    int ret = pdbCopy->open(nullptr,               // Txn pointer
                            filename.c_str(),   // Filename
                            "main",             // Logical db name
                            DB_BTREE,           // Database type
                            DB_CREATE,          // Flags
                            0);
    // Berkeley DB returns its own errors as negative numbers: any value but 0 is a failure.
    if (ret != 0) {
        error = strprintf(Untranslated("Cannot create database file %s"), filename);
        pdbCopy->close(0);
        return false;
    }

    CWallet dummyWallet(nullptr, "", CreateDummyWalletDatabase());
    bool kept_hd_secrets{false};
    bool kept_hd_accounts{false};
    // The flags and the version the wallet requires are written once, at the
    // end, merged with what the records kept require.
    std::optional<uint64_t> kept_flags;
    std::optional<int> kept_min_version;
    for (KeyValPair& row : salvagedData)
    {
        /* Filter for only private key type KV pairs to be added to the salvaged wallet */
        CDataStream ssKey(row.first, SER_DISK, CLIENT_VERSION);
        CDataStream ssValue(row.second, SER_DISK, CLIENT_VERSION);
        std::string strType, strErr;
        bool fReadOK;
        {
            // Required in LoadKeyMetadata():
            LOCK(dummyWallet.cs_wallet);
            fReadOK = ReadKeyValue(&dummyWallet, ssKey, ssValue, strType, strErr, KeyFilter);
        }
        if (strType == DBKeys::FLAGS || strType == DBKeys::MINVERSION) {
            try {
                CDataStream value(row.second, SER_DISK, CLIENT_VERSION);
                if (strType == DBKeys::FLAGS) {
                    uint64_t flags;
                    value >> flags;
                    kept_flags = flags;
                } else {
                    int min_version;
                    value >> min_version;
                    kept_min_version = min_version;
                }
            } catch (const std::exception&) {
                warnings.push_back(strprintf(Untranslated("WARNING: WalletBatch::Recover skipping %s: unreadable"), strType));
            }
            continue;
        }
        if (!KeyFilter(strType)) {
            continue;
        }
        if (!fReadOK)
        {
            warnings.push_back(strprintf(Untranslated("WARNING: WalletBatch::Recover skipping %s: %s"), strType, strErr));
            continue;
        }
        Dbt datKey(row.first.data(), row.first.size());
        Dbt datValue(row.second.data(), row.second.size());
        int ret2 = pdbCopy->put(nullptr, &datKey, &datValue, DB_NOOVERWRITE);
        if (ret2 > 0)
            fSuccess = false;
        if (strType == DBKeys::HDSECRETS) kept_hd_secrets = true;
        if (strType == DBKeys::HDACCOUNT || strType == DBKeys::HDACCOUNTMETA) kept_hd_accounts = true;
    }

    // The flags of the wallet are kept, with what the records kept require:
    // the flag that says the secrets of the chain are in their own record,
    // or a version that does not know the record would load the wallet as
    // one whose chain has no seed; and the flag of HD accounts, or a version
    // that does not know them would spend the coins of an account with those
    // of account 0. A wallet with accounts requires the version that knows them.
    uint64_t flags{kept_flags.value_or(0)};
    if (kept_hd_secrets) flags |= WALLET_FLAG_HD_SECRETS_ENCRYPTED;
    if (kept_hd_accounts) flags |= WALLET_FLAG_HD_ACCOUNTS;
    std::optional<int> min_version{kept_min_version};
    if (kept_hd_accounts) min_version = std::max(min_version.value_or(0), int{FEATURE_HD_ACCOUNTS});
    const auto put = [&](const std::string& type, const auto& data) {
        CDataStream key(SER_DISK, CLIENT_VERSION);
        CDataStream value(SER_DISK, CLIENT_VERSION);
        key << type;
        value << data;
        Dbt datKey(key.data(), key.size());
        Dbt datValue(value.data(), value.size());
        if (pdbCopy->put(nullptr, &datKey, &datValue, DB_NOOVERWRITE) > 0) fSuccess = false;
    };
    if (kept_flags || flags != 0) put(DBKeys::FLAGS, flags);
    if (min_version) put(DBKeys::MINVERSION, *min_version);
    if (pdbCopy->close(0) != 0) fSuccess = false;

    // The original is still there under another name. If it is the file of an
    // encrypted wallet that has its seed in the chain record, as earlier
    // versions left it, that copy holds the seed without encryption.
    if (LegacyScriptPubKeyMan* spk_man = dummyWallet.GetLegacyScriptPubKeyMan()) {
        const CHDChain& chain = spk_man->GetHDChain();
        if (dummyWallet.IsCrypted() && chain.nVersion == CHDChain::VERSION_HD_BIP39 && !chain.vchSeed.empty()) {
            warnings.push_back(strprintf(Untranslated("WARNING: the original file was kept as %s. It holds the seed of this wallet without encryption, as does the salvaged file until the wallet is next unlocked. Destroy the copy once the salvaged wallet works."), newFilename));
        }
    }

    return fSuccess;
}
