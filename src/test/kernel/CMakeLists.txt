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

# The full API executable also exercises historical context-free mainnet
# vectors and a 206-block typed-output regtest corpus mined with real RandomX.
# Keep its existing standalone execution policy: the serialized corpus must
# not be checked using the mock-PoW profile selected by some CI jobs. The
# smaller kernel_typed_output_tests above remains registered for every profile.
