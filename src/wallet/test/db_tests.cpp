// Copyright (c) 2018-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <fs.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/translation.h>
#include <wallet/bdb.h>
#include <wallet/walletdb.h>


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

// The memory Berkeley DB is given is zero, whatever the heap held before.
BOOST_AUTO_TEST_CASE(berkeley_allocator_gives_zeroed_memory)
{
    for (const size_t size : {size_t{1}, size_t{33}, size_t{4096}, size_t{70000}}) {
        // Leave the heap dirty with blocks of this size, taken and given back with malloc itself.
        std::vector<void*> dirty;
        for (int i = 0; i < 16; ++i) {
            dirty.push_back(std::malloc(size + 64));
            BOOST_REQUIRE(dirty.back());
            std::memset(dirty.back(), 0xA5, size + 64);
        }
        for (void* block : dirty) std::free(block);

        std::vector<unsigned char*> blocks;
        for (int i = 0; i < 16; ++i) {
            unsigned char* block = static_cast<unsigned char*>(BerkeleyAllocator::Allocate(size));
            BOOST_REQUIRE(block);
            BOOST_CHECK(std::all_of(block, block + size, [](unsigned char byte) { return byte == 0; }));
            BOOST_CHECK_EQUAL(reinterpret_cast<uintptr_t>(block) % alignof(std::max_align_t), 0U);
            std::memset(block, 0x5A, size);
            blocks.push_back(block);
        }
        for (unsigned char* block : blocks) BerkeleyAllocator::Release(block);
    }
    BerkeleyAllocator::Release(nullptr);
    BOOST_CHECK(BerkeleyAllocator::Allocate(std::numeric_limits<size_t>::max()) == nullptr);
}

namespace {
size_t g_released_blocks;
size_t g_released_bytes;
bool g_released_clean;
void ObserveRelease(const void* ptr, size_t size)
{
    const unsigned char* bytes = static_cast<const unsigned char*>(ptr);
    ++g_released_blocks;
    g_released_bytes += size;
    g_released_clean = g_released_clean && std::all_of(bytes, bytes + size, [](unsigned char byte) { return byte == 0; });
}
} // namespace

// Memory given back is cleansed first, and a block that is resized keeps its
// content, gains zeros, and leaves nothing behind where it was.
BOOST_AUTO_TEST_CASE(berkeley_allocator_cleanses_what_it_releases)
{
    g_released_blocks = 0;
    g_released_bytes = 0;
    g_released_clean = true;
    BerkeleyAllocator::g_release_observer = ObserveRelease;

    unsigned char* block = static_cast<unsigned char*>(BerkeleyAllocator::Allocate(1000));
    BOOST_REQUIRE(block);
    std::memset(block, 0x5A, 1000);
    BerkeleyAllocator::Release(block);
    BOOST_CHECK_EQUAL(g_released_blocks, 1U);
    BOOST_CHECK_EQUAL(g_released_bytes, 1000U);
    BOOST_CHECK(g_released_clean);

    // Grown: the old content, then zeros; the old block cleansed.
    block = static_cast<unsigned char*>(BerkeleyAllocator::Reallocate(nullptr, 100));
    BOOST_REQUIRE(block);
    for (size_t i = 0; i < 100; ++i) block[i] = static_cast<unsigned char>(i + 1);
    unsigned char* grown = static_cast<unsigned char*>(BerkeleyAllocator::Reallocate(block, 5000));
    BOOST_REQUIRE(grown);
    for (size_t i = 0; i < 100; ++i) BOOST_CHECK_EQUAL(grown[i], static_cast<unsigned char>(i + 1));
    BOOST_CHECK(std::all_of(grown + 100, grown + 5000, [](unsigned char byte) { return byte == 0; }));
    BOOST_CHECK_EQUAL(g_released_blocks, 2U);
    BOOST_CHECK_EQUAL(g_released_bytes, 1100U);

    // Shrunk: the content that fits; the whole old block cleansed.
    std::memset(grown + 100, 0x77, 4900);
    unsigned char* shrunk = static_cast<unsigned char*>(BerkeleyAllocator::Reallocate(grown, 10));
    BOOST_REQUIRE(shrunk);
    for (size_t i = 0; i < 10; ++i) BOOST_CHECK_EQUAL(shrunk[i], static_cast<unsigned char>(i + 1));
    BOOST_CHECK_EQUAL(g_released_blocks, 3U);
    BOOST_CHECK_EQUAL(g_released_bytes, 6100U);
    BerkeleyAllocator::Release(shrunk);
    BOOST_CHECK_EQUAL(g_released_blocks, 4U);
    BOOST_CHECK(g_released_clean);

    BerkeleyAllocator::g_release_observer = nullptr;
}

// Berkeley DB has the functions before its first environment exists, and what
// it reads into memory for the wallet to release still comes from malloc.
BOOST_AUTO_TEST_CASE(berkeley_allocator_is_installed_and_used)
{
    g_released_blocks = 0;
    g_released_clean = true;
    BerkeleyAllocator::g_release_observer = ObserveRelease;
    {
        std::unique_ptr<WalletDatabase> database = CreateMockWalletDatabase();
        BOOST_CHECK(BerkeleyAllocator::IsInstalled());
        std::unique_ptr<DatabaseBatch> batch = database->MakeBatch();
        const std::string value(3000, 'v');
        for (int i = 0; i < 50; ++i) BOOST_REQUIRE(batch->Write(strprintf("key%d", i), value));
        std::string read;
        for (int i = 0; i < 50; ++i) BOOST_REQUIRE(batch->Read(strprintf("key%d", i), read));
        BOOST_CHECK_EQUAL(read, value);
    }
    // The library released memory through the functions while it worked and when it closed.
    BOOST_CHECK_GT(g_released_blocks, 0U);
    BOOST_CHECK(g_released_clean);
    BerkeleyAllocator::g_release_observer = nullptr;
}

BOOST_AUTO_TEST_SUITE_END()
