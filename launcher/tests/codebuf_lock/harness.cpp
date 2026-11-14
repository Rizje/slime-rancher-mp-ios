// SR2 Launcher patch 0006 test (launcher/tests/codebuf_lock/run.sh).
// Runs FEXCore's real OnCodeBufferAllocated / InvalidateCodeBuffersCodeRange (extracted from Core.cpp)
// against stand-ins for what they call, modelling the waits seen on the phone:
//  - CIM: CodeInvalidationMutex. Invalidations hold it exclusively (as every caller does).
//  - Every allocation or free of FEX memory raises Wine's memory notification, which asks for an
//    invalidation and so for CIM exclusively, unless this thread already holds it (the nested skip).
//  - InvalidateRange takes the buffer's lookup-cache lock.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <unistd.h>

static std::shared_mutex CIM;
static thread_local bool HoldsCIM = false;
static std::atomic<uint64_t> Progress{0}, Invalidations{0}, Appends{0}, Destroyed{0};

// Wine's memory notification for FEX's own allocations and frees.
static void MemoryNotification() {
  if (HoldsCIM) return;             // LockCodeInvalidationUnlessHeld: nested, skipped
  std::unique_lock lk(CIM);         // InvalidateIntervalInternal takes it exclusively
}

template <class T> struct NotifyingAllocator {
  using value_type = T;
  NotifyingAllocator() = default;
  template <class U> NotifyingAllocator(const NotifyingAllocator<U>&) {}
  T* allocate(size_t n) { MemoryNotification(); return static_cast<T*>(::operator new(n * sizeof(T))); }
  void deallocate(T* p, size_t) { ::operator delete(p); MemoryNotification(); }
  template <class U> bool operator==(const NotifyingAllocator<U>&) const { return true; }
  template <class U> bool operator!=(const NotifyingAllocator<U>&) const { return false; }
};
namespace fextl {
template <class T> using vector = std::vector<T, NotifyingAllocator<T>>;
template <class T> using shared_ptr = std::shared_ptr<T>;
}
#define FEXCORE_PROFILE_SCOPED(x)
#define LOGMAN_THROW_A_FMT(...)
namespace LogMan::Msg { template <class... A> void EFmt(const char*, A&&...) {} }
static std::atomic<int32_t> NextTid{1};
namespace FHU::Syscalls { inline int32_t gettid() { static thread_local int32_t Tid = NextTid++; return Tid; } }

struct LookupCacheT {
  std::mutex Lock;
  void InvalidateRange(uint64_t, uint64_t) { std::lock_guard g(Lock); }
};
namespace CPU {
struct CodeBuffer {
  std::unique_ptr<LookupCacheT> LookupCache {new LookupCacheT};
  void* Ptr = nullptr; size_t AllocatedSize = 0;
  ~CodeBuffer() { Destroyed++; MemoryNotification(); }  // VirtualFree of its JIT memory
};
}
struct ContextImpl {
  struct { bool GlobalJITNaming() const { return false; } } Config;
  struct { void RegisterJITSpace(void*, size_t) {} } Symbols;
  struct { bool try_lock() { return false; } } CodeInvalidationMutex;
  std::mutex CodeBufferListLock;
  std::atomic<int32_t> CodeBufferListOwner {0};
  fextl::vector<std::weak_ptr<CPU::CodeBuffer>> CodeBufferList;
  void OnCodeBufferAllocated(const fextl::shared_ptr<CPU::CodeBuffer>& Buffer);
  void InvalidateCodeBuffersCodeRange(uint64_t Start, uint64_t Length);
};
#include IMPL

int main(int argc, char** argv) {
  const int Seconds = argc > 1 ? atoi(argv[1]) : 4;
  ContextImpl Ctx;
  std::atomic<bool> Stop{false};
  std::vector<std::thread> T;
  // Threads that start, get a code buffer, run a little, and exit (dropping the buffer), like the
  // thread-pool workers .NET started.
  for (int i = 0; i < 6; i++) T.emplace_back([&] {
    while (!Stop) {
      auto B = std::make_shared<CPU::CodeBuffer>();
      Ctx.OnCodeBufferAllocated(B); Appends++; Progress++;
      { std::shared_lock lk(CIM); std::lock_guard g(B->LookupCache->Lock); }  // compile a block
      std::this_thread::yield();
    }
  });
  // The game thread invalidating code (SMC writes from .NET's JIT).
  for (int i = 0; i < 1; i++) T.emplace_back([&] {
    while (!Stop) {
      std::unique_lock lk(CIM); HoldsCIM = true;
      Ctx.InvalidateCodeBuffersCodeRange(0x1000, 0x1000);
      HoldsCIM = false; lk.unlock();
      Invalidations++; Progress++;
    }
  });
  uint64_t Last = 0; int Still = 0;
  for (int s = 0; s < Seconds * 10; s++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    uint64_t P = Progress.load();
    Still = (P == Last) ? Still + 1 : 0; Last = P;
    if (Still >= 20) {
      printf("DEADLOCK: no progress for 2 s (invalidations=%llu appends=%llu destroyed=%llu)\n",
             (unsigned long long)Invalidations, (unsigned long long)Appends, (unsigned long long)Destroyed);
      fflush(stdout); _exit(2);
    }
  }
  Stop = true;
  for (auto& t : T) t.join();
  size_t Left = Ctx.CodeBufferList.size();
  printf("OK: %d s, invalidations=%llu appends=%llu buffers destroyed=%llu, list entries left=%zu\n", Seconds,
         (unsigned long long)Invalidations, (unsigned long long)Appends, (unsigned long long)Destroyed, Left);
  return 0;
}
