#include "OTAHttp.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "../api/WiFiService.h"

namespace ota {

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
}

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

  // Stream into a growable buffer so we don't pre-allocate maxBytes
  // when the actual response is small (most manifests are < 4 KB).
  size_t cap = (contentLen > 0)
                   ? static_cast<size_t>(contentLen) + 1
                   : 2048;
  if (cap > maxBytes + 1) cap = maxBytes + 1;
  char* buf = static_cast<char*>(std::malloc(cap));
  if (!buf) {
    std::snprintf(sError, sizeof(sError), "out of memory");
    http.end();
    wifiService.noteRequestFailed();
    return result;
  }

  size_t pos = 0;
  NetworkClient* stream = http.getStreamPtr();
  const uint32_t deadline = millis() + timeoutMs;
  while (http.connected() && (contentLen <= 0 || pos < (size_t)contentLen)) {
    if ((int32_t)(millis() - deadline) > 0) {
      std::snprintf(sError, sizeof(sError), "read timeout");
      std::free(buf);
      http.end();
      wifiService.noteRequestFailed();
      return result;
    }
    const int avail = stream ? stream->available() : 0;
    if (avail <= 0) {
      delay(5);
      continue;
    }
    if (pos + avail >= cap) {
      size_t newCap = cap * 2;
      if (newCap > maxBytes + 1) newCap = maxBytes + 1;
      if (pos + avail >= newCap) {
        std::snprintf(sError, sizeof(sError),
                      "response exceeded cap (%u)", (unsigned)maxBytes);
        std::free(buf);
        http.end();
        wifiService.noteRequestFailed();
        return result;
      }
      char* grown = static_cast<char*>(std::realloc(buf, newCap));
      if (!grown) {
        std::snprintf(sError, sizeof(sError), "out of memory");
        std::free(buf);
        http.end();
        wifiService.noteRequestFailed();
        return result;
      }
      buf = grown;
      cap = newCap;
    }
    int got = stream->readBytes(buf + pos, avail);
    if (got <= 0) {
      delay(5);
      continue;
    }
    pos += got;
  }

  buf[pos] = '\0';
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

// ── Stream ─────────────────────────────────────────────────────────────────

Stream::Stream() = default;

Stream::~Stream() { close(); }

bool Stream::open(const char* url, uint32_t timeoutMs) {
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
  if (code != 200) {
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
  // Avoid blocking forever — caller manages overall deadlines.
  const uint32_t kReadWindow = 5000;
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
