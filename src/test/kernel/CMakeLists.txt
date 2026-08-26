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

add_test(NAME connectcoin-test-kernel COMMAND connectcoin-test-kernel)
