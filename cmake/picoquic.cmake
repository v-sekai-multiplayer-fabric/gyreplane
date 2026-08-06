# picoquic + picotls transport, vendored to match the Godot fork's exact
# picoquic backend (V-Sekai-fire/multiplayer-fabric-build, godot/modules/http3),
# so the client (WebTransportPeer) and this server share one QUIC stack.
#
# Mirrors godot/modules/http3/SCsub's source selection and defines. mbedtls
# is used as the TLS 1.3 backend (not OpenSSL) to match that module exactly --
# picoquic's mbedtls glue lives in thirdparty/picoquic/picoquic_mbedtls/.

find_package(MbedTLS REQUIRED)

set(PICOQUIC_ROOT ${CMAKE_SOURCE_DIR}/thirdparty/picoquic)
set(PICOTLS_ROOT ${CMAKE_SOURCE_DIR}/thirdparty/picotls)

file(GLOB PICOQUIC_CORE_SOURCES "${PICOQUIC_ROOT}/picoquic/*.c")
file(GLOB PICOHTTP_SOURCES "${PICOQUIC_ROOT}/picohttp/*.c")
file(GLOB PICOQUIC_MBEDTLS_SOURCES "${PICOQUIC_ROOT}/picoquic_mbedtls/*.c")

# OpenSSL and Fusion backends are unused (mbedtls only), and each has its
# own #ifndef PTLS_WITHOUT_OPENSSL / #if !defined(PTLS_WITHOUT_FUSION) guard
# in tls_api.c, so leaving the .c files out of the build is safe.
#
# minicrypto is different: tls_api.c's picoquic_tls_api_init_providers()
# calls picoquic_ptls_minicrypto_load() with no compile-time guard at all
# (only a runtime flag check, TLS_API_INIT_FLAGS_NO_MINICRYPTO, which
# defaults to unset). Excluding picoquic_ptls_minicrypto.c left that call
# an undefined symbol at link time -- confirmed by the real build's own
# linker error ("undefined reference to picoquic_ptls_minicrypto_load"),
# not guessed. thirdparty/picoquic-godot-patches/0002-godot-fixes.patch
# itself patches ech.c's own minicrypto.h include, confirming the Godot
# fork this vendoring mirrors compiles minicrypto in too. mbedtls still
# wins as the active provider: picoquic_tls_api_init_providers() registers
# minicrypto first and mbedtls last, and "the latest registration wins"
# per tls_api.c's own comment.
list(FILTER PICOQUIC_CORE_SOURCES EXCLUDE REGEX ".*picoquic_ptls_openssl\\.c$")
list(FILTER PICOQUIC_CORE_SOURCES EXCLUDE REGEX ".*picoquic_ptls_fusion\\.c$")
list(FILTER PICOQUIC_CORE_SOURCES EXCLUDE REGEX ".*winsockloop\\.c$")

set(PICOTLS_SOURCES
    ${PICOTLS_ROOT}/lib/picotls.c
    ${PICOTLS_ROOT}/lib/pembase64.c
    ${PICOTLS_ROOT}/lib/hpke.c
    ${PICOTLS_ROOT}/lib/asn1.c
)

# picoquic_ptls_minicrypto.c (kept in the build, see the exclusion-list
# comment below) links against picotls's own minicrypto backend, which is
# a separate library target (picotls-minicrypto) in picotls's own
# CMakeLists.txt, not part of core picotls.c. Mirroring that file list
# exactly here rather than guessing which subset is needed.
set(PICOTLS_MINICRYPTO_SOURCES
    ${PICOTLS_ROOT}/deps/micro-ecc/uECC.c
    ${PICOTLS_ROOT}/deps/cifra/src/aes.c
    ${PICOTLS_ROOT}/deps/cifra/src/blockwise.c
    ${PICOTLS_ROOT}/deps/cifra/src/chacha20.c
    ${PICOTLS_ROOT}/deps/cifra/src/chash.c
    ${PICOTLS_ROOT}/deps/cifra/src/curve25519.c
    ${PICOTLS_ROOT}/deps/cifra/src/drbg.c
    ${PICOTLS_ROOT}/deps/cifra/src/hmac.c
    ${PICOTLS_ROOT}/deps/cifra/src/gcm.c
    ${PICOTLS_ROOT}/deps/cifra/src/gf128.c
    ${PICOTLS_ROOT}/deps/cifra/src/modes.c
    ${PICOTLS_ROOT}/deps/cifra/src/poly1305.c
    ${PICOTLS_ROOT}/deps/cifra/src/sha256.c
    ${PICOTLS_ROOT}/deps/cifra/src/sha512.c
    ${PICOTLS_ROOT}/lib/cifra.c
    ${PICOTLS_ROOT}/lib/cifra/x25519.c
    ${PICOTLS_ROOT}/lib/cifra/chacha20.c
    ${PICOTLS_ROOT}/lib/cifra/aes128.c
    ${PICOTLS_ROOT}/lib/cifra/aes256.c
    ${PICOTLS_ROOT}/lib/cifra/random.c
    ${PICOTLS_ROOT}/lib/minicrypto-pem.c
    ${PICOTLS_ROOT}/lib/uecc.c
    ${PICOTLS_ROOT}/lib/ffx.c
)

