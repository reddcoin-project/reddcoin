### TestGen ###

Utilities to generate test vectors for the data-driven Reddcoin tests.

Usage:

    # Generate address/key test vectors
    ./gen_key_io_test_vectors.py valid 70 > ../../src/test/data/key_io_valid.json
    ./gen_key_io_test_vectors.py invalid 70 > ../../src/test/data/key_io_invalid.json

    # Generate block filter test vectors (requires running reddcoind)
    ./gen_blockfilter_test_vectors.py 100 1000 5000 > ../../src/test/data/blockfilters.json

    # Or use default interesting blocks
    ./gen_blockfilter_test_vectors.py > ../../src/test/data/blockfilters.json
