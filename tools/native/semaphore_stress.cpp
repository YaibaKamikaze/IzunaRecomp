// [native renderer track] Stress test of the guest semaphore / SignalAndWait
// semantics NG2's job system relies on (docs/NATIVE_RENDERER.md, test25).
// Build against the Linux SDK runtime (include paths as out/linux/build.ninja):
//   g++ -std=c++20 -O1 <the -I flags of out/linux/build.ninja> semaphore_stress.cpp \
//       -Lthird_party/rexglue-sdk/out/linux-amd64 -lrexruntime -lpthread -Wl,-rpath,...
// Before the fix it hung within ~20000 batches; now 60000 pass.
// The NG2 job-system pattern (sub_822F2688 / 822F27B8 / 822F2870): 4 workers,
// each signals "idle[i]" and waits on a semaphore; the dispatcher waits for
// all four idle events, releases 4; each worker has one pinned job; the main
// thread waits for "done" (set when the finished count reaches the total).
#include <rex/thread.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>
#include <unistd.h>
using namespace rex::thread;
std::unique_ptr<Semaphore> sem;
std::unique_ptr<Event> idle[8], done;
std::atomic<int> idle_count{0}, finished{0}, total{0};
std::atomic<bool> pinned[4];
int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  printf("start\n");
  sem = Semaphore::Create(0, 64);
  for (auto& e : idle) e = Event::CreateAutoResetEvent(false);
  done = Event::CreateAutoResetEvent(false);
  for (int w = 0; w < 4; ++w) {
    std::thread([w] {
      std::mt19937 rng(w);
      for (;;) {
        int i = ++idle_count;
        SignalAndWait(idle[(i - 1) & 7].get(), sem.get(), false, std::chrono::milliseconds::max());
        if (rng() % 8 == 0) std::this_thread::sleep_for(std::chrono::microseconds(rng() % 300));
        if (pinned[w].exchange(false)) {
          if (++finished == total.load()) done->Set();
        }
      }
    }).detach();
  }
  const int batches = 60000;
  for (int b = 0; b < batches; ++b) {
    if (b < 3 || b % 2000 == 0) { printf("batch %d\n", b); fflush(stdout); }
    std::vector<WaitHandle*> h = {idle[0].get(), idle[1].get(), idle[2].get(), idle[3].get()};
    if (WaitMultiple(h.data(), 4, true, false, std::chrono::milliseconds(3000)).first != WaitResult::kSuccess) {
      printf("batch %d: the workers never all went idle\n", b);
      fflush(stdout); _exit(1);
    }
    idle_count = 0;
    finished = 0;
    total = 4;
    for (auto& p : pinned) p = true;
    sem->Release(4, nullptr);
    if (Wait(done.get(), false, std::chrono::milliseconds(3000)) != WaitResult::kSuccess) {
      printf("batch %d: HANG - a pinned job never ran (finished %d of 4)\n", b, finished.load());
      fflush(stdout); _exit(1);
    }
  }
  printf("%d batches, no hang\n", batches);
  fflush(stdout);
  _exit(0);
}
