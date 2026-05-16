#include "OTAHttp.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <Stream.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "esp32-hal-cpu.h"

#include "../api/WiFiService.h"
#include "../infra/PsramAllocator.h"

namespace ota {

ThroughputBoost::ThroughputBoost() {
  prevSleep_ = WiFi.getSleep();
  prevCpuMhz_ = getCpuFrequencyMhz();
  WiFi.setSleep(false);
  if (prevCpuMhz_ < 240) setCpuFrequencyMhz(240);
}

ThroughputBoost::~ThroughputBoost() {
  if (getCpuFrequencyMhz() != prevCpuMhz_ && prevCpuMhz_ != 0) {
    setCpuFrequencyMhz(prevCpuMhz_);
  }
  WiFi.setSleep(prevSleep_);
}

namespace {

bool isHttps(const char* url) {
  return url && std::strncmp(url, "https://", 8) == 0;
}

bool isHttp(const char* url) {
  return url && (std::strncmp(url, "http://", 7) == 0 || isHttps(url));
}

// Configure common HTTPClient options. Caller must already have called
// http.begin().
void applyCommonOptions(HTTPClient& http, uint32_t timeoutMs) {
  http.setTimeout(timeoutMs);
  http.setConnectTimeout(timeoutMs);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", kUserAgent);
  http.addHeader("Accept", "*/*");
  // The ESP32 HTTPClient does not transparently decompress responses,
  // so explicitly ask for raw bytes. Cloudflare-fronted hosts (incl.
  // jsDelivr) otherwise return gzip/brotli to clients that don't pin
  // identity, which then fails JSON parsing with "InvalidInput".
  http.addHeader("Accept-Encoding", "identity");
}

void applyCommonOptionsNoRedirect(HTTPClient& http, uint32_t timeoutMs) {
  applyCommonOptions(http, timeoutMs);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
}

// Accumulates HTTP response bytes into a single contiguous buffer allocated
// with BadgeMemory::allocPreferPsram (PSRAM first). Avoids HTTPClient::getString()
// / StreamString, which reserves the full body on internal heap and competes
// with mbedTLS during HTTPS.
class JsonBodySink : public ::Stream {
 public:
  explicit JsonBodySink(size_t capBytes) : cap_(capBytes) {
    if (capBytes == 0) return;
    buf_ = static_cast<char*>(BadgeMemory::allocPreferPsram(capBytes + 1));
    if (!buf_) return;
    buf_[0] = '\0';
  }

  ~JsonBodySink() override { std::free(buf_); }

  JsonBodySink(const JsonBodySink&) = delete;
  JsonBodySink& operator=(const JsonBodySink&) = delete;

  bool ok() const { return buf_ != nullptr; }
  size_t length() const { return len_; }
  const char* c_str() const { return buf_ ? buf_ : ""; }

  char* release() {
    char* p = buf_;
    buf_ = nullptr;
    len_ = 0;
    cap_ = 0;
    return p;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!buf_ || size == 0) return 0;
    if (len_ >= cap_) return 0;
    const size_t room = cap_ - len_;
    const size_t n = (size < room) ? size : room;
    std::memcpy(buf_ + len_, buffer, n);
    len_ += n;
    buf_[len_] = '\0';
    if (n < size) return 0;
    return size;
  }

 private:
  size_t cap_{0};
  size_t len_{0};
  char* buf_{nullptr};
};

}  // namespace

