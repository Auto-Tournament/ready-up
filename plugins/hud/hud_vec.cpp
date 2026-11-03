#include "hud_vec.h"

#include <cstdio>
#include <cstring>

namespace hud {

bool VecLooksValid(const RawVec* v, std::string* why) {
  auto fail = [&](const char* w) {
    if (why) *why = w;
    return false;
  };
  if (!v) return fail("no vector");
  if (v->size < 0 || v->size > 1 << 20) return fail("size out of range");
  if (v->alloc < 0 || v->alloc > 1 << 20) return fail("allocation count out of range");
  if (v->size > v->alloc) return fail("size above allocation count");
  if (v->size > 0 && !v->mem) return fail("elements but no memory");
  return true;
}

void* VecAt(const RawVec* v, int i, size_t stride) {
  if (!v || i < 0 || i >= v->size || !v->mem) return nullptr;
  return static_cast<uint8_t*>(v->mem) + static_cast<size_t>(i) * stride;
}

void* VecAppend(RawVec* v, size_t stride, const Allocator& a, std::string* why) {
  auto fail = [&](const char* w) -> void* {
    if (why) *why = w;
    return nullptr;
  };
  if (!a.ok()) return fail("tier0 allocator not resolved");
  if (!VecLooksValid(v, why)) return nullptr;
  if (v->size == v->alloc) {
    if (v->grow < 0) return fail("vector uses external memory");
    const int32_t cap = v->alloc < 4 ? 4 : v->alloc * 2;
    void* mem = v->mem ? a.realloc(v->mem, static_cast<size_t>(cap) * stride) : a.alloc(static_cast<size_t>(cap) * stride);
    if (!mem) return fail("allocation failed");
    v->mem = mem;
    v->alloc = cap;
  }
  void* e = static_cast<uint8_t*>(v->mem) + static_cast<size_t>(v->size) * stride;
  std::memset(e, 0, stride);
  ++v->size;
  return e;
}

int StringIndex(const RawVec* v, const char* s) {
  if (!v || !s || !VecLooksValid(v, nullptr)) return -1;
  for (int i = 0; i < v->size; ++i) {
    const char* e = *static_cast<char* const*>(VecAt(v, i, sizeof(char*)));
    if (e && std::strcmp(e, s) == 0) return i;
  }
  return -1;
}

int StringIndexOrAdd(RawVec* v, const char* s, const Allocator& a, std::string* why) {
  const int i = StringIndex(v, s);
  if (i >= 0) return i;
  char* copy = a.strdup ? a.strdup(s) : nullptr;
  if (!copy) {
    if (why) *why = "strdup failed";
    return -1;
  }
  void* e = VecAppend(v, sizeof(char*), a, why);
  if (!e) {
    a.free(copy);
    return -1;
  }
  *static_cast<char**>(e) = copy;
  return v->size - 1;
}

bool SetCUtlString(char** slot, const char* s, const Allocator& a) {
  if (!slot || !a.ok()) return false;
  if (*slot && s && std::strcmp(*slot, s) == 0) return true;
  char* copy = a.strdup(s ? s : "");
  if (!copy) return false;
  if (*slot) a.free(*slot);
  *slot = copy;
  return true;
}

std::string HexDump(const void* p, size_t n) {
  std::string out;
  const auto* b = static_cast<const uint8_t*>(p);
  char buf[8];
  for (size_t i = 0; i < n; ++i) {
    if (i % 16 == 0) {
      std::snprintf(buf, sizeof buf, "%s%04zx:", i ? "\n" : "", i);
      out += buf;
    }
    std::snprintf(buf, sizeof buf, " %02x", b[i]);
    out += buf;
  }
  return out;
}

}  // namespace hud
