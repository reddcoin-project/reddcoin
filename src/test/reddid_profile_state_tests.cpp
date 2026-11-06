// Copyright (c) 2025 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include <hash.h>
#include <id/profile.h>
#include <id/reddid.h>
#include <id/reddid_db.h>
#include <id/transaction_handler.h>
#include <key.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <streams.h>
#include <test/reddid_auction_test_util.h>
#include <test/reddid_profile_test_util.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <util/time.h>
#include <version.h>

#include <map>
#include <string>
#include <vector>

using namespace reddid_test;

namespace {
/** A transaction with one output for each of the given scripts, spending an output nobody has. */
CTransaction TxWith(const std::vector<CScript>& scripts)
{
    CMutableTransaction mtx;
    mtx.vin.emplace_back(COutPoint(InsecureRand256(), 0));
    for (const CScript& script : scripts) {
        mtx.vout.emplace_back(0, script);
    }
    return CTransaction(mtx);
}

CTransaction ProfileUpdateTx(const std::string& reddId, const uint256& profileHash = uint256())
{
    return TxWith({CreateProfileUpdateOpReturn(reddId, profileHash)});
}

CTransaction ConnectionTx(const std::string& from, const std::string& to, SocialConnectionType type = CONNECTION_FOLLOW)
{
    return TxWith({CreateConnectionOpReturn(from, to, type)});
}
} // namespace

/**
 * The state that ProfileManager keeps, apart from the reputation formula:
 * the profile and connection records in transactions, connections, namespace
 * resolutions, names, lookups by owner, expiry and the profile hash.
 */
BOOST_FIXTURE_TEST_SUITE(reddid_profile_state_tests, ProfileTestingSetup)

// ===================================================================
// Category 1: ProcessTransaction
// ===================================================================

// SUSPECTED BUG (documented, not fixed): a profile update record changes
// nothing but lastActivityTime. The profile hash that it carries is not looked
// at, and the time is the clock of the node and not the time of the block, so
// that two nodes that connect the same block at different times disagree.
BOOST_AUTO_TEST_CASE(profile_update_record_only_bumps_last_activity_time)
{
    ReddIDProfile created = AddProfile("alice");

    SetMockTime(T0 + 5 * DAY);
    BOOST_CHECK(pMgr->ProcessTransaction(ProfileUpdateTx("alice", uint256S("bad")), 7));

    ReddIDProfile profile = GetProfile("alice");
    BOOST_CHECK_EQUAL(profile.lastActivityTime, T0 + 5 * DAY);
    BOOST_CHECK_EQUAL(profile.lastUpdated, T0);
    BOOST_CHECK_EQUAL(profile.creationTime, T0);
    BOOST_CHECK_EQUAL(profile.displayName, created.displayName);
    BOOST_CHECK(profile.owner == created.owner);
    BOOST_CHECK_EQUAL(GetStoredProfile("alice").lastActivityTime, T0 + 5 * DAY);
}

// SUSPECTED BUG (documented, not fixed): nothing ties the record to the owner
// of the profile. Anybody can keep the profile of somebody else from decaying.
BOOST_AUTO_TEST_CASE(profile_update_record_has_no_ownership_check)
{
    AddProfile("alice");
    CKeyID stranger = MakeKeyID();

    SetMockTime(T0 + 100 * DAY);
    CMutableTransaction mtx = TxFrom(stranger, CreateProfileUpdateOpReturn("alice", uint256()));
    BOOST_CHECK(pMgr->ProcessTransaction(CTransaction(mtx), 7));
    BOOST_CHECK_EQUAL(GetProfile("alice").lastActivityTime, T0 + 100 * DAY);
}

BOOST_AUTO_TEST_CASE(profile_update_record_for_unknown_profile_creates_nothing)
{
    BOOST_CHECK(pMgr->ProcessTransaction(ProfileUpdateTx("nobody"), 7));
    BOOST_CHECK(!pMgr->ProfileExists("nobody"));
    ReddIDProfile profile;
    BOOST_CHECK(!pMgr->GetProfile("nobody", profile));
    BOOST_CHECK(!db->ExistsProfile("nobody"));
}

// SUSPECTED BUG (documented, not fixed): a connection record creates no
// connection. It bumps lastActivityTime of the source profile, with the clock
// of the node and without an ownership check.
BOOST_AUTO_TEST_CASE(connection_record_only_bumps_last_activity_time_of_the_source)
{
    AddProfile("alice");
    AddProfile("bob");

    SetMockTime(T0 + 3 * DAY);
    BOOST_CHECK(pMgr->ProcessTransaction(ConnectionTx("alice", "bob", CONNECTION_FRIEND), 7));

    BOOST_CHECK_EQUAL(GetProfile("alice").lastActivityTime, T0 + 3 * DAY);
    BOOST_CHECK_EQUAL(GetStoredProfile("alice").lastActivityTime, T0 + 3 * DAY);
    BOOST_CHECK_EQUAL(GetProfile("bob").lastActivityTime, T0);
    BOOST_CHECK(pMgr->GetConnections("alice").empty());
    ReddIDConnection connection;
    BOOST_CHECK(!pMgr->GetConnection("alice", "bob", connection));

    // The target need not exist, and neither need the source
    BOOST_CHECK(pMgr->ProcessTransaction(ConnectionTx("alice", "nobody"), 7));
    BOOST_CHECK(pMgr->ProcessTransaction(ConnectionTx("nobody", "alice"), 7));
    BOOST_CHECK(!pMgr->ProfileExists("nobody"));
}

