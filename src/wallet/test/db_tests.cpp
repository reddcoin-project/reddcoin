// Copyright (c) 2018-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <fstream>
#include <memory>
#include <string>

#include <boost/test/unit_test.hpp>

#include <fs.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/translation.h>
#include <wallet/bdb.h>


BOOST_FIXTURE_TEST_SUITE(db_tests, BasicTestingSetup)

static std::shared_ptr<BerkeleyEnvironment> GetWalletEnv(const fs::path& path, std::string& database_filename)
{
    fs::path data_file = BDBDataFile(path);
    database_filename = data_file.filename().string();
    return GetBerkeleyEnv(data_file.parent_path());
}

BOOST_AUTO_TEST_CASE(getwalletenv_file)
{
    std::string test_name = "test_name.dat";
    const fs::path datadir = gArgs.GetDataDirNet();
    fs::path file_path = datadir / test_name;
#if BOOST_VERSION >= 107700
    std::ofstream f(BOOST_FILESYSTEM_C_STR(file_path));
#else
    std::ofstream f(file_path.BOOST_FILESYSTEM_C_STR);
#endif // BOOST_VERSION >= 107700
    f.close();

    std::string filename;
    std::shared_ptr<BerkeleyEnvironment> env = GetWalletEnv(file_path, filename);
    BOOST_CHECK_EQUAL(filename, test_name);
    BOOST_CHECK_EQUAL(env->Directory(), datadir);
}

BOOST_AUTO_TEST_CASE(getwalletenv_directory)
{
    std::string expected_name = "wallet.dat";
    const fs::path datadir = gArgs.GetDataDirNet();

    std::string filename;
    std::shared_ptr<BerkeleyEnvironment> env = GetWalletEnv(datadir, filename);
    BOOST_CHECK_EQUAL(filename, expected_name);
    BOOST_CHECK_EQUAL(env->Directory(), datadir);
}

BOOST_AUTO_TEST_CASE(getwalletenv_g_dbenvs_multiple)
{
    fs::path datadir = gArgs.GetDataDirNet() / "1";
    fs::path datadir_2 = gArgs.GetDataDirNet() / "2";
    std::string filename;

    std::shared_ptr<BerkeleyEnvironment> env_1 = GetWalletEnv(datadir, filename);
    std::shared_ptr<BerkeleyEnvironment> env_2 = GetWalletEnv(datadir, filename);
    std::shared_ptr<BerkeleyEnvironment> env_3 = GetWalletEnv(datadir_2, filename);

    BOOST_CHECK(env_1 == env_2);
    BOOST_CHECK(env_2 != env_3);
}

BOOST_AUTO_TEST_CASE(getwalletenv_g_dbenvs_free_instance)
{
    fs::path datadir = gArgs.GetDataDirNet() / "1";
    fs::path datadir_2 = gArgs.GetDataDirNet() / "2";
    std::string filename;

    std::shared_ptr <BerkeleyEnvironment> env_1_a = GetWalletEnv(datadir, filename);
    std::shared_ptr <BerkeleyEnvironment> env_2_a = GetWalletEnv(datadir_2, filename);
    env_1_a.reset();

    std::shared_ptr<BerkeleyEnvironment> env_1_b = GetWalletEnv(datadir, filename);
    std::shared_ptr<BerkeleyEnvironment> env_2_b = GetWalletEnv(datadir_2, filename);

    BOOST_CHECK(env_1_a != env_1_b);
    BOOST_CHECK(env_2_a == env_2_b);
}

//! Count the pages of a Berkeley DB file and those among them whose log
//! sequence number is set, reading the file itself and not through Berkeley
//! DB. A page with one set still refers to the log of the environment that
//! wrote it.
static void CountPages(const fs::path& file, int64_t& pages, int64_t& pages_with_lsn)
{
    pages = 0;
    pages_with_lsn = 0;
    std::ifstream f(file.string(), std::ios::binary);
    // The metadata page starts with its LSN, page number, magic and version;
    // the page size is the sixth 32-bit field.
    uint32_t meta[6];
    BOOST_REQUIRE(f.read(reinterpret_cast<char*>(meta), sizeof(meta)));
    const uint32_t page_size = meta[5];
    BOOST_REQUIRE(page_size >= 512);
    for (;; ++pages) {
        // Every page starts with its LSN: a file number and an offset.
        // File 0, offset 1 is how a page that is not logged is marked.
        uint32_t lsn[2];
        f.seekg(pages * static_cast<int64_t>(page_size));
        if (!f.read(reinterpret_cast<char*>(lsn), sizeof(lsn))) break;
        if (lsn[0] != 0 || lsn[1] != 1) ++pages_with_lsn;
    }
}

