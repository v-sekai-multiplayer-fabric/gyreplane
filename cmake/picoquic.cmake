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

# Same exclusions as SCsub: OpenSSL/Fusion/minicrypto TLS backends unused
# (mbedtls only), Windows-only packet loop unused on Linux.
list(FILTER PICOQUIC_CORE_SOURCES EXCLUDE REGEX ".*picoquic_ptls_openssl\\.c$")
list(FILTER PICOQUIC_CORE_SOURCES EXCLUDE REGEX ".*picoquic_ptls_fusion\\.c$")
list(FILTER PICOQUIC_CORE_SOURCES EXCLUDE REGEX ".*picoquic_ptls_minicrypto\\.c$")
list(FILTER PICOQUIC_CORE_SOURCES EXCLUDE REGEX ".*winsockloop\\.c$")

set(PICOTLS_SOURCES
    ${PICOTLS_ROOT}/lib/picotls.c
    ${PICOTLS_ROOT}/lib/pembase64.c
    ${PICOTLS_ROOT}/lib/hpke.c
    ${PICOTLS_ROOT}/lib/asn1.c
)

add_library(picoquic_vendored STATIC
    ${PICOQUIC_CORE_SOURCES}
    ${PICOHTTP_SOURCES}
    ${PICOQUIC_MBEDTLS_SOURCES}
    ${PICOTLS_SOURCES}
)

target_include_directories(picoquic_vendored PUBLIC
    ${PICOTLS_ROOT}/include
    ${PICOQUIC_ROOT}/picoquic
    ${PICOQUIC_ROOT}/picohttp
    ${PICOQUIC_ROOT}/picoquic_mbedtls
    ${MBEDTLS_INCLUDE_DIRS}
)

target_compile_definitions(picoquic_vendored PUBLIC
    PTLS_WITHOUT_OPENSSL
    PTLS_WITHOUT_FUSION
    PICOQUIC_WITH_MBEDTLS
    DISABLE_DEBUG_PRINTF
)

target_link_libraries(picoquic_vendored PUBLIC ${MBEDTLS_LIBRARIES})

# NOTE: the three godot_patches/*.patch files (thirdparty/picoquic-godot-patches/)
# are mostly Windows/MinGW path-separator fixes -- not yet confirmed necessary
# on Linux. Apply and verify before relying on ech.c or private-key-loading
# behavior matching the Godot client exactly; tracked as a follow-up, not
# silently assumed irrelevant.
