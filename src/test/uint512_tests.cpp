// Copyright (c) 2022 The Reddcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <streams.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <version.h>

#include <boost/test/unit_test.hpp>

#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

BOOST_AUTO_TEST_SUITE(uint512_tests)

// Test data arrays for uint512 (64 bytes each)
const unsigned char R1Array[] =
    "\x9c\x52\x4a\xdb\xcf\x56\x11\x12\x2b\x29\x12\x5e\x5d\x35\xd2\xd2"
    "\x22\x81\xaa\xb5\x33\xf0\x08\x32\xd5\x56\xb1\xf9\xea\xe5\x1d\x7d"
    "\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff\x00"
    "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10";
const char R1ArrayHex[] = "100f0e0d0c0b0a090807060504030201" "00ffeeddccbbaa998877665544332211" "7d1de5eaf9b156d53208f033b5aa8122" "d2d2355d5e12292b121156cfdb4a529c";
const uint512 R1 = uint512(std::vector<unsigned char>(R1Array, R1Array+64));

const unsigned char R2Array[] =
    "\x70\x32\x1d\x7c\x47\xa5\x6b\x40\x26\x7e\x0a\xc3\xa6\x9c\xb6\xbf"
    "\x13\x30\x47\xa3\x19\x2d\xda\x71\x49\x13\x72\xf0\xb4\xca\x81\xd7"
    "\xaa\xbb\xcc\xdd\xee\xff\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99"
    "\x99\x88\x77\x66\x55\x44\x33\x22\x11\x00\xff\xee\xdd\xcc\xbb\xaa";
const uint512 R2 = uint512(std::vector<unsigned char>(R2Array, R2Array+64));

const unsigned char ZeroArray[] =
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00";
const uint512 Zero = uint512(std::vector<unsigned char>(ZeroArray, ZeroArray+64));

const unsigned char OneArray[] =
    "\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00";
const uint512 One = uint512(std::vector<unsigned char>(OneArray, OneArray+64));

const unsigned char MaxArray[] =
    "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
    "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
    "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff"
    "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff";
const uint512 Max = uint512(std::vector<unsigned char>(MaxArray, MaxArray+64));

static std::string ArrayToString(const unsigned char A[], unsigned int width)
{
    std::stringstream Stream;
    Stream << std::hex;
    for (unsigned int i = 0; i < width; ++i)
    {
        Stream << std::setw(2) << std::setfill('0') << (unsigned int)A[width-i-1];
    }
    return Stream.str();
}

BOOST_AUTO_TEST_CASE( basics ) // constructors, equality, inequality
{
    // constructor uint512(vector<char>):
    BOOST_CHECK(R1.ToString() == ArrayToString(R1Array, 64));
    BOOST_CHECK(R2.ToString() == ArrayToString(R2Array, 64));
    BOOST_CHECK(Zero.ToString() == ArrayToString(ZeroArray, 64));
    BOOST_CHECK(One.ToString() == ArrayToString(OneArray, 64));
    BOOST_CHECK(Max.ToString() == ArrayToString(MaxArray, 64));
    BOOST_CHECK(One.ToString() != ArrayToString(ZeroArray, 64));

    // == and !=
    BOOST_CHECK(R1 != R2);
    BOOST_CHECK(Zero != One);
    BOOST_CHECK(One != Zero);
    BOOST_CHECK(Max != Zero);

    // String Constructor and Copy Constructor
    BOOST_CHECK(uint512S("0x"+R1.ToString()) == R1);
    BOOST_CHECK(uint512S("0x"+R2.ToString()) == R2);
    BOOST_CHECK(uint512S("0x"+Zero.ToString()) == Zero);
    BOOST_CHECK(uint512S("0x"+One.ToString()) == One);
    BOOST_CHECK(uint512S("0x"+Max.ToString()) == Max);
    BOOST_CHECK(uint512S(R1.ToString()) == R1);
    BOOST_CHECK(uint512S("   0x"+R1.ToString()+"   ") == R1);
    BOOST_CHECK(uint512S("") == Zero);
    BOOST_CHECK(R1 == uint512S(R1ArrayHex));
    BOOST_CHECK(uint512(R1) == R1);
    BOOST_CHECK(uint512(Zero) == Zero);
    BOOST_CHECK(uint512(One) == One);
}

BOOST_AUTO_TEST_CASE( comparison ) // <= >= < >
{
    // Test bit-by-bit comparison
    uint512 Last;
    for (int i = 511; i >= 0; --i) {
        uint512 Tmp;
        *(Tmp.begin() + (i>>3)) |= 1<<(7-(i&7));
        BOOST_CHECK(Last < Tmp);
        Last = Tmp;
    }

    // Test basic comparisons
    BOOST_CHECK(Zero < R1);
    BOOST_CHECK(R2 < R1);
    BOOST_CHECK(Zero < One);
    BOOST_CHECK(One < Max);
    BOOST_CHECK(R1 < Max);
    BOOST_CHECK(R2 < Max);

    // Test greater than
    BOOST_CHECK(!(Zero < Zero));
    BOOST_CHECK(!(R1 < R1));
    BOOST_CHECK(!(Max < Max));
    BOOST_CHECK(!(R1 < R2));
    BOOST_CHECK(!(One < Zero));
}