// Current behaviour: a malformed profile or connection record returns true
// like a well-formed one, and a profile update record needs no hash.
BOOST_AUTO_TEST_CASE(process_transaction_return_value)
{
    AddProfile("alice");
    SetMockTime(T0 + DAY);

    // No output, no record, not a ReddID record, a record of another manager
    BOOST_CHECK(!pMgr->ProcessTransaction(TxWith({}), 7));
    BOOST_CHECK(!pMgr->ProcessTransaction(TxWith({P2PKH(MakeKeyID())}), 7));
    BOOST_CHECK(!pMgr->ProcessTransaction(TxWith({CScript() << OP_RETURN}), 7));
    BOOST_CHECK(!pMgr->ProcessTransaction(TxWith({CScript() << OP_RETURN << std::vector<unsigned char>{'X', '#', 5, 'a', 'l', 'i', 'c', 'e'}}), 7));
    BOOST_CHECK(!pMgr->ProcessTransaction(TxWith({RecordScript(OP_BID_DEPOSIT, std::vector<unsigned char>{1, 2, 3})}), 7));
    // 'R' and the opcode alone are too short to be looked at
    BOOST_CHECK(!pMgr->ProcessTransaction(TxWith({RecordScript(OP_REDDID_PROFILE_UPDATE, std::vector<unsigned char>{})}), 7));

    // The three auction opcodes are accepted and ignored
    for (unsigned char opCode : {OP_REDDID_AUCTION_CREATE, OP_REDDID_AUCTION_BID, OP_REDDID_AUCTION_FINALIZE}) {
        BOOST_CHECK(pMgr->ProcessTransaction(TxWith({RecordScript(opCode, std::vector<unsigned char>{1, 2, 3})}), 7));
    }

    // Current behaviour: malformed profile and connection records are accepted and ignored
    BOOST_CHECK(pMgr->ProcessTransaction(TxWith({RecordScript(OP_REDDID_PROFILE_UPDATE, std::vector<unsigned char>{0})}), 7));
    BOOST_CHECK(pMgr->ProcessTransaction(TxWith({RecordScript(OP_REDDID_PROFILE_UPDATE, std::vector<unsigned char>{9, 'a', 'l', 'i', 'c', 'e'})}), 7));
    BOOST_CHECK(pMgr->ProcessTransaction(TxWith({RecordScript(OP_REDDID_CONNECTION, std::vector<unsigned char>{0})}), 7));
    BOOST_CHECK(pMgr->ProcessTransaction(TxWith({RecordScript(OP_REDDID_CONNECTION, std::vector<unsigned char>{9, 'a', 'l', 'i', 'c', 'e'})}), 7));

    BOOST_CHECK_EQUAL(GetProfile("alice").lastActivityTime, T0);

    // Current behaviour: a profile update record needs no hash after the name
    BOOST_CHECK(pMgr->ProcessTransaction(TxWith({RecordScript(OP_REDDID_PROFILE_UPDATE, std::vector<unsigned char>{5, 'a', 'l', 'i', 'c', 'e'})}), 7));
    BOOST_CHECK_EQUAL(GetProfile("alice").lastActivityTime, T0 + DAY);
}

// Current behaviour: processing stops at the first record it recognizes
BOOST_AUTO_TEST_CASE(process_transaction_handles_the_first_record_only)
{
    AddProfile("alice");
    AddProfile("bob");
    AddProfile("carol");
    SetMockTime(T0 + DAY);

    CTransaction tx = TxWith({P2PKH(MakeKeyID()),
                              RecordScript(OP_BID_DEPOSIT, std::vector<unsigned char>{1, 2, 3}),
                              CreateProfileUpdateOpReturn("alice", uint256()),
                              CreateProfileUpdateOpReturn("bob", uint256()),
                              CreateConnectionOpReturn("carol", "alice", CONNECTION_FOLLOW)});
    BOOST_CHECK(pMgr->ProcessTransaction(tx, 7));

    BOOST_CHECK_EQUAL(GetProfile("alice").lastActivityTime, T0 + DAY);
    BOOST_CHECK_EQUAL(GetProfile("bob").lastActivityTime, T0);
    BOOST_CHECK_EQUAL(GetProfile("carol").lastActivityTime, T0);
}

// ===================================================================
// Category 2: UndoTransaction
// ===================================================================

// SUSPECTED BUG (documented, not fixed): undoing a profile update record does
// not restore the previous lastActivityTime, it erases the profile, from
// memory and from the database. Processing a record and undoing it again, as a
// reorganization does, destroys a profile that existed before the record. The
// reputation and the connections of the profile stay behind.
BOOST_AUTO_TEST_CASE(undo_of_profile_update_record_erases_the_profile)
{
    AddProfile("alice");
    AddProfile("bob");
    Connect("alice", "bob", CONNECTION_FOLLOW);
    Connect("bob", "alice", CONNECTION_FRIEND);

    SetMockTime(T0 + DAY);
    CTransaction tx = ProfileUpdateTx("alice");
    BOOST_REQUIRE(pMgr->ProcessTransaction(tx, 7));
    BOOST_REQUIRE(pMgr->ProfileExists("alice"));

    BOOST_CHECK(pMgr->UndoTransaction(tx, 7));

    BOOST_CHECK(!pMgr->ProfileExists("alice"));
    ReddIDProfile profile;
    BOOST_CHECK(!pMgr->GetProfile("alice", profile));
    BOOST_CHECK(!db->ExistsProfile("alice"));
    BOOST_CHECK(!pMgr->IsProfileActive("alice"));
    BOOST_CHECK(pMgr->ProfileExists("bob"));

    // What belonged to the profile is still there
    ReddIDReputation reputation;
    BOOST_CHECK(pMgr->GetReputation("alice", reputation));
    BOOST_CHECK(db->ReadReputation("alice", reputation));
    BOOST_CHECK_EQUAL(pMgr->GetConnections("alice").size(), 1U);
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("alice", CONNECTION_FRIEND), 1);
    BOOST_CHECK_EQUAL(pMgr->CountIncomingByType("bob", CONNECTION_FOLLOW), 1);

    // Anybody can now create the profile again, and takes over its connections
    CKeyID newOwner = MakeKeyID();
    BOOST_CHECK(pMgr->CreateProfile(TestProfile("alice", newOwner)));
    BOOST_CHECK(pMgr->IsProfileOwner("alice", newOwner));
    BOOST_CHECK_EQUAL(pMgr->GetConnections("alice").size(), 1U);

    // Undoing a record for a profile that does not exist changes nothing
    BOOST_CHECK(pMgr->UndoTransaction(ProfileUpdateTx("nobody"), 7));
    BOOST_CHECK(pMgr->ProfileExists("alice"));
    BOOST_CHECK(pMgr->ProfileExists("bob"));
}

