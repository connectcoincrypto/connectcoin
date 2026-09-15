### TestGen ###

Utilities to generate candidate vectors for the data-driven ConnectCoin key/address parsing tests, including inherited address formats. These vectors do not describe the set of output types accepted by the current wallet or consensus.

From this directory, write candidates to new files (choose unused filenames):

    ./gen_key_io_test_vectors.py valid 70 > key_io_valid.generated.json
    ./gen_key_io_test_vectors.py invalid 70 > key_io_invalid.generated.json

Do not replace the checked-in fixtures directly with this output. The generator's
`valid` mode uses random bytes for version-1 witness programs without checking
whether they are valid x-only public keys; current ConnectCoin decoding rejects
invalid curve points. Review and validate every candidate against the current
key/address parsing code before replacing fixtures, and preserve the existing
hand-maintained regression cases. Fixture changes must also pass the
`key_io_tests` unit tests after rebuilding `connectcoin-test`.
