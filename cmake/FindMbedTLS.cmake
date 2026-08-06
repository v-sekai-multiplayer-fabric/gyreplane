# Minimal MbedTLS finder (pkg-config based). picotls's mbedtls bridge
# (thirdparty/picoquic/picoquic_mbedtls/) needs mbedcrypto/mbedtls/mbedx509.
find_package(PkgConfig REQUIRED)
pkg_check_modules(MBEDTLS REQUIRED mbedtls mbedcrypto mbedx509)