// SUSPECTED BUG (documented, not fixed): processing a connection record
// creates no connection, but undoing it erases the connection between the two
// profiles, however that connection came about.
BOOST_AUTO_TEST_CASE(undo_of_connection_record_erases_the_connection)
{
    AddProfile("alice");
    AddProfile("bob");
    AddProfile("carol");
    Connect("alice", "bob", CONNECTION_FRIEND);
    Connect("alice", "carol", CONNECTION_FOLLOW);
    Connect("bob", "alice", CONNECTION_FOLLOW);

    // The type in the record is not compared with the type of the connection
    CTransaction tx = ConnectionTx("alice", "bob", CONNECTION_BLOCK);
    BOOST_REQUIRE(pMgr->ProcessTransaction(tx, 7));
    ReddIDConnection connection;
    BOOST_REQUIRE(pMgr->GetConnection("alice", "bob", connection));
    BOOST_CHECK_EQUAL(connection.connectionType, CONNECTION_FRIEND);

    BOOST_CHECK(pMgr->UndoTransaction(tx, 7));

    BOOST_CHECK(!pMgr->GetConnection("alice", "bob", connection));
    BOOST_CHECK(!db->ReadConnection("alice", "bob", connection));
    BOOST_CHECK(pMgr->GetConnection("alice", "carol", connection));
    BOOST_CHECK(pMgr->GetConnection("bob", "alice", connection));
    BOOST_CHECK(pMgr->ProfileExists("alice"));
    BOOST_CHECK(pMgr->ProfileExists("bob"));

    // Undoing a record for a connection that does not exist changes nothing
    BOOST_CHECK(pMgr->UndoTransaction(tx, 7));
    BOOST_CHECK(pMgr->UndoTransaction(ConnectionTx("carol", "bob"), 7));
    BOOST_CHECK(pMgr->UndoTransaction(ConnectionTx("nobody", "bob"), 7));
    BOOST_CHECK_EQUAL(pMgr->GetConnections("alice").size(), 1U);
    BOOST_CHECK_EQUAL(pMgr->GetConnections("bob").size(), 1U);
}

// Current behaviour: UndoTransaction returns true whatever the transaction holds
BOOST_AUTO_TEST_CASE(undo_transaction_ignores_what_it_does_not_recognize)
{
    AddProfile("alice");
    AddProfile("bob");
    Connect("alice", "bob", CONNECTION_FOLLOW);

    BOOST_CHECK(pMgr->UndoTransaction(TxWith({}), 7));
    BOOST_CHECK(pMgr->UndoTransaction(TxWith({P2PKH(MakeKeyID())}), 7));
    BOOST_CHECK(pMgr->UndoTransaction(TxWith({CScript() << OP_RETURN}), 7));
    BOOST_CHECK(pMgr->UndoTransaction(TxWith({CScript() << OP_RETURN << std::vector<unsigned char>{'X', '#', 5, 'a', 'l', 'i', 'c', 'e'}}), 7));
    BOOST_CHECK(pMgr->UndoTransaction(TxWith({RecordScript(OP_BID_DEPOSIT, std::vector<unsigned char>{1, 2, 3})}), 7));
    for (unsigned char opCode : {OP_REDDID_AUCTION_CREATE, OP_REDDID_AUCTION_BID, OP_REDDID_AUCTION_FINALIZE}) {
        BOOST_CHECK(pMgr->UndoTransaction(TxWith({RecordScript(opCode, std::vector<unsigned char>{1, 2, 3})}), 7));
    }
    // A name longer than the record, and a connection record without a target
    BOOST_CHECK(pMgr->UndoTransaction(TxWith({RecordScript(OP_REDDID_PROFILE_UPDATE, std::vector<unsigned char>{9, 'a', 'l', 'i', 'c', 'e'})}), 7));
    BOOST_CHECK(pMgr->UndoTransaction(TxWith({RecordScript(OP_REDDID_CONNECTION, std::vector<unsigned char>{5, 'a', 'l', 'i', 'c', 'e'})}), 7));
    BOOST_CHECK(pMgr->UndoTransaction(TxWith({RecordScript(OP_REDDID_CONNECTION, std::vector<unsigned char>{5, 'a', 'l', 'i', 'c', 'e', 9, 'b', 'o', 'b'})}), 7));

    BOOST_CHECK(pMgr->ProfileExists("alice"));
    BOOST_CHECK(pMgr->ProfileExists("bob"));
    BOOST_CHECK_EQUAL(pMgr->GetConnections("alice").size(), 1U);
}

// Current behaviour: a malformed profile or connection record ends the undo of
// the whole transaction, so the records in later outputs are not undone. A
// well-formed one ends it too: only the first record is undone.
BOOST_AUTO_TEST_CASE(undo_transaction_stops_at_the_first_profile_or_connection_record)
{
    AddProfile("alice");
    AddProfile("bob");

    CTransaction malformedFirst = TxWith({RecordScript(OP_REDDID_PROFILE_UPDATE, std::vector<unsigned char>{0}),
                                          CreateProfileUpdateOpReturn("alice", uint256())});
    BOOST_CHECK(pMgr->UndoTransaction(malformedFirst, 7));
    BOOST_CHECK(pMgr->ProfileExists("alice"));

    CTransaction two = TxWith({CreateProfileUpdateOpReturn("alice", uint256()),
                               CreateProfileUpdateOpReturn("bob", uint256())});
    BOOST_CHECK(pMgr->UndoTransaction(two, 7));
    BOOST_CHECK(!pMgr->ProfileExists("alice"));
    BOOST_CHECK(pMgr->ProfileExists("bob"));
}

// Current behaviour: undoing a connection record does not need the type byte.
// A record that ends exactly after the name of the target is undone like a
// complete one (the length check is `offset + toLen > size`); a record whose
// target name is cut short by one byte is not.
BOOST_AUTO_TEST_CASE(undo_of_connection_record_without_the_type_byte)
{
    AddProfile("alice");
    AddProfile("bob");
    AddProfile("carol");
    Connect("alice", "bob", CONNECTION_FOLLOW);
    Connect("alice", "carol", CONNECTION_FOLLOW);

    // The name of the target is one byte shorter than its length says
    CTransaction cutShort = TxWith({RecordScript(OP_REDDID_CONNECTION, std::vector<unsigned char>{5, 'a', 'l', 'i', 'c', 'e', 5, 'c', 'a', 'r', 'o'})});
    BOOST_CHECK(pMgr->UndoTransaction(cutShort, 7));
    ReddIDConnection connection;
    BOOST_CHECK(pMgr->GetConnection("alice", "carol", connection));
    BOOST_CHECK(pMgr->GetConnection("alice", "bob", connection));

    // The record ends with the last byte of the name of the target
    CTransaction withoutType = TxWith({RecordScript(OP_REDDID_CONNECTION, std::vector<unsigned char>{5, 'a', 'l', 'i', 'c', 'e', 3, 'b', 'o', 'b'})});
    BOOST_CHECK(pMgr->UndoTransaction(withoutType, 7));
    BOOST_CHECK(!pMgr->GetConnection("alice", "bob", connection));
    BOOST_CHECK(!db->ReadConnection("alice", "bob", connection));
    BOOST_CHECK(pMgr->GetConnection("alice", "carol", connection));
    BOOST_CHECK_EQUAL(pMgr->GetConnections("alice").size(), 1U);
}

