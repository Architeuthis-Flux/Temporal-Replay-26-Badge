#pragma once

#include <Arduino.h>
#include <Stream.h>
#include <cstdlib>
#include <cstring>

#include "PsramAllocator.h"

namespace BadgeMemory {

// Append target for HTTPClient::writeToStream. Allocates the body buffer with
// allocPreferPsram so HTTPS responses do not use StreamString / getString(),
// which reserve the full Content-Length on internal heap alongside mbedTLS.
class PsramBodySink : public ::Stream {
 public:
  explicit PsramBodySink(size_t capBytes) : cap_(capBytes) {
    if (capBytes == 0) return;
    buf_ = static_cast<char*>(allocPreferPsram(capBytes + 1));
    if (!buf_) return;
    buf_[0] = '\0';
  }

  ~PsramBodySink() override { std::free(buf_); }

  PsramBodySink(const PsramBodySink&) = delete;
  PsramBodySink& operator=(const PsramBodySink&) = delete;

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

}  // namespace BadgeMemory
