// Genesis block miner for Reddcoin regtest
// Compile: g++ -o mine_regtest_genesis mine_regtest_genesis.cpp src/crypto/scrypt.cpp -Isrc -std=c++17 -O3
// Run: ./mine_regtest_genesis

#include <iostream>
#include <iomanip>
#include <cstring>
#include <stdint.h>
#include <chrono>
#include <crypto/scrypt.h>

// Simplified block header structure for mining
struct CBlockHeader {
    int32_t nVersion;
    uint8_t hashPrevBlock[32];
    uint8_t hashMerkleRoot[32];
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;
};

void uint256_to_hex(const uint8_t* hash, char* output) {
    for (int i = 31; i >= 0; i--) {
        sprintf(output + (31 - i) * 2, "%02x", hash[i]);
    }
    output[64] = '\0';
}

void hex_to_uint256(const char* hex, uint8_t* output) {
    for (int i = 0; i < 32; i++) {
        sscanf(hex + i * 2, "%2hhx", &output[31 - i]);
    }
}

uint32_t decode_compact(uint32_t nBits) {
    // This is simplified - just for comparison
    return nBits;
}

bool check_pow_hash(const uint8_t* hash, uint32_t nBits) {
    // Check if hash meets the target
    // For 0x1d00ffff, we need hash to start with 0x0000

    // Simplified check: hash should have leading zeros
    if (hash[31] != 0 || hash[30] != 0) return false;

    // For nBits 0x1d00ffff, target is 0x00000000ffff0000000000000000000000000000000000000000000000000000
    // So we need at least 4 zero bytes at the end (beginning in little endian)

    // More accurate: decode nBits and compare
    uint8_t target[32] = {0};
    uint32_t size = nBits >> 24;
    uint32_t word = nBits & 0x007fffff;

    if (size <= 3) {
        word >>= 8 * (3 - size);
        target[0] = word & 0xff;
        target[1] = (word >> 8) & 0xff;
        target[2] = (word >> 16) & 0xff;
    } else {
        int offset = size - 3;
        target[offset] = word & 0xff;
        target[offset + 1] = (word >> 8) & 0xff;
        target[offset + 2] = (word >> 16) & 0xff;
    }

    // Compare hash <= target (both in little endian)
    for (int i = 31; i >= 0; i--) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return true;
}

int main() {
    std::cout << "=== Reddcoin Regtest Genesis Block Miner ===" << std::endl;
    std::cout << "Using Scrypt-1024-1-1-256 (N=1024, r=1, p=1)" << std::endl << std::endl;

    // Regtest genesis parameters
    CBlockHeader header;
    header.nVersion = 1;

    // hashPrevBlock = 0 (genesis)
    memset(header.hashPrevBlock, 0, 32);

    // hashMerkleRoot from CreateGenesisBlock
    const char* merkleroot_hex = "b502bc1dc42b07092b9187e92f70e32f9a53247feae16d821bebffa916af79ff";
    hex_to_uint256(merkleroot_hex, header.hashMerkleRoot);

    header.nTime = 1642570147;  // From current regtest
    header.nBits = 0x207fffff;   // Bitcoin regtest difficulty (easy for instant mining)
    header.nNonce = 0;

    std::cout << "Genesis Parameters:" << std::endl;
    std::cout << "  nVersion: " << header.nVersion << std::endl;
    std::cout << "  nTime: " << header.nTime << std::endl;
    std::cout << "  nBits: 0x" << std::hex << header.nBits << std::dec << std::endl;

    char merkle_str[65];
    uint256_to_hex(header.hashMerkleRoot, merkle_str);
    std::cout << "  hashMerkleRoot: " << merkle_str << std::endl;
    std::cout << std::endl;

    // Calculate target
    uint8_t target[32] = {0};
    uint32_t size = header.nBits >> 24;
    uint32_t word = header.nBits & 0x007fffff;

    if (size <= 3) {
        word >>= 8 * (3 - size);
        target[0] = word & 0xff;
        target[1] = (word >> 8) & 0xff;
        target[2] = (word >> 16) & 0xff;
    } else {
        int offset = size - 3;
        target[offset] = word & 0xff;
        target[offset + 1] = (word >> 8) & 0xff;
        target[offset + 2] = (word >> 16) & 0xff;
    }

    char target_str[65];
    uint256_to_hex(target, target_str);
    std::cout << "Target: 0x" << target_str << std::endl;
    std::cout << std::endl;

    std::cout << "Mining..." << std::endl;

    uint64_t hashes = 0;
    uint8_t hash[32];
    auto start_time = std::chrono::high_resolution_clock::now();

    while (true) {
        // Calculate scrypt hash of header
        scrypt_1024_1_1_256((const char*)&header, (char*)hash);

        hashes++;

        // Check if we found a valid block
        if (check_pow_hash(hash, header.nBits)) {
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

            char hash_str[65];
            uint256_to_hex(hash, hash_str);

            std::cout << std::endl;
            std::cout << "=== GENESIS BLOCK FOUND! ===" << std::endl;
            std::cout << std::endl;
            std::cout << "Nonce: " << header.nNonce << std::endl;
            std::cout << "Hash: 0x" << hash_str << std::endl;
            std::cout << std::endl;
            std::cout << "Mining Statistics:" << std::endl;
            std::cout << "  Total hashes: " << hashes << std::endl;
            std::cout << "  Time: " << duration.count() << " ms" << std::endl;
            if (duration.count() > 0) {
                std::cout << "  Hashrate: " << (hashes * 1000 / duration.count()) << " H/s" << std::endl;
            }
            std::cout << std::endl;

            std::cout << "=== UPDATE src/chainparams.cpp (Regtest) ===" << std::endl;
            std::cout << std::endl;
            std::cout << "Replace line ~656 with:" << std::endl;
            std::cout << "    genesis = CreateGenesisBlock("
                      << header.nTime << ", "
                      << header.nTime << ", "
                      << header.nNonce << ", 0x"
                      << std::hex << header.nBits << std::dec
                      << ", 1, 10000 * COIN);" << std::endl;
            std::cout << std::endl;
            std::cout << "Replace line ~659 with:" << std::endl;
            std::cout << "    assert(consensus.hashGenesisBlock == uint256S(\"" << hash_str << "\"));" << std::endl;
            std::cout << std::endl;

            break;
        }

        // Progress update every 10000 hashes
        if (hashes % 10000 == 0) {
            auto current_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - start_time);
            if (duration.count() > 0) {
                std::cout << "\rHashes: " << hashes
                          << " (" << (hashes * 1000 / duration.count()) << " H/s)" << std::flush;
            }
        }

        header.nNonce++;

        // Safety check - stop if nonce overflows
        if (header.nNonce == 0) {
            std::cout << std::endl;
            std::cout << "ERROR: Nonce overflow! Try different timestamp." << std::endl;
            return 1;
        }
    }

    return 0;
}