add_library(picoquic_vendored STATIC
    ${PICOQUIC_CORE_SOURCES}
    ${PICOHTTP_SOURCES}
    ${PICOQUIC_MBEDTLS_SOURCES}
    ${PICOTLS_SOURCES}
    ${PICOTLS_MINICRYPTO_SOURCES}
)

# BEFORE matters here: h2o's own CMakeLists.txt installs its bundled,
# different-version deps/picotls/include/*.h into /opt/h2o/include (the
# main CMakeLists.txt's global include_directories(... ${H2O_INCLUDE})
# picks that up for every target, including this one). Without BEFORE,
# the compiler found h2o's older picotls.h ahead of this vendored one
# when compiling thirdparty/picotls/lib/picotls.c against it --
# "conflicting types for ptls_build_v4_mapped_v6_address", "unknown
# type name ptls_log_getsni_t" -- confirmed by reading h2o's own
# CMakeLists.txt install rules directly, not guessed. This ensures our
# own picotls headers are found first for this target regardless of
# global include-directory ordering.
target_include_directories(picoquic_vendored BEFORE PUBLIC
    ${PICOTLS_ROOT}/include
    ${PICOQUIC_ROOT}/picoquic
    ${PICOQUIC_ROOT}/picohttp
    ${PICOQUIC_ROOT}/picoquic_mbedtls
    ${MBEDTLS_INCLUDE_DIRS}
)

# picotls-minicrypto's own include path, per picotls's CMakeLists.txt's
# INCLUDE_DIRECTORIES() call -- cifra's headers use "ext/..." and
# "bitops.h"-style relative includes that only resolve with these two
# directories on the path, and deps/micro-ecc/uECC.h needs its own dir too.
target_include_directories(picoquic_vendored PRIVATE
    ${PICOTLS_ROOT}/deps/cifra/src/ext
    ${PICOTLS_ROOT}/deps/cifra/src
    ${PICOTLS_ROOT}/deps/micro-ecc
)

target_compile_definitions(picoquic_vendored PUBLIC
    PTLS_WITHOUT_OPENSSL
    PTLS_WITHOUT_FUSION
    PICOQUIC_WITH_MBEDTLS
    DISABLE_DEBUG_PRINTF
)

# MBEDTLS_LIBRARIES (from pkg_check_modules) is just bare names
# (-lmbedtls -lmbedcrypto -lmbedx509) -- the linker needs
# MBEDTLS_LIBRARY_DIRS too (/opt/mbedtls/lib in CI), or it fails with
# "cannot find -lmbedtls" even though the .so files exist, just not on
# ld's default search path. Confirmed by reading the actual link error,
# not guessed.
target_link_directories(picoquic_vendored PUBLIC ${MBEDTLS_LIBRARY_DIRS})
target_link_libraries(picoquic_vendored PUBLIC ${MBEDTLS_LIBRARIES})

# NOTE: the three godot_patches/*.patch files (thirdparty/picoquic-godot-patches/)
# are mostly Windows/MinGW path-separator fixes -- not yet confirmed necessary
# on Linux. Apply and verify before relying on ech.c or private-key-loading
# behavior matching the Godot client exactly; tracked as a follow-up, not
# silently assumed irrelevant.