HttpResult getJson(const char* url, char** outBuf, size_t* outLen,
                   size_t maxBytes, uint32_t timeoutMs) {
  static char sError[80];
  sError[0] = '\0';
  HttpResult result{0, 0, false, sError};
  if (outBuf) *outBuf = nullptr;
  if (outLen) *outLen = 0;

  if (!isHttp(url)) {
    std::snprintf(sError, sizeof(sError), "url must start with http(s)://");
    return result;
  }
  if (!wifiService.connect()) {
    std::snprintf(sError, sizeof(sError), "wifi unavailable");
    return result;
  }

  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  bool began = false;
  String urlStr(url);
  if (isHttps(url)) {
    secure.setInsecure();
    began = http.begin(secure, urlStr);
  } else {
    began = http.begin(plain, urlStr);
  }
  if (!began) {
    std::snprintf(sError, sizeof(sError), "http begin failed");
    wifiService.noteRequestFailed();
    return result;
  }
  applyCommonOptions(http, timeoutMs);

  const int code = http.GET();
  result.httpCode = code;
  if (code <= 0) {
    std::snprintf(sError, sizeof(sError),
                  "http error %d (%s)", code,
                  HTTPClient::errorToString(code).c_str());
    http.end();
    wifiService.noteRequestFailed();
    return result;
  }
  if (code != 200) {
    // Drain a bounded amount of error body without String/getString().
    constexpr size_t kErrBodyCap = 512;
    JsonBodySink errSink(kErrBodyCap);
    if (errSink.ok()) {
      (void)http.writeToStream(&errSink);
    }
    Serial.printf("[ota-http] %d body[%u]: %.*s\n", code,
                  (unsigned)errSink.length(),
                  errSink.length() > 200 ? 200 : (int)errSink.length(),
                  errSink.c_str());
    std::snprintf(sError, sizeof(sError), "http status %d", code);
    http.end();
    wifiService.noteRequestFailed();
    return result;
  }

  const int contentLen = http.getSize();
  if (contentLen > 0 && static_cast<size_t>(contentLen) > maxBytes) {
    std::snprintf(sError, sizeof(sError),
                  "response too large (%d > %u)",
                  contentLen, (unsigned)maxBytes);
    http.end();
    wifiService.noteRequestFailed();
    return result;
  }

  // Stream via HTTPClient::writeToStream so chunked encoding and
  // Content-Length=-1 are decoded by the library (same as getString),
  // but the body buffer is one PSRAM-backed allocation — no
  // StreamString reserve on internal heap.
  JsonBodySink sink(maxBytes);
  if (!sink.ok()) {
    std::snprintf(sError, sizeof(sError), "out of memory");
    http.end();
    wifiService.noteRequestFailed();
    return result;
  }
  const int wr = http.writeToStream(&sink);
  if (wr < 0) {
    std::snprintf(sError, sizeof(sError), "http read %d (%s)", wr,
                  HTTPClient::errorToString(wr).c_str());
    http.end();
    wifiService.noteRequestFailed();
    return result;
  }
  const size_t pos = sink.length();
  char* buf = sink.release();
  http.end();
  wifiService.noteRequestOk();

  if (outBuf) *outBuf = buf;
  else std::free(buf);
  if (outLen) *outLen = pos;
  result.bytesRead = pos;
  result.ok = true;
  result.error = "";
  return result;
}

bool resolveRedirect(const char* url, char* outUrl, size_t outUrlLen,
                     uint32_t timeoutMs) {
  if (outUrl && outUrlLen > 0) outUrl[0] = '\0';
  if (!outUrl || outUrlLen == 0 || !isHttp(url)) {
    return false;
  }
  if (!wifiService.connect()) {
    return false;
  }

  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  bool began = false;
  String urlStr(url);
  if (isHttps(url)) {
    secure.setInsecure();
    began = http.begin(secure, urlStr);
  } else {
    began = http.begin(plain, urlStr);
  }
  if (!began) {
    return false;
  }
  applyCommonOptionsNoRedirect(http, timeoutMs);
  http.addHeader("Range", "bytes=0-0");

  const int code = http.GET();
  const String location = http.getLocation();
  http.end();

  if (code < 300 || code >= 400 || location.length() == 0) {
    return false;
  }
  if (static_cast<size_t>(location.length()) >= outUrlLen) {
    Serial.printf("[ota-http] redirect too long: %u >= %u\n",
                  (unsigned)location.length(), (unsigned)outUrlLen);
    return false;
  }
  std::strncpy(outUrl, location.c_str(), outUrlLen - 1);
  outUrl[outUrlLen - 1] = '\0';
  return true;
}

// ── Stream ─────────────────────────────────────────────────────────────────

Stream::Stream() = default;

Stream::~Stream() { close(); }

