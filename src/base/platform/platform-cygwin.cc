// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Platform-specific code for Cygwin goes here. For the POSIX-compatible
// parts, the implementation is in platform-posix.cc.

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <strings.h>    // index
#include <sys/mman.h>   // mmap & munmap
#include <sys/time.h>
#include <unistd.h>     // sysconf

#include <cmath>

#undef MAP_TYPE

#include "src/base/macros.h"
#include "src/base/platform/platform-posix.h"
#include "src/base/platform/platform.h"
#include "src/base/win32-headers.h"

namespace v8 {
namespace base {

namespace {

// The VirtualMemory implementation is taken from platform-win32.cc.
// The mmap-based virtual memory implementation as it is used on most posix
// platforms does not work well because Cygwin does not support MAP_FIXED.
// This causes VirtualMemory::Commit to not always commit the memory region
// specified.

static void* RandomizedVirtualAlloc(size_t size, int action, int protection,
                                    void* hint) {
  LPVOID base = nullptr;

  if (protection == PAGE_EXECUTE_READWRITE || protection == PAGE_NOACCESS) {
    // For exectutable pages try and randomize the allocation address
    base = VirtualAlloc(hint, size, action, protection);
  }

  // After three attempts give up and let the OS find an address to use.
  if (base == nullptr) base = VirtualAlloc(nullptr, size, action, protection);

  return base;
}

}  // namespace

class CygwinTimezoneCache : public PosixTimezoneCache {
  const char* LocalTimezone(double time) override;

  double LocalTimeOffset() override;