static std::unique_ptr<BerkeleyDatabase> MakeDatabase(const fs::path& dir)
{
    fs::create_directories(dir);
    DatabaseOptions options;
    DatabaseStatus status;
    bilingual_str error;
    return MakeBerkeleyDatabase(dir, options, status, error);
}

static void WriteRecords(BerkeleyDatabase& database, int first, int count)
{
    std::unique_ptr<DatabaseBatch> batch = database.MakeBatch();
    for (int i = first; i < first + count; ++i) {
        BOOST_REQUIRE(batch->Write(strprintf("key%08d", i), std::string(500, 'x')));
    }
}

//! The periodic flush leaves the file with no page referring to the log, as
//! it always did, and writes only the pages changed since the flush before.
BOOST_AUTO_TEST_CASE(periodic_flush_resets_changed_pages_only)
{
    const fs::path dir = gArgs.GetDataDirNet() / "flush_source";
    std::unique_ptr<BerkeleyDatabase> database = MakeDatabase(dir);
    BOOST_REQUIRE(database);
    const fs::path file = dir / "wallet.dat";
    BerkeleyEnvironment& env = *database->env;
    int64_t pages;
    int64_t pages_with_lsn;

    // Closing the handle and checkpointing puts the changes in the file, and
    // on its own leaves the pages they touched referring to the log. This is
    // what the reset is for; if it did not hold, the checks below would pass
    // whatever the reset did.
    WriteRecords(*database, 0, 4000);
    env.CloseDb(database->strFile);
    env.dbenv->txn_checkpoint(0, 0, 0);
    CountPages(file, pages, pages_with_lsn);
    BOOST_CHECK(pages > 100);
    BOOST_CHECK(pages_with_lsn > 100);

    // The reset clears every one of them.
    const int64_t reset_after_load = env.CheckpointResetChangedLSNs(database->strFile);
    CountPages(file, pages, pages_with_lsn);
    BOOST_CHECK_EQUAL(pages_with_lsn, 0);

    // After one more record the file is clear again, and few pages were
    // written to get there. A return of -1 means this build resets every
    // page, as it must with a Berkeley DB the page-by-page reset is not
    // enabled for; the state of the file is checked either way.
    WriteRecords(*database, 4000, 1);
    env.CloseDb(database->strFile);
    const int64_t reset_after_write = env.CheckpointResetChangedLSNs(database->strFile);
    CountPages(file, pages, pages_with_lsn);
    BOOST_CHECK_EQUAL(pages_with_lsn, 0);
#if DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR == 8
    // This is the version it is enabled for, so it must not have fallen back.
    BOOST_CHECK(reset_after_write >= 0);
#endif
    if (reset_after_write >= 0) {
        BOOST_CHECK(reset_after_load > 100);
        BOOST_CHECK(reset_after_write >= 1);
        BOOST_CHECK(reset_after_write * 20 < pages);

        // With nothing written in between there is nothing to reset.
        BOOST_CHECK_EQUAL(env.CheckpointResetChangedLSNs(database->strFile), 0);
    }

    // The same through the flush the scheduler calls.
    WriteRecords(*database, 4001, 1);
    BOOST_CHECK(database->PeriodicFlush());
    CountPages(file, pages, pages_with_lsn);
    BOOST_CHECK_EQUAL(pages_with_lsn, 0);

    // The file on its own, without the log it was written under, opens in
    // another environment and takes a write to the page last changed.
    const fs::path copy_dir = gArgs.GetDataDirNet() / "flush_copy";
    fs::create_directories(copy_dir);
    fs::copy_file(file, copy_dir / "wallet.dat");
    std::unique_ptr<BerkeleyDatabase> copy = MakeDatabase(copy_dir);
    BOOST_REQUIRE(copy);
    {
        std::unique_ptr<DatabaseBatch> batch = copy->MakeBatch();
        std::string value;
        BOOST_CHECK(batch->Read(strprintf("key%08d", 4001), value));
        BOOST_CHECK_EQUAL(value, std::string(500, 'x'));
        BOOST_CHECK(batch->Write(strprintf("key%08d", 4001), std::string("changed")));
        BOOST_CHECK(batch->Write(strprintf("key%08d", 4002), std::string(500, 'x')));
    }
    BOOST_CHECK(copy->PeriodicFlush());
    CountPages(copy_dir / "wallet.dat", pages, pages_with_lsn);
    BOOST_CHECK_EQUAL(pages_with_lsn, 0);
    {
        std::unique_ptr<DatabaseBatch> batch = copy->MakeBatch();
        std::string value;
        BOOST_CHECK(batch->Read(strprintf("key%08d", 4001), value));
        BOOST_CHECK_EQUAL(value, "changed");
        BOOST_CHECK(batch->Read(strprintf("key%08d", 0), value));
    }
}

BOOST_AUTO_TEST_SUITE_END()