// Current behaviour: the three auction opcodes, which the profile manager
// accepts without doing anything, end the processing and the undo of the whole
// transaction. A profile update or connection record in a later output is
// neither processed nor undone.
BOOST_AUTO_TEST_CASE(auction_record_in_an_earlier_output_hides_the_later_records)
{
    AddProfile("alice");
    AddProfile("bob");
    Connect("alice", "bob", CONNECTION_FOLLOW);
    SetMockTime(T0 + DAY);

    const CScript profileUpdate = CreateProfileUpdateOpReturn("alice", uint256());
    const CScript connectionRecord = CreateConnectionOpReturn("alice", "bob", CONNECTION_FOLLOW);

    for (unsigned char opCode : {OP_REDDID_AUCTION_CREATE, OP_REDDID_AUCTION_BID, OP_REDDID_AUCTION_FINALIZE}) {
        const CScript auctionRecord = RecordScript(opCode, std::vector<unsigned char>{1, 2, 3});
        for (const CScript& later : {profileUpdate, connectionRecord}) {
            CTransaction tx = TxWith({auctionRecord, later});

            BOOST_CHECK(pMgr->ProcessTransaction(tx, 7));
            BOOST_CHECK_EQUAL(GetProfile("alice").lastActivityTime, T0);

            BOOST_CHECK(pMgr->UndoTransaction(tx, 7));
            BOOST_CHECK(pMgr->ProfileExists("alice"));
            ReddIDConnection connection;
            BOOST_CHECK(pMgr->GetConnection("alice", "bob", connection));
        }
    }

    // In the other order the profile update and the connection record take effect
    const CScript auctionRecord = RecordScript(OP_REDDID_AUCTION_CREATE, std::vector<unsigned char>{1, 2, 3});
    BOOST_CHECK(pMgr->ProcessTransaction(TxWith({profileUpdate, auctionRecord}), 7));
    BOOST_CHECK_EQUAL(GetProfile("alice").lastActivityTime, T0 + DAY);

    BOOST_CHECK(pMgr->UndoTransaction(TxWith({connectionRecord, auctionRecord}), 7));
    ReddIDConnection connection;
    BOOST_CHECK(!pMgr->GetConnection("alice", "bob", connection));
    BOOST_CHECK(pMgr->UndoTransaction(TxWith({profileUpdate, auctionRecord}), 7));
    BOOST_CHECK(!pMgr->ProfileExists("alice"));
}

// ===================================================================
// Category 3: Connections
// ===================================================================

BOOST_AUTO_TEST_CASE(create_connection_validates_the_connection)
{
    AddProfile("alice");
    AddProfile("bob");

    BOOST_CHECK(!pMgr->CreateConnection(TestConnection("alice", "alice", CONNECTION_FOLLOW)));
    BOOST_CHECK(!pMgr->CreateConnection(TestConnection("nobody", "bob", CONNECTION_FOLLOW)));
    BOOST_CHECK(!pMgr->CreateConnection(TestConnection("alice", "bob", static_cast<SocialConnectionType>(4))));

    ReddIDConnection connection = TestConnection("alice", "bob", CONNECTION_FOLLOW);
    connection.visibility = -1;
    BOOST_CHECK(!pMgr->CreateConnection(connection));
    connection.visibility = 3;
    BOOST_CHECK(!pMgr->CreateConnection(connection));
    BOOST_CHECK(pMgr->GetConnections("alice").empty());

    for (int visibility : {0, 1, 2}) {
        connection.visibility = visibility;
        BOOST_CHECK(pMgr->CreateConnection(connection));
    }
    // Each one replaced the one before
    BOOST_CHECK_EQUAL(pMgr->GetConnections("alice").size(), 1U);
    ReddIDConnection stored;
    BOOST_REQUIRE(db->ReadConnection("alice", "bob", stored));
    BOOST_CHECK_EQUAL(stored.visibility, 2);

    // An invalid replacement leaves the connection as it was
    connection.visibility = 3;
    BOOST_CHECK(!pMgr->CreateConnection(connection));
    BOOST_REQUIRE(pMgr->GetConnection("alice", "bob", stored));
    BOOST_CHECK_EQUAL(stored.visibility, 2);
}

// Current behaviour: the update overwrites the creation time of the connection
BOOST_AUTO_TEST_CASE(update_connection_replaces_an_existing_connection)
{
    AddProfile("alice");
    AddProfile("bob");
    AddProfile("carol");
    Connect("alice", "bob", CONNECTION_FOLLOW);
    Connect("alice", "carol", CONNECTION_FOLLOW);

    SetMockTime(T0 + DAY);
    ReddIDConnection update = TestConnection("alice", "bob", CONNECTION_FRIEND);
    update.visibility = 2;
    update.metadata = "met at a conference";
    update.txid = uint256S("c0");
    BOOST_CHECK(pMgr->UpdateConnection(update));

    for (bool fromDatabase : {false, true}) {
        ReddIDConnection connection;
        if (fromDatabase) {
            BOOST_REQUIRE(db->ReadConnection("alice", "bob", connection));
        } else {
            BOOST_REQUIRE(pMgr->GetConnection("alice", "bob", connection));
        }
        BOOST_CHECK_EQUAL(connection.connectionType, CONNECTION_FRIEND);
        BOOST_CHECK_EQUAL(connection.visibility, 2);
        BOOST_CHECK_EQUAL(connection.metadata, "met at a conference");
        BOOST_CHECK(connection.txid == uint256S("c0"));
        // Current behaviour: the whole record is replaced, the creation time included
        BOOST_CHECK_EQUAL(connection.creationTime, T0 + DAY);
    }

    ReddIDConnection other;
    BOOST_REQUIRE(pMgr->GetConnection("alice", "carol", other));
    BOOST_CHECK_EQUAL(other.connectionType, CONNECTION_FOLLOW);
    BOOST_CHECK_EQUAL(pMgr->GetConnections("alice").size(), 2U);
}

