add_executable(connectcoin-test-kernel
    test_kernel.cpp
)

target_link_libraries(connectcoin-test-kernel
  PRIVATE
    core_interface
    connectcoinkernel
    Boost::headers
)

add_windows_application_manifest(connectcoin-test-kernel)

add_executable(connectcoin-test-kernel-typed-output
    typed_output_tests.cpp
)

target_link_libraries(connectcoin-test-kernel-typed-output
  PRIVATE
    core_interface
    connectcoinkernel
    connectcoin_common
    Boost::headers
)

add_windows_application_manifest(connectcoin-test-kernel-typed-output)

add_test(NAME kernel_typed_output_tests
  COMMAND connectcoin-test-kernel-typed-output --log_level=test_suite
)

# The inherited executable embeds Bitcoin P2PKH/P2WSH transactions and a
# historical regtest block corpus in the legacy CTxOut wire format. Keep
# building it so the experimental Kernel API stays compile-checked, but do not
# register those foreign-chain vectors as ConnectCoin consensus tests. Re-add
# this target after a finalized typed-output block corpus is generated.
