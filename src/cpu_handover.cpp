// ng2 - Ninja Gaiden II static recompilation
//
// The packages carry the game twice: built for any x86-64 PC since about
// 2009 (x86-64-v2: SSE4.2), and for AVX2 machines (x86-64-v3: AVX2, FMA, BMI;
// the Steam Deck, any Ryzen, Intel since Haswell). The first is started; on a
// CPU with all of x86-64-v3 it hands over at once, before anything else runs,
// to the second next to it (ng2-avx2, ng2-avx2.exe), with the same
// arguments and environment. NG2_NO_AVX2=1 keeps the first.

#include <cstdlib>

#if !defined(__AVX2__) && (defined(__x86_64__) || defined(_M_X64))

#include <cstdint>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <intrin.h>
#include <windows.h>
#else
#include <cpuid.h>
#include <unistd.h>

#include <fstream>
#include <iterator>
#include <vector>
#endif

namespace {

void Cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
#if defined(_WIN32)
  int out[4];
  __cpuidex(out, int(leaf), int(sub));
  for (int i = 0; i < 4; i++) {
    r[i] = uint32_t(out[i]);
  }
#else
  __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
#endif
}

// All of x86-64-v3, and the OS saving the AVX registers.
bool HasX86_64V3() {
  uint32_t r[4];
  Cpuid(0, 0, r);
  uint32_t maxLeaf = r[0];
  if (maxLeaf < 7) {
    return false;
  }
  Cpuid(1, 0, r);
  uint32_t ecx1 = r[2];
  bool fma = ecx1 & (1u << 12), movbe = ecx1 & (1u << 22), osxsave = ecx1 & (1u << 27),
       avx = ecx1 & (1u << 28), f16c = ecx1 & (1u << 29);
  Cpuid(7, 0, r);
  uint32_t ebx7 = r[1];
  bool bmi1 = ebx7 & (1u << 3), avx2 = ebx7 & (1u << 5), bmi2 = ebx7 & (1u << 8);
  Cpuid(0x80000000u, 0, r);
  bool lzcnt = false;
  if (r[0] >= 0x80000001u) {
    Cpuid(0x80000001u, 0, r);
    lzcnt = r[2] & (1u << 5);
  }
  if (!(fma && movbe && osxsave && avx && f16c && bmi1 && avx2 && bmi2 && lzcnt)) {
    return false;
  }
  uint32_t lo, hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));  // XCR0
  (void)hi;
  return (lo & 6) == 6;  // XMM and YMM state
}

struct Handover {
  Handover() {
    const char* off = std::getenv("NG2_NO_AVX2");
    if ((off && *off && *off != '0') || !HasX86_64V3()) {
      return;
    }
#if defined(_WIN32)
    wchar_t self[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) {
      return;
    }
    std::filesystem::path other = std::filesystem::path(self).parent_path() / L"ng2-avx2.exe";
    std::error_code ec;
    if (std::filesystem::path(self).filename() == other.filename() ||
        !std::filesystem::exists(other, ec)) {
      return;
    }
    SetEnvironmentVariableW(L"NG2_NO_AVX2", L"1");  // (the other never hands over again)
    // The command line as given, its first word (the program) replaced.
    std::wstring line = GetCommandLineW();
    size_t rest = 0;
    if (!line.empty() && line[0] == L'"') {
      rest = line.find(L'"', 1);
      rest = rest == std::wstring::npos ? line.size() : rest + 1;
    } else {
      rest = line.find(L' ');
      rest = rest == std::wstring::npos ? line.size() : rest;
    }
    std::wstring command = L"\"" + other.wstring() + L"\"" + line.substr(rest);
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    // In a job closed with this process: ended (Task Manager, a launcher), so is the other.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    if (!CreateProcessW(other.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED,
                        nullptr, nullptr, &startup, &process)) {
      SetEnvironmentVariableW(L"NG2_NO_AVX2", nullptr);
      return;
    }
    if (job) {
      AssignProcessToJobObject(job, process.hProcess);
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    ExitProcess(code);
#else
    std::error_code ec;
    std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
      return;
    }
    std::filesystem::path other = self.parent_path() / "ng2-avx2";
    if (self.filename() == other.filename() || access(other.c_str(), X_OK) != 0) {
      return;
    }
    setenv("NG2_NO_AVX2", "1", 1);  // (the other never hands over again)
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<char*> argv;
    std::string program = other.string();
    argv.push_back(program.data());
    bool first = true;
    for (size_t at = 0; at < all.size();) {
      size_t end = all.find('\0', at);
      if (end == std::string::npos) {
        end = all.size();
      }
      if (!first) {
        argv.push_back(all.data() + at);
      }
      first = false;
      at = end + 1;
    }
    argv.push_back(nullptr);
    execv(program.c_str(), argv.data());
    unsetenv("NG2_NO_AVX2");  // (execv failed: the first build carries on)
#endif
  }
};

Handover g_handover;

}  // namespace

#endif