BOOST_AUTO_TEST_CASE(update_connection_fails_without_an_existing_connection)
{
    AddProfile("alice");
    AddProfile("bob");
    AddProfile("carol");

    // No source profile, a source profile without connections, no connection to the target
    BOOST_CHECK(!pMgr->UpdateConnection(TestConnection("nobody", "bob", CONNECTION_FOLLOW)));
    BOOST_CHECK(!pMgr->UpdateConnection(TestConnection("alice", "bob", CONNECTION_FOLLOW)));
    Connect("alice", "carol", CONNECTION_FOLLOW);
    BOOST_CHECK(!pMgr->UpdateConnection(TestConnection("alice", "bob", CONNECTION_FOLLOW)));
    // The opposite direction is another connection
    BOOST_CHECK(!pMgr->UpdateConnection(TestConnection("carol", "alice", CONNECTION_FOLLOW)));

    ReddIDConnection connection;
    BOOST_CHECK(!pMgr->GetConnection("alice", "bob", connection));
    BOOST_CHECK_EQUAL(pMgr->GetConnections("alice").size(), 1U);
}

// SUSPECTED BUG (documented, not fixed): unlike CreateConnection,
// UpdateConnection does not validate what it is given, and its ownership
// check compares the owner of the source profile with itself.
BOOST_AUTO_TEST_CASE(update_connection_does_not_validate)
{
    AddProfile("alice");
    AddProfile("bob");
    Connect("alice", "bob", CONNECTION_FOLLOW);

    ReddIDConnection update = TestConnection("alice", "bob", static_cast<SocialConnectionType>(9));
    update.visibility = 99;
    BOOST_CHECK(pMgr->UpdateConnection(update));

    ReddIDConnection connection;
    BOOST_REQUIRE(pMgr->GetConnection("alice", "bob", connection));
    BOOST_CHECK_EQUAL(static_cast<int>(connection.connectionType), 9);
    BOOST_CHECK_EQUAL(connection.visibility, 99);
    BOOST_REQUIRE(db->ReadConnection("alice", "bob", connection));
    BOOST_CHECK_EQUAL(connection.visibility, 99);
}

BOOST_AUTO_TEST_CASE(remove_connection_of_unknown_connection_fails)
{
    CKeyID alice = MakeKeyID();
    AddProfile("alice", alice);
    AddProfile("bob");
    AddProfile("carol");

    BOOST_CHECK(!pMgr->RemoveConnection("alice", "bob", alice));
    Connect("alice", "carol", CONNECTION_FOLLOW);
    BOOST_CHECK(!pMgr->RemoveConnection("alice", "bob", alice));
    BOOST_CHECK(pMgr->RemoveConnection("alice", "carol", alice));
    BOOST_CHECK(!pMgr->RemoveConnection("alice", "carol", alice));
    ReddIDConnection connection;
    BOOST_CHECK(!db->ReadConnection("alice", "carol", connection));
}

// ===================================================================
// Category 4: Namespace resolution
// ===================================================================

BOOST_AUTO_TEST_CASE(update_namespace_resolution_needs_an_existing_resolution)
{
    AddProfile("alice");

    BOOST_CHECK(!pMgr->UpdateNamespaceResolution("nobody", "redd", "al"));
    BOOST_CHECK(!pMgr->UpdateNamespaceResolution("alice", "redd", "al"));
    BOOST_CHECK_EQUAL(pMgr->ResolveNamespace("alice", "redd"), "");
    BOOST_CHECK_EQUAL(pMgr->ResolveReddID("al", "redd"), "");

    BOOST_REQUIRE(pMgr->AddNamespaceResolution("alice", "redd", "al"));
    // A resolution in another namespace is another resolution
    BOOST_CHECK(!pMgr->UpdateNamespaceResolution("alice", "crypto", "al"));

    BOOST_CHECK(pMgr->UpdateNamespaceResolution("alice", "redd", "alice1"));
    BOOST_CHECK_EQUAL(pMgr->ResolveNamespace("alice", "redd"), "alice1");
    BOOST_CHECK_EQUAL(pMgr->ResolveNamespace("alice", "crypto"), "");
    std::string stored;
    BOOST_REQUIRE(db->ReadResolution("alice", "redd", stored));
    BOOST_CHECK_EQUAL(stored, "alice1");
    BOOST_CHECK_EQUAL(pMgr->ResolveReddID("alice1", "redd"), "alice");
}

// SUSPECTED BUG (documented, not fixed): changing a resolution writes the new
// reverse entry and leaves the old one, so the user ID that the profile no
// longer resolves to still resolves to the profile.
BOOST_AUTO_TEST_CASE(update_namespace_resolution_leaves_the_old_reverse_entry)
{
    AddProfile("alice");
    BOOST_REQUIRE(pMgr->AddNamespaceResolution("alice", "redd", "al"));
    BOOST_CHECK_EQUAL(pMgr->ResolveReddID("al", "redd"), "alice");

    BOOST_REQUIRE(pMgr->UpdateNamespaceResolution("alice", "redd", "alice1"));
    BOOST_CHECK_EQUAL(pMgr->ResolveReddID("alice1", "redd"), "alice");
    BOOST_CHECK_EQUAL(pMgr->ResolveReddID("al", "redd"), "alice");
}

// SUSPECTED BUG (documented, not fixed): a resolution is not checked against
// anything. The namespace and the user ID need not exist, the profile need
// not own the user ID, and AddNamespaceResolution replaces a resolution that
// exists. When two profiles resolve to one user ID, the user ID resolves to
// the profile that was written last.
BOOST_AUTO_TEST_CASE(namespace_resolution_is_not_checked)
{
    AddProfile("alice");
    AddProfile("mallory");

    BOOST_CHECK(pMgr->AddNamespaceResolution("alice", "nosuchns", "al"));
    BOOST_CHECK(pMgr->AddNamespaceResolution("alice", "nosuchns", "alice1"));
    BOOST_CHECK_EQUAL(pMgr->ResolveNamespace("alice", "nosuchns"), "alice1");

    BOOST_CHECK(pMgr->AddNamespaceResolution("mallory", "nosuchns", "alice1"));
    BOOST_CHECK_EQUAL(pMgr->ResolveNamespace("alice", "nosuchns"), "alice1");
    BOOST_CHECK_EQUAL(pMgr->ResolveNamespace("mallory", "nosuchns"), "alice1");
    BOOST_CHECK_EQUAL(pMgr->ResolveReddID("alice1", "nosuchns"), "mallory");
}

