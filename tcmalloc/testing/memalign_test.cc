// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Check memalign related routines.
//
// We can't really do a huge amount of checking, but at the very
// least, the following code checks that return values are properly
// aligned, and that writing into the objects works.

#define _XOPEN_SOURCE 600  // to get posix_memalign
#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <iostream>
#include <memory>
#include <vector>

#include "benchmark/benchmark.h"
#include "gtest/gtest.h"
#include "absl/random/random.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace {

// Return the next interesting size/delta to check.  Returns -1 if no more.
int NextSize(int size) {
  if (size < 100) {
    return size+1;
  } else if (size < 1048576) {
    // Find next power of two
    int power = 1;
    while (power < size) {
      power <<= 1;
    }

    // Yield (power-1, power, power+1)
    if (size < power-1) {
      return power-1;
    } else if (size == power-1) {
      return power;
    } else {
      assert(size == power);
      return power+1;
    }
  } else {
    return -1;
  }
}

// Check alignment
void CheckAlignment(void* p, int align) {
  ASSERT_EQ(0, reinterpret_cast<uintptr_t>(p) % align)
      << "wrong alignment; wanted 0x" << std::hex << align << "; got " << p;
}

// Fill a buffer of the specified size with a predetermined pattern
void Fill(void* p, int n, char seed) {
  unsigned char* buffer = reinterpret_cast<unsigned char*>(p);
  for (int i = 0; i < n; i++) {
    buffer[i] = ((seed + i) & 0xff);
  }
}

// Check that the specified buffer has the predetermined pattern
// generated by Fill()
bool Valid(const void* p, int n, char seed) {
  const unsigned char* buffer = reinterpret_cast<const unsigned char*>(p);
  for (int i = 0; i < n; i++) {
    if (buffer[i] != ((seed + i) & 0xff)) {
      return false;
    }
  }
  return true;
}

// Check that we do not fail catastrophically when we allocate a pointer with
// aligned_alloc and then realloc it.  Note:  realloc is not expected to
// preserve alignment.
TEST(MemalignTest, AlignedAllocRealloc) {
  absl::BitGen rand;

  struct alloc {
    void* ptr;
    size_t size;
    size_t alignment;
  };

  std::vector<alloc> allocated;
  for (int i = 0; i < 100; ++i) {
    alloc a;
    a.size = absl::LogUniform(rand, 0, 1 << 20);
    a.alignment = 1 << absl::Uniform(rand, 0, 6);

    a.size = (a.size + a.alignment - 1) & ~(a.alignment - 1);

    a.ptr = aligned_alloc(a.alignment, a.size);
    ASSERT_TRUE(a.ptr != nullptr);
    ASSERT_EQ(0, reinterpret_cast<uintptr_t>(a.ptr) %
                     static_cast<size_t>(a.alignment));
    allocated.emplace_back(a);
  }

  for (int i = 0; i < 100; ++i) {
    size_t new_size = absl::LogUniform(rand, 0, 1 << 20);
    void* new_ptr = realloc(allocated[i].ptr, new_size);
    ASSERT_TRUE(new_size == 0 || new_ptr != nullptr)
        << allocated[i].size << " " << new_size;
    allocated[i].ptr = new_ptr;
  }

  for (int i = 0; i < 100; ++i) {
    free(allocated[i].ptr);
  }
}

// Produces a vector of sizes to allocate, all with the specified alignment.
std::vector<size_t> SizesWithAlignment(size_t align) {
  std::vector<size_t> v;
  for (size_t s = 0; s < 100; s += align) {
    v.push_back(s + align);
  }

  for (size_t s = 128; s < 1048576; s *= 2) {
    if (s <= align) {
      continue;
    }

    v.push_back(s - align);
    v.push_back(s);
    v.push_back(s + align);
  }

  return v;
}

TEST(MemalignTest, AlignedAlloc) {
  // Try allocating data with a bunch of alignments and sizes
  for (int a = 1; a < 1048576; a *= 2) {
    for (auto s : SizesWithAlignment(a)) {
      void* ptr = aligned_alloc(a, s);
      CheckAlignment(ptr, a);
      Fill(ptr, s, 'x');
      ASSERT_TRUE(Valid(ptr, s, 'x'));
      free(ptr);
    }
  }

  // Grab some memory so that the big allocation below will definitely fail.
  // This allocates 4MB of RAM, therefore the request below for 2^64-4KB*i will
  // fail as it cannot possibly be represented in our address space, since
  //   4MB + (2^64-4KB*i) > 2^64 for i = {1...kMinusNTimes}
  void* p_small = malloc(4 * 1048576);
  ASSERT_NE(nullptr, p_small);

  // Make sure overflow is returned as nullptr.
  const size_t zero = 0;
  static const size_t kMinusNTimes = 10;
  for (size_t i = 1; i < kMinusNTimes; ++i) {
    EXPECT_EQ(nullptr, aligned_alloc(1024, zero - 1024 * i));
  }

  free(p_small);
}

