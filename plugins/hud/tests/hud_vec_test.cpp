// ctest hud_vec: the raw CUtlVector helpers (hud_vec.h) with libc's allocator standing in for tier0.
#include "hud_vec.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int g_fail = 0;
#define CHECK(c)                                              \
  do {                                                        \
    if (!(c)) {                                               \
      std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                               \
    }                                                         \
  } while (0)

char* Dup(const char* s) { return ::strdup(s); }

hud::Allocator Libc() {
  hud::Allocator a;
  a.alloc = &std::malloc;
  a.realloc = &std::realloc;
  a.free = &std::free;
  a.strdup = &Dup;
  return a;
}

void FreeStrings(hud::RawVec* v) {
  for (int i = 0; i < v->size; ++i) std::free(*static_cast<char**>(hud::VecAt(v, i, sizeof(char*))));
  std::free(v->mem);
}

void TestValid() {
  hud::RawVec v{};
  CHECK(hud::VecLooksValid(&v, nullptr));
  v.size = 2;
  CHECK(!hud::VecLooksValid(&v, nullptr));  // elements but no memory, size > alloc
  v = hud::RawVec{};
  v.size = -1;
  CHECK(!hud::VecLooksValid(&v, nullptr));
  CHECK(hud::VecAt(nullptr, 0, 8) == nullptr);
}

void TestStrings() {
  const hud::Allocator a = Libc();
  hud::RawVec v{};
  CHECK(hud::StringIndex(&v, "at_card") == -1);
  CHECK(hud::StringIndexOrAdd(&v, "at_card", a, nullptr) == 0);
  CHECK(hud::StringIndexOrAdd(&v, "at_kind_a", a, nullptr) == 1);
  CHECK(hud::StringIndexOrAdd(&v, "at_card", a, nullptr) == 0);
  for (int i = 0; i < 40; ++i) {  // grows past several reallocations
    char name[16];
    std::snprintf(name, sizeof name, "p%d", i);
    CHECK(hud::StringIndexOrAdd(&v, name, a, nullptr) == i + 2);
  }
  CHECK(v.size == 42 && v.alloc >= 42);
  CHECK(hud::StringIndex(&v, "p39") == 41);
  CHECK(hud::StringIndex(&v, "at_kind_a") == 1);
  FreeStrings(&v);
}

void TestExternalMemory() {
  const hud::Allocator a = Libc();
  char* fixed[1] = {nullptr};
  hud::RawVec v{};
  v.mem = fixed;
  v.alloc = 1;
  v.grow = -1;  // external buffer: may fill it, never reallocate it
  std::string why;
  CHECK(hud::VecAppend(&v, sizeof(char*), a, &why) != nullptr);
  CHECK(hud::VecAppend(&v, sizeof(char*), a, &why) == nullptr);
  CHECK(why == "vector uses external memory");
}

void TestElements() {
  struct Elem {
    const void* vtable;
    uint16_t panel, var;
    char* value;
    bool set;
  };
  static_assert(sizeof(Elem) == 32, "dialog variable element");
  const hud::Allocator a = Libc();
  hud::RawVec v{};
  for (int i = 0; i < 9; ++i) {
    auto* e = static_cast<Elem*>(hud::VecAppend(&v, sizeof(Elem), a, nullptr));
    CHECK(e && e->vtable == nullptr && e->value == nullptr && !e->set);
    e->panel = static_cast<uint16_t>(i);
    CHECK(hud::SetCUtlString(&e->value, "x", a));
  }
  auto* e5 = static_cast<Elem*>(hud::VecAt(&v, 5, sizeof(Elem)));
  CHECK(e5 && e5->panel == 5 && std::strcmp(e5->value, "x") == 0);
  CHECK(hud::SetCUtlString(&e5->value, "7 / 10 ready", a) && std::strcmp(e5->value, "7 / 10 ready") == 0);
  for (int i = 0; i < v.size; ++i) std::free(static_cast<Elem*>(hud::VecAt(&v, i, sizeof(Elem)))->value);
  std::free(v.mem);
}

}  // namespace

int main() {
  TestValid();
  TestStrings();
  TestExternalMemory();
  TestElements();
  if (g_fail) {
    std::fprintf(stderr, "hud_vec: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("hud_vec: ok\n");
  return 0;
}