// ===================================================================
// Category 5: Names and validation
// ===================================================================

BOOST_AUTO_TEST_CASE(is_valid_profile_name)
{
    BOOST_CHECK(!pMgr->IsValidProfileName(""));
    BOOST_CHECK(!pMgr->IsValidProfileName("ab"));
    BOOST_CHECK(pMgr->IsValidProfileName("abc"));
    BOOST_CHECK(pMgr->IsValidProfileName(std::string(MAX_REDDID_LENGTH, 'a')));
    BOOST_CHECK(!pMgr->IsValidProfileName(std::string(MAX_REDDID_LENGTH + 1, 'a')));

    BOOST_CHECK(pMgr->IsValidProfileName("abc123"));
    BOOST_CHECK(pMgr->IsValidProfileName("123"));
    BOOST_CHECK(pMgr->IsValidProfileName("a_b"));
    BOOST_CHECK(pMgr->IsValidProfileName("a__b"));
    BOOST_CHECK(!pMgr->IsValidProfileName("_ab"));
    BOOST_CHECK(!pMgr->IsValidProfileName("ab_"));
    BOOST_CHECK(!pMgr->IsValidProfileName("Alice"));
    BOOST_CHECK(!pMgr->IsValidProfileName("al-ice"));
    BOOST_CHECK(!pMgr->IsValidProfileName("al.ice"));
    BOOST_CHECK(!pMgr->IsValidProfileName("al ice"));
    BOOST_CHECK(!pMgr->IsValidProfileName(std::string("al\0ce", 5)));
    BOOST_CHECK(!pMgr->IsValidProfileName("alic\xc3\xa9"));

    // The same rule as ReddIDManager::ValidateReddID
    for (const std::string name : {"", "ab", "abc", "a_b", "_ab", "ab_", "Alice", "al-ice"}) {
        BOOST_CHECK_EQUAL(pMgr->IsValidProfileName(name), reddidManager->ValidateReddID(name));
    }
}

BOOST_AUTO_TEST_CASE(create_profile_validates_the_profile)
{
    CKeyID owner = MakeKeyID();

    BOOST_CHECK(!pMgr->CreateProfile(TestProfile("ab", owner)));
    BOOST_CHECK(!pMgr->CreateProfile(TestProfile("Alice", owner)));
    BOOST_CHECK(!pMgr->ProfileExists("ab"));

    ReddIDProfile profile = TestProfile("alice", owner);
    profile.displayName = std::string(65, 'x');
    BOOST_CHECK(!pMgr->CreateProfile(profile));
    profile.displayName = std::string(64, 'x');
    profile.bio = std::string(257, 'x');
    BOOST_CHECK(!pMgr->CreateProfile(profile));
    profile.bio = std::string(256, 'x');
    profile.verificationStatus = static_cast<VerificationStatus>(4);
    BOOST_CHECK(!pMgr->CreateProfile(profile));
    BOOST_CHECK(!pMgr->ProfileExists("alice"));
    ReddIDReputation reputation;
    BOOST_CHECK(!pMgr->GetReputation("alice", reputation));

    profile.verificationStatus = VERIFICATION_NONE;
    BOOST_CHECK(pMgr->CreateProfile(profile));
    BOOST_CHECK_EQUAL(GetStoredProfile("alice").bio.size(), 256U);

    // An update is held to the same rules
    profile.bio = std::string(257, 'x');
    BOOST_CHECK(!pMgr->UpdateProfile(profile));
    BOOST_CHECK_EQUAL(GetProfile("alice").bio.size(), 256U);
}

// UpdateProfile of a profile that does not exist creates it
BOOST_AUTO_TEST_CASE(update_profile_creates_a_missing_profile)
{
    CKeyID owner = MakeKeyID();
    BOOST_CHECK(pMgr->UpdateProfile(TestProfile("alice", owner)));
    BOOST_CHECK(pMgr->IsProfileOwner("alice", owner));
    BOOST_CHECK(db->ExistsProfile("alice"));
    ReddIDReputation reputation;
    BOOST_REQUIRE(pMgr->GetReputation("alice", reputation));
    BOOST_CHECK_EQUAL(reputation.overallScore, 50.0);
}

// SUSPECTED BUG (documented, not fixed): the owner of a profile sets its
// verification status. Any value from VERIFICATION_NONE to
// VERIFICATION_OFFICIAL is accepted when the profile is created or updated.
BOOST_AUTO_TEST_CASE(owner_chooses_the_verification_status)
{
    CKeyID owner = MakeKeyID();
    ReddIDProfile profile = TestProfile("alice", owner);
    profile.verificationStatus = VERIFICATION_COMMUNITY;
    BOOST_CHECK(pMgr->CreateProfile(profile));
    BOOST_CHECK_EQUAL(GetProfile("alice").verificationStatus, VERIFICATION_COMMUNITY);

    profile.verificationStatus = VERIFICATION_OFFICIAL;
    BOOST_CHECK(pMgr->UpdateProfile(profile));
    BOOST_CHECK_EQUAL(GetProfile("alice").verificationStatus, VERIFICATION_OFFICIAL);
    BOOST_CHECK_EQUAL(GetStoredProfile("alice").verificationStatus, VERIFICATION_OFFICIAL);
    BOOST_CHECK_EQUAL(Calculate("alice").verificationScore, 100.0);
}

// ===================================================================
// Category 6: Lookups
// ===================================================================