#ifndef NDEBUG
TEST(MemalignTest, AlignedAllocDeathTest) {
  EXPECT_DEATH(benchmark::DoNotOptimize(aligned_alloc(0, 1)), "");
  EXPECT_DEATH(benchmark::DoNotOptimize(aligned_alloc(sizeof(void*) + 1, 1)),
               "");
  EXPECT_DEATH(benchmark::DoNotOptimize(aligned_alloc(4097, 1)), "");
}
#endif

TEST(MemalignTest, Memalign) {
  // Try allocating data with a bunch of alignments and sizes
  for (int a = 1; a < 1048576; a *= 2) {
    for (auto s : SizesWithAlignment(a)) {
      void* ptr = memalign(a, s);
      CheckAlignment(ptr, a);
      Fill(ptr, s, 'x');
      ASSERT_TRUE(Valid(ptr, s, 'x'));
      free(ptr);
    }
  }

  {
    // Check various corner cases
    void* p1 = memalign(1<<20, 1<<19);
    void* p2 = memalign(1<<19, 1<<19);
    void* p3 = memalign(1<<21, 1<<19);
    CheckAlignment(p1, 1<<20);
    CheckAlignment(p2, 1<<19);
    CheckAlignment(p3, 1<<21);
    Fill(p1, 1<<19, 'a');
    Fill(p2, 1<<19, 'b');
    Fill(p3, 1<<19, 'c');
    ASSERT_TRUE(Valid(p1, 1 << 19, 'a'));
    ASSERT_TRUE(Valid(p2, 1 << 19, 'b'));
    ASSERT_TRUE(Valid(p3, 1 << 19, 'c'));
    free(p1);
    free(p2);
    free(p3);
  }
}

TEST(MemalignTest, PosixMemalign) {
  // Try allocating data with a bunch of alignments and sizes
  for (int a = sizeof(void*); a < 1048576; a *= 2) {
    for (auto s : SizesWithAlignment(a)) {
      void* ptr;
      ASSERT_EQ(0, posix_memalign(&ptr, a, s));
      CheckAlignment(ptr, a);
      Fill(ptr, s, 'x');
      ASSERT_TRUE(Valid(ptr, s, 'x'));
      free(ptr);
    }
  }
}

TEST(MemalignTest, PosixMemalignFailure) {
  void* ptr;
  ASSERT_EQ(posix_memalign(&ptr, 0, 1), EINVAL);
  ASSERT_EQ(posix_memalign(&ptr, sizeof(void*) / 2, 1), EINVAL);
  ASSERT_EQ(posix_memalign(&ptr, sizeof(void*) + 1, 1), EINVAL);
  ASSERT_EQ(posix_memalign(&ptr, 4097, 1), EINVAL);

  // Grab some memory so that the big allocation below will definitely fail.
  void* p_small = malloc(4 * 1048576);
  ASSERT_NE(p_small, nullptr);

  // Make sure overflow is returned as ENOMEM
  const size_t zero = 0;
  static const size_t kMinusNTimes = 10;
  for (size_t i = 1; i < kMinusNTimes; ++i) {
    int r = posix_memalign(&ptr, 1024, zero - i);
    ASSERT_EQ(r, ENOMEM);
  }

  free(p_small);
}

TEST(MemalignTest, valloc) {
  const int pagesize = getpagesize();

  for (int s = 0; s != -1; s = NextSize(s)) {
    void* p = valloc(s);
    CheckAlignment(p, pagesize);
    Fill(p, s, 'v');
    ASSERT_TRUE(Valid(p, s, 'v'));
    free(p);
  }
}

TEST(MemalignTest, pvalloc) {
  const int pagesize = getpagesize();

  for (int s = 0; s != -1; s = NextSize(s)) {
    void* p = pvalloc(s);
    CheckAlignment(p, pagesize);
    int alloc_needed = ((s + pagesize - 1) / pagesize) * pagesize;
    Fill(p, alloc_needed, 'x');
    ASSERT_TRUE(Valid(p, alloc_needed, 'x'));
    free(p);
  }

  // should be safe to write upto a page in pvalloc(0) region
  void* p = pvalloc(0);
  Fill(p, pagesize, 'y');
  ASSERT_TRUE(Valid(p, pagesize, 'y'));
  free(p);
}

}  // namespace
}  // namespace tcmalloc
