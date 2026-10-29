// Engine-free helpers for the custom_hud_layout's networked vectors (hud_plugin.cpp; ctest `hud_vec`).
//
// The entity keeps its state in CUtlVector<T>s. In memory a CUtlVector is
//   int m_Size; (pad); T* m_pMemory; int m_nAllocationCount; int m_nGrowSize;
// (24 bytes on linux x86-64). Memory the vector owns comes from tier0's allocator, so growing it
// must use that allocator too; m_nGrowSize < 0 marks external memory that must not be reallocated.
// Elements are moved with memcpy when the buffer grows: every element type we store (CUtlString =
// one char*, the dialog-variable struct with its vtable pointer) is trivially relocatable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hud {

struct RawVec {
  int32_t size;
  int32_t pad;
  void* mem;
  int32_t alloc;
  int32_t grow;
};
static_assert(sizeof(RawVec) == 24, "CUtlVector layout");

// tier0's allocator (MemAlloc_*Func). Tests pass malloc / realloc / free.
struct Allocator {
  void* (*alloc)(size_t) = nullptr;
  void* (*realloc)(void*, size_t) = nullptr;
  void (*free)(void*) = nullptr;
  char* (*strdup)(const char*) = nullptr;
  bool ok() const { return alloc && realloc && free && strdup; }
};

// A vector that is sane to read: non-negative size within its allocation, memory when non-empty.
bool VecLooksValid(const RawVec* v, std::string* why);

// Element i (stride bytes each), or nullptr when out of range.
void* VecAt(const RawVec* v, int i, size_t stride);

// Appends one zeroed element and returns it (nullptr: invalid vector, external memory, alloc failed).
void* VecAppend(RawVec* v, size_t stride, const Allocator& a, std::string* why);

// The index of `s` in a CUtlVector<CUtlString> (elements are char*), or -1.
int StringIndex(const RawVec* v, const char* s);

// StringIndex, appending a copy of `s` (tier0 strdup) when it is missing. -1 on failure.
int StringIndexOrAdd(RawVec* v, const char* s, const Allocator& a, std::string* why);

// Replaces the char* at `slot` with a tier0 copy of `s` (frees the old one). False if strdup fails.
bool SetCUtlString(char** slot, const char* s, const Allocator& a);

// Hex dump of n bytes, 16 per line (for `ru hudkit info`).
std::string HexDump(const void* p, size_t n);

}  // namespace hud