bool Stream::open(const char* url, uint32_t timeoutMs, size_t rangeStart) {
  close();
  lastError_[0] = '\0';
  contentLength_ = 0;
  httpCode_ = 0;

  if (!isHttp(url)) {
    std::snprintf(lastError_, sizeof(lastError_),
                  "url must start with http(s)://");
    return false;
  }
  if (!wifiService.connect()) {
    std::snprintf(lastError_, sizeof(lastError_), "wifi unavailable");
    return false;
  }

  http_ = new HTTPClient();
  bool began = false;
  String urlStr(url);
  if (isHttps(url)) {
    auto* sec = new WiFiClientSecure();
    sec->setInsecure();
    // Cap the TLS handshake at the per-request timeout (in seconds).
    // The arduino-esp32 default is 30s which is fine, but tying it to
    // the caller's timeoutMs keeps a stuck handshake from outliving
    // the rest of the budget.
    sec->setHandshakeTimeout(timeoutMs / 1000);
    secure_ = sec;
    began = http_->begin(*sec, urlStr);
  } else {
    auto* p = new WiFiClient();
    plain_ = p;
    began = http_->begin(*p, urlStr);
  }
  if (!began) {
    std::snprintf(lastError_, sizeof(lastError_), "http begin failed");
    close();
    wifiService.noteRequestFailed();
    return false;
  }
  applyCommonOptions(*http_, timeoutMs);
  if (rangeStart > 0) {
    char rangeHdr[40];
    std::snprintf(rangeHdr, sizeof(rangeHdr), "bytes=%u-",
                  static_cast<unsigned>(rangeStart));
    http_->addHeader("Range", rangeHdr);
  }

  const int code = http_->GET();
  httpCode_ = code;
  if (code <= 0) {
    std::snprintf(lastError_, sizeof(lastError_),
                  "http error %d (%s)", code,
                  HTTPClient::errorToString(code).c_str());
    close();
    wifiService.noteRequestFailed();
    return false;
  }
  // 200 OK and 206 Partial Content are both acceptable. Anything else
  // (including 416 Range Not Satisfiable) is a hard fail — the caller
  // should restart from offset 0 by reopening with rangeStart=0.
  if (code != 200 && code != 206) {
    std::snprintf(lastError_, sizeof(lastError_), "http status %d", code);
    close();
    wifiService.noteRequestFailed();
    return false;
  }

  const int len = http_->getSize();
  contentLength_ = (len > 0) ? static_cast<size_t>(len) : 0;
  body_ = http_->getStreamPtr();
  return true;
}

bool Stream::connected() const {
  if (!http_) return false;
  return http_->connected();
}

int Stream::read(uint8_t* buf, size_t len) {
  if (!body_ || !buf || len == 0) return 0;
  // Per-read patience window. 10 s is generous enough that a brief
  // mid-frame WiFi glitch (e.g. AP roaming, neighbouring badge
  // hammering 2.4 GHz with IR/USB harmonics) doesn't immediately
  // collapse the chunk; short enough that a truly dead socket gets
  // surfaced to the caller's outer retry loop within a sensible
  // bound. The caller (AssetRegistry installer) wraps reads in a
  // resume-with-Range retry, so a surfaced timeout here is recoverable
  // rather than fatal.
  const uint32_t kReadWindow = 10000;
  uint32_t deadline = millis() + kReadWindow;
  size_t total = 0;
  while (total < len) {
    int avail = body_->available();
    if (avail <= 0) {
      if ((int32_t)(millis() - deadline) > 0) break;
      if (!http_->connected() && body_->available() <= 0) break;
      delay(2);
      continue;
    }
    int want = static_cast<int>(len - total);
    if (avail < want) want = avail;
    int got = body_->readBytes(buf + total, want);
    if (got <= 0) break;
    total += got;
    deadline = millis() + kReadWindow;
  }
  return static_cast<int>(total);
}

void Stream::close() {
  if (http_) {
    http_->end();
    delete http_;
    http_ = nullptr;
  }
  if (plain_) {
    delete plain_;
    plain_ = nullptr;
  }
  if (secure_) {
    delete secure_;
    secure_ = nullptr;
  }
  body_ = nullptr;
}

}  // namespace ota