BOOST_AUTO_TEST_CASE( methods ) // GetHex SetHex begin() end() size() GetSerializeSize, Serialize, Unserialize
{
    // Test GetHex and ToString
    BOOST_CHECK(R1.GetHex() == R1.ToString());
    BOOST_CHECK(R2.GetHex() == R2.ToString());
    BOOST_CHECK(One.GetHex() == One.ToString());
    BOOST_CHECK(Max.GetHex() == Max.ToString());

    // Test SetHex
    uint512 Tmp(R1);
    BOOST_CHECK(Tmp == R1);
    Tmp.SetHex(R2.ToString());   BOOST_CHECK(Tmp == R2);
    Tmp.SetHex(Zero.ToString()); BOOST_CHECK(Tmp == uint512());

    // Test begin/end and memcmp
    Tmp.SetHex(R1.ToString());
    BOOST_CHECK(memcmp(R1.begin(), R1Array, 64) == 0);
    BOOST_CHECK(memcmp(Tmp.begin(), R1Array, 64) == 0);
    BOOST_CHECK(memcmp(R2.begin(), R2Array, 64) == 0);
    BOOST_CHECK(memcmp(Zero.begin(), ZeroArray, 64) == 0);
    BOOST_CHECK(memcmp(One.begin(), OneArray, 64) == 0);

    // Test size
    BOOST_CHECK(R1.size() == sizeof(R1));
    BOOST_CHECK(sizeof(R1) == 64);
    BOOST_CHECK(R1.size() == 64);
    BOOST_CHECK(R2.size() == 64);
    BOOST_CHECK(Zero.size() == 64);
    BOOST_CHECK(Max.size() == 64);

    // Test begin/end pointers
    BOOST_CHECK(R1.begin() + 64 == R1.end());
    BOOST_CHECK(R2.begin() + 64 == R2.end());
    BOOST_CHECK(One.begin() + 64 == One.end());
    BOOST_CHECK(Max.begin() + 64 == Max.end());
    BOOST_CHECK(Tmp.begin() + 64 == Tmp.end());

    // Test GetSerializeSize
    BOOST_CHECK(GetSerializeSize(R1, PROTOCOL_VERSION) == 64);
    BOOST_CHECK(GetSerializeSize(Zero, PROTOCOL_VERSION) == 64);

    // Test Serialize/Unserialize
    CDataStream ss(0, PROTOCOL_VERSION);
    ss << R1;
    BOOST_CHECK(ss.str() == std::string(R1Array, R1Array+64));
    ss >> Tmp;
    BOOST_CHECK(R1 == Tmp);
    ss.clear();

    ss << Zero;
    BOOST_CHECK(ss.str() == std::string(ZeroArray, ZeroArray+64));
    ss >> Tmp;
    BOOST_CHECK(Zero == Tmp);
    ss.clear();

    ss << Max;
    BOOST_CHECK(ss.str() == std::string(MaxArray, MaxArray+64));
    ss >> Tmp;
    BOOST_CHECK(Max == Tmp);
    ss.clear();

    // Test IsNull
    BOOST_CHECK(Zero.IsNull());
    BOOST_CHECK(!R1.IsNull());
    BOOST_CHECK(!One.IsNull());
    BOOST_CHECK(!Max.IsNull());

    // Test SetNull
    uint512 TestNull = R1;
    BOOST_CHECK(!TestNull.IsNull());
    TestNull.SetNull();
    BOOST_CHECK(TestNull.IsNull());
}

BOOST_AUTO_TEST_CASE( uint512_specialized_methods )
{
    // Test GetCheapHash - should return first 8 bytes as uint64
    uint512 hash_test = uint512S("123456789abcdef0fedcba9876543210123456789abcdef0fedcba9876543210123456789abcdef0fedcba9876543210123456789abcdef0fedcba9876543210");
    uint64_t cheap_hash = hash_test.GetCheapHash();
    BOOST_CHECK(cheap_hash != 0);

    // Verify it actually returns the first 8 bytes (little-endian)
    const unsigned char expected_bytes[] = "\x10\x32\x54\x76\x98\xba\xdc\xfe";
    uint64_t expected = 0;
    memcpy(&expected, expected_bytes, 8);
    BOOST_CHECK_EQUAL(cheap_hash, expected);

    // Test trim256 - should return the first 32 bytes (lower-order bytes in little-endian)
    uint512 full = uint512S("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0");
    uint256 trimmed = full.trim256();
    // The hex string is displayed in reverse byte order, so first 32 bytes (0-31) are the rightmost 64 hex chars
    BOOST_CHECK_EQUAL(trimmed.GetHex(), "123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0");

    // Test trim256 with all zeros
    uint256 trimmed_zero = Zero.trim256();
    BOOST_CHECK(trimmed_zero.IsNull());

    // Test trim256 with all ones
    uint256 trimmed_max = Max.trim256();
    BOOST_CHECK_EQUAL(trimmed_max.GetHex(), "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
}

BOOST_AUTO_TEST_SUITE_END()