BOOST_AUTO_TEST_CASE(get_profiles_by_owner)
{
    CKeyID alice = MakeKeyID();
    CKeyID bob = MakeKeyID();
    AddProfile("zeta", alice);
    AddProfile("alpha", alice);
    AddProfile("bob", bob);

    std::vector<std::string> reddIds;
    BOOST_CHECK(pMgr->GetProfilesByOwner(alice, reddIds));
    BOOST_REQUIRE_EQUAL(reddIds.size(), 2U);
    BOOST_CHECK_EQUAL(reddIds[0], "alpha");
    BOOST_CHECK_EQUAL(reddIds[1], "zeta");

    // Current behaviour: the result is appended to what the caller passes in
    BOOST_CHECK(pMgr->GetProfilesByOwner(bob, reddIds));
    BOOST_REQUIRE_EQUAL(reddIds.size(), 3U);
    BOOST_CHECK_EQUAL(reddIds[2], "bob");

    reddIds.clear();
    BOOST_CHECK(pMgr->GetProfilesByOwner(MakeKeyID(), reddIds));
    BOOST_CHECK(reddIds.empty());

    // A transfer moves the profile from one owner to the other
    BOOST_REQUIRE(pMgr->TransferProfile("zeta", alice, bob));
    BOOST_CHECK(pMgr->GetProfilesByOwner(bob, reddIds));
    BOOST_REQUIRE_EQUAL(reddIds.size(), 2U);
    BOOST_CHECK_EQUAL(reddIds[0], "bob");
    BOOST_CHECK_EQUAL(reddIds[1], "zeta");
    reddIds.clear();
    BOOST_CHECK(pMgr->GetProfilesByOwner(alice, reddIds));
    BOOST_REQUIRE_EQUAL(reddIds.size(), 1U);
    BOOST_CHECK_EQUAL(reddIds[0], "alpha");

    // A deactivated profile is still listed
    BOOST_REQUIRE(pMgr->DeactivateProfile("alpha", alice));
    reddIds.clear();
    BOOST_CHECK(pMgr->GetProfilesByOwner(alice, reddIds));
    BOOST_CHECK_EQUAL(reddIds.size(), 1U);
}

BOOST_AUTO_TEST_CASE(lookups_of_unknown_profiles)
{
    ReddIDProfile profile;
    ReddIDReputation reputation;
    ReddIDConnection connection;
    BOOST_CHECK(!pMgr->ProfileExists("nobody"));
    BOOST_CHECK(!pMgr->IsProfileActive("nobody"));
    BOOST_CHECK(!pMgr->IsProfileOwner("nobody", MakeKeyID()));
    BOOST_CHECK(!pMgr->GetProfile("nobody", profile));
    BOOST_CHECK(!pMgr->GetReputation("nobody", reputation));
    BOOST_CHECK(!pMgr->GetConnection("nobody", "nobody2", connection));
    BOOST_CHECK(pMgr->GetConnections("nobody").empty());
    BOOST_CHECK_EQUAL(pMgr->ResolveNamespace("nobody", "redd"), "");
    BOOST_CHECK_EQUAL(pMgr->ResolveReddID("nobody", "redd"), "");
    BOOST_CHECK(!pMgr->DeactivateProfile("nobody", MakeKeyID()));
    BOOST_CHECK(!pMgr->ReactivateProfile("nobody", MakeKeyID()));
    BOOST_CHECK(!pMgr->TransferProfile("nobody", MakeKeyID(), MakeKeyID()));
    BOOST_CHECK(!pMgr->UpdateReputation(ReddIDReputation()));
    BOOST_CHECK(pMgr->GetDB() == db);
}

BOOST_AUTO_TEST_CASE(deactivate_reactivate_and_transfer_set_last_updated)
{
    CKeyID owner = MakeKeyID();
    CKeyID buyer = MakeKeyID();
    AddProfile("alice", owner);

    SetMockTime(T0 + DAY);
    BOOST_REQUIRE(pMgr->DeactivateProfile("alice", owner));
    BOOST_CHECK_EQUAL(GetStoredProfile("alice").lastUpdated, T0 + DAY);
    BOOST_CHECK(!GetStoredProfile("alice").active);

    SetMockTime(T0 + 2 * DAY);
    BOOST_CHECK(!pMgr->ReactivateProfile("alice", buyer));
    BOOST_REQUIRE(pMgr->ReactivateProfile("alice", owner));
    BOOST_CHECK_EQUAL(GetStoredProfile("alice").lastUpdated, T0 + 2 * DAY);
    BOOST_CHECK(GetStoredProfile("alice").active);

    SetMockTime(T0 + 3 * DAY);
    BOOST_REQUIRE(pMgr->TransferProfile("alice", owner, buyer));
    BOOST_CHECK_EQUAL(GetStoredProfile("alice").lastUpdated, T0 + 3 * DAY);
    BOOST_CHECK(GetStoredProfile("alice").owner == buyer);
    // None of them counts as activity
    BOOST_CHECK_EQUAL(GetProfile("alice").lastActivityTime, T0);
}

// ===================================================================
// Category 7: Expiry
// ===================================================================

BOOST_AUTO_TEST_CASE(check_expired_profiles_deactivates_after_the_expiration_time)
{
    CKeyID owner = MakeKeyID();
    ReddIDProfile expiring = TestProfile("expiring", owner);
    expiring.expirationTime = T0 + 10 * DAY;
    AddProfile(expiring);
    ReddIDProfile later = TestProfile("later", owner);
    later.expirationTime = T0 + 10 * DAY + 2;
    AddProfile(later);
    ReddIDProfile never = TestProfile("never", owner);
    never.expirationTime = 0;
    AddProfile(never);

    pMgr->CheckExpiredProfiles();
    BOOST_CHECK(pMgr->IsProfileActive("expiring"));

    // At the expiration time itself the profile is still active
    SetMockTime(T0 + 10 * DAY);
    pMgr->CheckExpiredProfiles();
    BOOST_CHECK(pMgr->IsProfileActive("expiring"));

    SetMockTime(T0 + 10 * DAY + 1);
    pMgr->CheckExpiredProfiles();
    BOOST_CHECK(!pMgr->IsProfileActive("expiring"));
    BOOST_CHECK(!GetStoredProfile("expiring").active);
    BOOST_CHECK(pMgr->IsProfileActive("later"));
    BOOST_CHECK(pMgr->IsProfileActive("never"));

    // The profile stays, with its owner, and nothing else about it changes
    BOOST_CHECK(pMgr->ProfileExists("expiring"));
    BOOST_CHECK(pMgr->IsProfileOwner("expiring", owner));
    BOOST_CHECK_EQUAL(GetProfile("expiring").lastUpdated, T0);
    BOOST_CHECK_EQUAL(GetProfile("expiring").expirationTime, T0 + 10 * DAY);

    SetMockTime(T0 + 10000 * DAY);
    pMgr->CheckExpiredProfiles();
    BOOST_CHECK(!pMgr->IsProfileActive("later"));
    BOOST_CHECK(pMgr->IsProfileActive("never"));
    BOOST_CHECK(GetStoredProfile("never").active);
}

