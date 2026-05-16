"""Patch Arduino-ESP32 TLS cipher preference for this SDK bundle.

Some local Arduino-ESP32 cores were patched to force ChaCha20-Poly1305
for GitHub TLS, but the ESP32-S3 SDK bundled with PlatformIO 55.3.38
does not compile ChaCha/Poly1305 support. Offering only those disabled
ciphers makes GitHub abort the handshake with a fatal TLS alert.
"""

from pathlib import Path

Import("env")  # type: ignore[name-defined]


def patch_ssl_client() -> None:
    platform = env.PioPlatform()  # type: ignore[name-defined]
    framework_dir = platform.get_package_dir("framework-arduinoespressif32")
    if not framework_dir:
        print("[patch_network_client_secure] framework package not found")
        return

    ssl_client = (
        Path(framework_dir)
        / "libraries"
        / "NetworkClientSecure"
        / "src"
        / "ssl_client.cpp"
    )
    if not ssl_client.exists():
        print(f"[patch_network_client_secure] missing {ssl_client}")
        return

    text = ssl_client.read_text()
    if "MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256" in text:
        return

    legacy = """  // Prefer ChaCha20-Poly1305 on memory-constrained ESP32-S3 builds. The
  // default GitHub TLS negotiation selects AES-GCM, which goes through the
  // ESP AES accelerator and can fail when internal RAM is fragmented
  // ("esp-aes: Failed to allocate memory"). GitHub supports this TLS 1.2
  // suite, and it avoids the AES hardware allocation path entirely.
  static const int preferred_ciphersuites[] = {
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    0
  };
  mbedtls_ssl_conf_max_tls_version(&ssl_client->ssl_conf,
                                   MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_ciphersuites(&ssl_client->ssl_conf,
                                preferred_ciphersuites);
"""
    guarded_chacha = """  // Prefer ChaCha20-Poly1305 on memory-constrained ESP32-S3 builds only
  // when this SDK actually compiled the ChaCha/Poly1305 primitives. Some
  // Arduino-ESP32 SDK bundles leave them disabled; forcing those suites
  // then produces a ClientHello with no usable GitHub cipher and the peer
  // aborts with MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE.
#if defined(MBEDTLS_CHACHAPOLY_C)
  static const int preferred_ciphersuites[] = {
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    0
  };
  mbedtls_ssl_conf_max_tls_version(&ssl_client->ssl_conf,
                                   MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_ciphersuites(&ssl_client->ssl_conf,
                                preferred_ciphersuites);
#endif
"""
    new = """  // Prefer ChaCha20-Poly1305 on memory-constrained ESP32-S3 builds only
  // when this SDK actually compiled the ChaCha/Poly1305 primitives. Some
  // Arduino-ESP32 SDK bundles leave them disabled; forcing those suites
  // then produces a ClientHello with no usable GitHub cipher and the peer
  // aborts with MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE. Otherwise prefer
  // AES-CBC over AES-GCM: GitHub accepts it, and it avoids the ESP AES-GCM
  // allocation path that fails on memory-fragmented badges during OTA streams.
#if defined(MBEDTLS_CHACHAPOLY_C)
  static const int preferred_ciphersuites[] = {
    MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    0
  };
#else
  static const int preferred_ciphersuites[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,
    MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
    0
  };
#endif
  mbedtls_ssl_conf_max_tls_version(&ssl_client->ssl_conf,
                                   MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_ciphersuites(&ssl_client->ssl_conf,
                                preferred_ciphersuites);
"""
    if legacy in text:
        text = text.replace(legacy, new)
    elif guarded_chacha in text:
        text = text.replace(guarded_chacha, new)
    else:
        print("[patch_network_client_secure] cipher block not present; no patch")
        return

    ssl_client.write_text(text)
    print("[patch_network_client_secure] guarded ChaCha and preferred AES-CBC")


patch_ssl_client()
