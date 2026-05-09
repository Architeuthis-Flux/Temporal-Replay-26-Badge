#include "MenuOrderStore.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstdio>
#include <cstring>

namespace MenuOrderStore {

namespace {

constexpr const char* kNamespace = "menu_order";

// FNV-1a 32-bit. Compact and good enough for the tiny label space; the
// bottom 8 hex chars become the NVS key (12 chars total with the "o"
// prefix, well under the 15-char NVS limit).
uint32_t fnv1a(const char* s) {
  uint32_t h = 0x811C9DC5u;
  for (; s && *s; s++) {
    h ^= static_cast<uint8_t>(*s);
    h *= 0x01000193u;
  }
  return h;
}

void formatKey(const char* label, char out[16]) {
  std::snprintf(out, 16, "o%08lx", static_cast<unsigned long>(fnv1a(label)));
}

}  // namespace

int16_t lookup(const char* label) {
  if (!label || !*label) return kNoOverride;
  Preferences p;
  if (!p.begin(kNamespace, true)) return kNoOverride;
  char key[16];
  formatKey(label, key);
  if (!p.isKey(key)) {
    p.end();
    return kNoOverride;
  }
  int16_t v = p.getShort(key, kNoOverride);
  p.end();
  return v;
}

void put(const char* label, int16_t order) {
  if (!label || !*label) return;
  Preferences p;
  if (!p.begin(kNamespace, false)) return;
  char key[16];
  formatKey(label, key);
  p.putShort(key, order);
  p.end();
}

void erase(const char* label) {
  if (!label || !*label) return;
  Preferences p;
  if (!p.begin(kNamespace, false)) return;
  char key[16];
  formatKey(label, key);
  p.remove(key);
  p.end();
}

void clearAll() {
  Preferences p;
  if (!p.begin(kNamespace, false)) return;
  p.clear();
  p.end();
}

size_t count() {
  Preferences p;
  if (!p.begin(kNamespace, true)) return 0;
  size_t n = p.freeEntries();  // ESP-IDF NVS counts free slots; not direct.
  // No direct "used count" API; report 0 for callers that just need a
  // boolean "has anything?" check via clearAll. Keeping the symbol so
  // future code can grow into it without breaking the header.
  (void)n;
  p.end();
  return 0;
}

}  // namespace MenuOrderStore