  ~CygwinTimezoneCache() override {}
};

const char* CygwinTimezoneCache::LocalTimezone(double time) {
  if (std::isnan(time)) return "";
  time_t tv = static_cast<time_t>(std::floor(time/msPerSecond));
  struct tm tm;
  struct tm* t = localtime_r(&tv, &tm);
  if (nullptr == t) return "";
  return tzname[0];  // The location of the timezone string on Cygwin.
}

double CygwinTimezoneCache::LocalTimeOffset() {
  // On Cygwin, struct tm does not contain a tm_gmtoff field.
  time_t utc = time(nullptr);
  DCHECK(utc != -1);
  struct tm tm;
  struct tm* loc = localtime_r(&utc, &tm);
  DCHECK(loc != nullptr);
  // time - localtime includes any daylight savings offset, so subtract it.
  return static_cast<double>((mktime(loc) - utc) * msPerSecond -
                             (loc->tm_isdst > 0 ? 3600 * msPerSecond : 0));
}

void* OS::Allocate(const size_t requested, size_t* allocated,
                   OS::MemoryPermission access, void* hint) {
  const size_t msize = RoundUp(requested, sysconf(_SC_PAGESIZE));
  int prot = GetProtectionFromMemoryPermission(access);
  void* mbase = mmap(nullptr, msize, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mbase == MAP_FAILED) return nullptr;
  *allocated = msize;
  return mbase;
}

// static
void* OS::ReserveRegion(size_t size, void* hint) {
  return RandomizedVirtualAlloc(size, MEM_RESERVE, PAGE_NOACCESS, hint);
}

// static
void* OS::ReserveAlignedRegion(size_t size, size_t alignment, void* hint,
                               size_t* allocated) {
  hint = AlignedAddress(hint, alignment);
  DCHECK((alignment % OS::AllocateAlignment()) == 0);
  size_t request_size =
      RoundUp(size + alignment, static_cast<intptr_t>(OS::AllocateAlignment()));
  void* address = ReserveRegion(request_size, hint);
  if (address == nullptr) {
    *allocated = 0;
    return nullptr;
  }
  uint8_t* base = RoundUp(static_cast<uint8_t*>(address), alignment);
  // Try reducing the size by freeing and then reallocating a specific area.
  bool result = ReleaseRegion(address, request_size);
  USE(result);
  DCHECK(result);
  address = VirtualAlloc(base, size, MEM_RESERVE, PAGE_NOACCESS);
  if (address != nullptr) {
    request_size = size;
    DCHECK(base == static_cast<uint8_t*>(address));
  } else {
    // Resizing failed, just go with a bigger area.
    address = ReserveRegion(request_size, hint);
    if (address == nullptr) {
      *allocated = 0;
      return nullptr;
    }
  }

  *allocated = request_size;
  return static_cast<void*>(address);
}

// static
bool OS::CommitRegion(void* address, size_t size, bool is_executable) {
  int prot = is_executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
  if (nullptr == VirtualAlloc(address, size, MEM_COMMIT, prot)) {
    return false;
  }
  return true;
}

// static
bool OS::UncommitRegion(void* address, size_t size) {
  return VirtualFree(address, size, MEM_DECOMMIT) != 0;
}

// static
bool OS::ReleaseRegion(void* address, size_t size) {
  return VirtualFree(address, 0, MEM_RELEASE) != 0;
}

// static
bool OS::ReleasePartialRegion(void* address, size_t size) {
  return VirtualFree(address, size, MEM_DECOMMIT) != 0;
}

// static
bool OS::HasLazyCommits() {
  // TODO(alph): implement for the platform.
  return false;
}

std::vector<OS::SharedLibraryAddress> OS::GetSharedLibraryAddresses() {
  std::vector<SharedLibraryAddresses> result;
  // This function assumes that the layout of the file is as follows:
  // hex_start_addr-hex_end_addr rwxp <unused data> [binary_file_name]
  // If we encounter an unexpected situation we abort scanning further entries.
  FILE* fp = fopen("/proc/self/maps", "r");
  if (fp == nullptr) return result;

  // Allocate enough room to be able to store a full file name.
  const int kLibNameLen = FILENAME_MAX + 1;
  char* lib_name = reinterpret_cast<char*>(malloc(kLibNameLen));

  // This loop will terminate once the scanning hits an EOF.
  while (true) {
    uintptr_t start, end;
    char attr_r, attr_w, attr_x, attr_p;
    // Parse the addresses and permission bits at the beginning of the line.
    if (fscanf(fp, "%" V8PRIxPTR "-%" V8PRIxPTR, &start, &end) != 2) break;
    if (fscanf(fp, " %c%c%c%c", &attr_r, &attr_w, &attr_x, &attr_p) != 4) break;

    int c;
    if (attr_r == 'r' && attr_w != 'w' && attr_x == 'x') {
      // Found a read-only executable entry. Skip characters until we reach
      // the beginning of the filename or the end of the line.
      do {
        c = getc(fp);
      } while ((c != EOF) && (c != '\n') && (c != '/'));
      if (c == EOF) break;  // EOF: Was unexpected, just exit.

      // Process the filename if found.
      if (c == '/') {
        ungetc(c, fp);  // Push the '/' back into the stream to be read below.

        // Read to the end of the line. Exit if the read fails.
        if (fgets(lib_name, kLibNameLen, fp) == nullptr) break;

        // Drop the newline character read by fgets. We do not need to check
        // for a zero-length string because we know that we at least read the
        // '/' character.
        lib_name[strlen(lib_name) - 1] = '\0';
      } else {
        // No library name found, just record the raw address range.
        snprintf(lib_name, kLibNameLen,
                 "%08" V8PRIxPTR "-%08" V8PRIxPTR, start, end);
      }
      result.push_back(SharedLibraryAddress(lib_name, start, end));
    } else {
      // Entry not describing executable data. Skip to end of line to set up
      // reading the next entry.
      do {
        c = getc(fp);
      } while ((c != EOF) && (c != '\n'));
      if (c == EOF) break;
    }
  }
  free(lib_name);
  fclose(fp);
  return result;
}

void OS::SignalCodeMovingGC(void* hint) {
  // Nothing to do on Cygwin.
}

}  // namespace base
}  // namespace v8