// Current behaviour: the owner can reactivate an expired profile without
// renewing it. It stays active until the next check.
BOOST_AUTO_TEST_CASE(expired_profile_can_be_reactivated_until_the_next_check)
{
    CKeyID owner = MakeKeyID();
    ReddIDProfile profile = TestProfile("alice", owner);
    profile.expirationTime = T0 + DAY;
    AddProfile(profile);

    SetMockTime(T0 + 2 * DAY);
    pMgr->CheckExpiredProfiles();
    BOOST_REQUIRE(!pMgr->IsProfileActive("alice"));

    BOOST_CHECK(pMgr->ReactivateProfile("alice", owner));
    BOOST_CHECK(pMgr->IsProfileActive("alice"));
    BOOST_CHECK_EQUAL(GetProfile("alice").expirationTime, T0 + DAY);

    pMgr->CheckExpiredProfiles();
    BOOST_CHECK(!pMgr->IsProfileActive("alice"));
}

// Current behaviour: an expired profile is only marked inactive. It still
// connects, resolves, is updated and has its reputation calculated.
BOOST_AUTO_TEST_CASE(expired_profile_keeps_working)
{
    CKeyID owner = MakeKeyID();
    ReddIDProfile profile = TestProfile("alice", owner);
    profile.expirationTime = T0 + DAY;
    AddProfile(profile);
    AddProfile("bob");

    SetMockTime(T0 + 2 * DAY);
    pMgr->CheckExpiredProfiles();
    BOOST_REQUIRE(!pMgr->IsProfileActive("alice"));

    BOOST_CHECK(pMgr->CreateConnection(TestConnection("alice", "bob", CONNECTION_FOLLOW)));
    BOOST_CHECK(pMgr->CreateConnection(TestConnection("bob", "alice", CONNECTION_FOLLOW)));
    BOOST_CHECK(pMgr->AddNamespaceResolution("alice", "redd", "al"));
    ReddIDReputation reputation;
    BOOST_CHECK(pMgr->CalculateReputation("alice", BidderStats(), reputation));
    BOOST_CHECK_EQUAL(reputation.engagementScore, 2.5);

    // An update with the active flag set brings the profile back
    profile.bio = "back";
    BOOST_CHECK(pMgr->UpdateProfile(profile));
    BOOST_CHECK(pMgr->IsProfileActive("alice"));
}

// ===================================================================
// Category 8: Profile hash
// ===================================================================

BOOST_AUTO_TEST_CASE(profile_hash_is_the_hash_of_the_serialized_profile)
{
    ReddIDProfile profile = TestProfile("alice", MakeKeyID());
    profile.avatarHash = uint256S("a1");
    profile.emailHash = uint256S("e1");
    profile.socialData = "{}";
    profile.messagingPubkey = {2, 3, 4};

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << profile;
    uint256 expected = Hash(ss);
    BOOST_CHECK(pMgr->CalculateProfileHash(profile) == expected);

    // Every field is covered
    std::map<std::string, ReddIDProfile> changed;
    for (const std::string field : {"reddId", "owner", "displayName", "avatarHash", "bio", "emailHash", "socialData",
                                    "messagingPubkey", "verificationStatus", "creationTime", "lastUpdated",
                                    "expirationTime", "lastActivityTime", "active", "flags"}) {
        changed[field] = profile;
    }
    changed["reddId"].reddId = "alicf";
    changed["owner"].owner = MakeKeyID();
    changed["displayName"].displayName = "Alice";
    changed["avatarHash"].avatarHash = uint256S("a2");
    changed["bio"].bio = "Another bio";
    changed["emailHash"].emailHash = uint256S("e2");
    changed["socialData"].socialData = "[]";
    changed["messagingPubkey"].messagingPubkey = {2, 3, 5};
    changed["verificationStatus"].verificationStatus = VERIFICATION_SELF;
    changed["creationTime"].creationTime += 1;
    changed["lastUpdated"].lastUpdated += 1;
    changed["expirationTime"].expirationTime += 1;
    changed["lastActivityTime"].lastActivityTime += 1;
    changed["active"].active = false;
    changed["flags"].flags = 1;
    for (const auto& [field, other] : changed) {
        BOOST_CHECK_MESSAGE(pMgr->CalculateProfileHash(other) != expected, field);
    }
}

// SUSPECTED BUG (documented, not fixed): the profile hash covers
// lastActivityTime (and lastUpdated, active and the verification status, which
// the node changes by itself). A profile update record carries the hash of
// the profile, and processing that very record bumps lastActivityTime, so
// the stored profile no longer has the hash that the record announced. The
// same happens on every connection record, and whenever the profile expires
// or is promoted.
BOOST_AUTO_TEST_CASE(profile_hash_changes_on_every_activity_bump)
{
    ReddIDProfile created = AddProfile("alice");
    AddProfile("bob");
    uint256 announced = pMgr->CalculateProfileHash(created);
    BOOST_CHECK(pMgr->CalculateProfileHash(GetProfile("alice")) == announced);

    SetMockTime(T0 + 1);
    BOOST_REQUIRE(pMgr->ProcessTransaction(ProfileUpdateTx("alice", announced), 7));
    uint256 afterUpdate = pMgr->CalculateProfileHash(GetProfile("alice"));
    BOOST_CHECK(afterUpdate != announced);

    SetMockTime(T0 + 2);
    BOOST_REQUIRE(pMgr->ProcessTransaction(ConnectionTx("alice", "bob"), 8));
    uint256 afterConnection = pMgr->CalculateProfileHash(GetProfile("alice"));
    BOOST_CHECK(afterConnection != afterUpdate);
    BOOST_CHECK(afterConnection != announced);

    // With the activity time put back, the hash is the announced one again
    ReddIDProfile profile = GetProfile("alice");
    profile.lastActivityTime = created.lastActivityTime;
    BOOST_CHECK(pMgr->CalculateProfileHash(profile) == announced);
}

BOOST_AUTO_TEST_SUITE_END()
