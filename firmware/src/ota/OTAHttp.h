// OTAHttp.h — shared HTTP helper for the OTA + asset-registry modules.
//
// Wraps the `HTTPClient` + `WiFiClientSecure::setInsecure()` boilerplate
// used by both `BadgeOTA` (firmware pull from GitHub Releases) and
// `AssetRegistry` (asset manifest + per-asset file downloads). The badge
// firmware is open source with no secrets to protect, so TLS uses
// setInsecure() — there is no certificate pinning. Redirects are
// followed automatically (GitHub redirects release-asset URLs to
// objects.githubusercontent.com).
//
// Two surfaces:
//   `request()` — simple GET into a malloc'd buffer, suitable for
//                 small JSON manifests (≤ 16 KB by default).
//   `Stream`     — streaming open / read / close for large bodies
//                 (firmware .bin, asset files). Caller drains chunks
//                 and feeds them to `Update.write` or filesystem.

#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

class HTTPClient;
class NetworkClient;
class NetworkClientSecure;

namespace ota {

// User-Agent sent on every request. GitHub's API requires a UA header.
constexpr const char* kUserAgent = "TemporalBadge-OTA/1.0";

// Default network timeout (per request). Generous so firmware streams
// don't trip on a slow conference WiFi.
constexpr uint32_t kDefaultTimeoutMs = 20000;

// Cap for `OTAHttp::getJson()` — guard against runaway manifests.
constexpr size_t kJsonMaxBytes = 16 * 1024;

// Result code from a GET. Negative = transport / auth failure (no
// HTTP code yet). Positive = HTTP status.
struct HttpResult {
  int httpCode;        // > 0 = HTTP status, < 0 = HTTPClient error
  size_t bytesRead;
  bool ok;             // httpCode == 200 and (for getJson) under cap
  const char* error;   // human-readable, valid until next call
};

// Fetch a small response into a malloc'd, NUL-terminated buffer.
// Caller must `free(*outBuf)` on success. On failure `*outBuf` is
// nullptr and `result.ok` is false.
HttpResult getJson(const char* url, char** outBuf, size_t* outLen,
                   size_t maxBytes = kJsonMaxBytes,
                   uint32_t timeoutMs = kDefaultTimeoutMs);

// Streaming download. Opens an HTTP GET and exposes the response
// stream so the caller can drain chunks (Update.write, file write).
// `Stream` is non-copyable; close() releases the connection.
class Stream {
 public:
  Stream();
  ~Stream();

  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  // Open the URL. Returns true on HTTP 200. On false, lastError() has
  // a human-readable message.
  bool open(const char* url, uint32_t timeoutMs = kDefaultTimeoutMs);

  // Total content length (0 if server didn't send one).
  size_t contentLength() const { return contentLength_; }

  // True until the stream is exhausted or close() called.
  bool connected() const;

  // Read up to `len` bytes into `buf`. Returns bytes actually read,
  // or -1 on transport error. 0 means EOF.
  int read(uint8_t* buf, size_t len);

  void close();

  const char* lastError() const { return lastError_; }
  int httpCode() const { return httpCode_; }

 private:
  HTTPClient* http_ = nullptr;
  NetworkClient* plain_ = nullptr;
  NetworkClientSecure* secure_ = nullptr;
  ::Stream* body_ = nullptr;
  size_t contentLength_ = 0;
  int httpCode_ = 0;
  char lastError_[80] = {};
};

}  // namespace ota
