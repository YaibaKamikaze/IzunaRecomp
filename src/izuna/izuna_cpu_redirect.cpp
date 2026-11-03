// [IzunaRecomp] ng2.exe is built for x86-64-v3 (AVX2, FMA, BMI1/2, MOVBE:
// every Intel CPU since Haswell, 2013, and every AMD Ryzen): the recompiled
// game's 3,530 fused multiply-adds become one instruction each instead of a
// call (as on the PS5's Zen 2), its byte swaps on loads and stores MOVBE, and
// the compiler may use 256-bit vectors. A CPU without them would crash on the
// first such instruction - so before any of that code runs, this checks the
// CPU and, if it falls short, starts ng2-compat.exe (the x86-64-v2 build,
// beside it) with the same command line and leaves.
//
// It runs from a TLS callback at process start (before anything else of the
// program), and again - should that not be wired - from the C runtime's first
// C++ initializer slot (.CRT$XCB, before every static constructor in
// .CRT$XCU). This file alone is compiled for the baseline x86-64
// (CMakeLists.txt), calling only Windows.

#if defined(_WIN32) && defined(IZUNA_CPU_V3)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <intrin.h>

namespace {

bool CpuHasV3() {
  int r[4];
  __cpuid(r, 0);
  const int max_leaf = r[0];
  if (max_leaf < 7) return false;
  __cpuid(r, 1);
  const unsigned ecx1 = unsigned(r[2]);
  const bool fma = ecx1 & (1u << 12), movbe = ecx1 & (1u << 22), popcnt = ecx1 & (1u << 23),
             osxsave = ecx1 & (1u << 27), avx = ecx1 & (1u << 28), f16c = ecx1 & (1u << 29);
  __cpuidex(r, 7, 0);
  const unsigned ebx7 = unsigned(r[1]);
  const bool bmi1 = ebx7 & (1u << 3), avx2 = ebx7 & (1u << 5), bmi2 = ebx7 & (1u << 8);
  __cpuid(r, int(0x80000001u));
  const bool lzcnt = unsigned(r[2]) & (1u << 5);
  if (!(fma && movbe && popcnt && osxsave && avx && f16c && bmi1 && avx2 && bmi2 && lzcnt)) return false;
  // The OS saves the AVX registers on a thread switch (XCR0 bits 1 and 2).
  unsigned lo, hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  (void)hi;
  return (lo & 6u) == 6u;
}

void __cdecl RedirectIfOldCpu() {
  if (CpuHasV3()) return;
  wchar_t path[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(sizeof(path) / sizeof(path[0])));
  if (n == 0 || n >= sizeof(path) / sizeof(path[0]) - 32) ExitProcess(1);
  wchar_t* slash = path;
  for (wchar_t* p = path; *p; ++p)
    if (*p == L'\\' || *p == L'/') slash = p + 1;
  lstrcpyW(slash, L"ng2-compat.exe");
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  // (the same command line: its first word still names ng2.exe, which the
  // compatible build does not read - it finds its folder from its own path)
  wchar_t* cmd = GetCommandLineW();
  wchar_t* cmd_copy = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, (lstrlenW(cmd) + 1) * sizeof(wchar_t)));
  if (cmd_copy) lstrcpyW(cmd_copy, cmd);
  if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES &&
      CreateProcessW(path, cmd_copy, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    TerminateProcess(GetCurrentProcess(), 0);  // (nothing of this process has started: safe in a TLS callback)
  }
  MessageBoxW(nullptr,
              L"This PC's processor does not have AVX2, which ng2.exe is built for, and ng2-compat.exe (the "
              L"version for older processors) is not beside it. Copy ng2-compat.exe from the release into "
              L"this folder.",
              L"IzunaRecomp", MB_OK | MB_ICONERROR);
  TerminateProcess(GetCurrentProcess(), 1);
}

// The earliest place: the loader runs an executable's TLS callbacks at process
// start, before its entry point - and the C runtime's own (.CRT$XLC, the
// dynamic thread_local constructors, compiled for v3 like the rest) come after
// this one in .CRT$XLB.
void NTAPI CheckAtProcessStart(PVOID, DWORD reason, PVOID) {
  if (reason == DLL_PROCESS_ATTACH) RedirectIfOldCpu();
}

}  // namespace

#pragma section(".CRT$XLB", long, read)
extern "C" __declspec(allocate(".CRT$XLB")) const PIMAGE_TLS_CALLBACK izuna_tls_cpu_check = CheckAtProcessStart;
#pragma comment(linker, "/include:_tls_used")
#pragma comment(linker, "/include:izuna_tls_cpu_check")

// The C runtime runs the function pointers in .CRT$XCA..XCZ in name order; the
// compiler's own static constructors are in .CRT$XCU.
#pragma section(".CRT$XCB", read)
extern "C" __declspec(allocate(".CRT$XCB")) void(__cdecl* const izuna_cpu_redirect)(void) = RedirectIfOldCpu;
#pragma comment(linker, "/include:izuna_cpu_redirect")

#endif
