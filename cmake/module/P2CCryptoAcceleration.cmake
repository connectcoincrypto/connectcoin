# Copyright (c) 2026 The ConnectCoin developers. MIT license.
# Ported from connectcoin-claimer bc0a182. No protocol or trust-policy changes.
option(P2C_X25519_ACCELERATION "Use bundled HACL for validated PSA X25519 multiplication" ON)
option(P2C_SHA256_ACCELERATION "Accelerate Mbed TLS SHA-224/256 compression on supported CPUs" ON)
option(P2C_CRYPTO_ACCELERATION_TESTING "Enable provider dispatch counters for differential tests" OFF)
option(P2C_SHA256_X86_BACKEND "Build runtime-CPU-gated x86-64 SHA-NI backend" ON)

set(p2c_sha256_x86_target OFF)
if(P2C_SHA256_ACCELERATION AND P2C_SHA256_X86_BACKEND AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT SANITIZERS MATCHES "memory")
  if((NOT CMAKE_VS_PLATFORM_NAME OR CMAKE_VS_PLATFORM_NAME STREQUAL "x64") AND
     (NOT APPLE OR NOT CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64"))
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("
      #if defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
      #error Not an x86 target
      #endif
      #if !defined(__x86_64__) && !defined(_M_X64)
      #error SHA-NI requires x86-64
      #endif
      int main() { return 0; }
    " P2C_SHA256_COMPILER_X86_64)
    set(p2c_sha256_x86_target "${P2C_SHA256_COMPILER_X86_64}")
  endif()
endif()
if(P2C_X25519_ACCELERATION)
  target_compile_definitions(everest PUBLIC CONNECTCOIN_X25519_ACCELERATION)
endif()
foreach(p2c_crypto_target IN ITEMS mbedcrypto mbedcrypto_static)
  if(NOT TARGET ${p2c_crypto_target})
    continue()
  endif()
  if(P2C_X25519_ACCELERATION)
    target_sources(${p2c_crypto_target} PRIVATE "${PROJECT_SOURCE_DIR}/src/crypto/mbedtls_x25519.c")
    target_include_directories(${p2c_crypto_target} PRIVATE
      "${connectcoin_mbedtls_SOURCE_DIR}/3rdparty/everest/include/everest"
      "${connectcoin_mbedtls_SOURCE_DIR}/3rdparty/everest/include/everest/kremlib")
    if(P2C_CRYPTO_ACCELERATION_TESTING)
      target_compile_definitions(${p2c_crypto_target} PUBLIC CONNECTCOIN_X25519_TESTING)
    endif()
  endif()
  if(P2C_SHA256_ACCELERATION)
    target_compile_definitions(${p2c_crypto_target} PUBLIC CONNECTCOIN_SHA256_ACCELERATION)
    target_sources(${p2c_crypto_target} PRIVATE
      "${PROJECT_SOURCE_DIR}/src/crypto/mbedtls_sha256.cpp"
      "${PROJECT_SOURCE_DIR}/src/crypto/mbedtls_sha256_portable.c")
    if(P2C_CRYPTO_ACCELERATION_TESTING)
      target_compile_definitions(${p2c_crypto_target} PUBLIC CONNECTCOIN_SHA256_TESTING)
    endif()
    if(p2c_sha256_x86_target)
      target_compile_definitions(${p2c_crypto_target} PRIVATE CONNECTCOIN_SHA256_X86_BACKEND)
      target_sources(${p2c_crypto_target} PRIVATE "${PROJECT_SOURCE_DIR}/src/crypto/mbedtls_sha256_shani.cpp")
      set_source_files_properties("${PROJECT_SOURCE_DIR}/src/crypto/mbedtls_sha256_shani.cpp"
        TARGET_DIRECTORY ${p2c_crypto_target} PROPERTIES COMPILE_DEFINITIONS "ENABLE_SSE41;ENABLE_X86_SHANI")
      if(NOT MSVC)
        set_source_files_properties("${PROJECT_SOURCE_DIR}/src/crypto/mbedtls_sha256_shani.cpp"
          TARGET_DIRECTORY ${p2c_crypto_target} PROPERTIES COMPILE_OPTIONS "-msha;-mssse3;-msse4.1")
      endif()
    endif()
  endif()
endforeach()
unset(p2c_crypto_target)
unset(p2c_sha256_x86_target)
