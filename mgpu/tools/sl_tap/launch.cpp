// Starts a game suspended and loads a DLL through an early APC on its main thread; --attach injects into a running process instead.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>

static DWORD find_pid(const wchar_t* name, DWORD exclude) {
  DWORD pid = 0;
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  PROCESSENTRY32W e{sizeof e};
  for (BOOL ok = Process32FirstW(s, &e); ok; ok = Process32NextW(s, &e))
    if (!_wcsicmp(e.szExeFile, name) && e.th32ProcessID != exclude) pid = e.th32ProcessID;
  CloseHandle(s);
  return pid;
}

static void* write_path(HANDLE p, const std::wstring& dll) {
  SIZE_T n = (dll.size() + 1) * sizeof(wchar_t);
  void* mem = VirtualAllocEx(p, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (mem) WriteProcessMemory(p, mem, dll.c_str(), n, nullptr);
  return mem;
}

static auto load_library() { return (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"); }

static int attach(DWORD pid, const std::wstring& dll) {
  HANDLE p = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
  if (!p) return printf("OpenProcess %lu failed %lu\n", pid, GetLastError()), 1;
  HANDLE t = CreateRemoteThread(p, nullptr, 0, load_library(), write_path(p, dll), 0, nullptr);
  if (!t) return printf("CreateRemoteThread failed %lu\n", GetLastError()), 1;
  WaitForSingleObject(t, 10000);
  DWORD code = 0;
  GetExitCodeThread(t, &code);
  printf("attached to %lu, LoadLibraryW -> 0x%lx\n", pid, code);
  return code ? 0 : 1;
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    printf("usage: sl_tap_launch <dll> <exe> [--attach] [--fallback-sec N] [--env K=V]...\n");
    return 2;
  }
  std::wstring dll = argv[1], exe = argv[2];
  bool attach_only = false;
  int fallback = 30;
  for (int i = 3; i < argc; ++i) {
    if (!wcscmp(argv[i], L"--attach")) attach_only = true;
    else if (!wcscmp(argv[i], L"--fallback-sec") && i + 1 < argc) fallback = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--env") && i + 1 < argc) {
      std::wstring kv = argv[++i];
      size_t eq = kv.find(L'=');
      if (eq != std::wstring::npos) SetEnvironmentVariableW(kv.substr(0, eq).c_str(), kv.c_str() + eq + 1);
    }
  }
  std::wstring name = exe.substr(exe.find_last_of(L"\\/") + 1);
  if (attach_only) {
    DWORD pid = 0;
    for (int i = 0; i < fallback * 10 && !pid; ++i, Sleep(100)) pid = find_pid(name.c_str(), 0);
    return pid ? attach(pid, dll) : (printf("no %ls\n", name.c_str()), 1);
  }
  std::wstring dir = exe.substr(0, exe.find_last_of(L"\\/"));
  std::wstring cmd = L"\"" + exe + L"\"";
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi))
    return printf("CreateProcess failed %lu\n", GetLastError()), 1;
  QueueUserAPC((PAPCFUNC)load_library(), pi.hThread, (ULONG_PTR)write_path(pi.hProcess, dll));
  ResumeThread(pi.hThread);
  printf("started %lu suspended, APC queued, resumed\n", pi.dwProcessId);
  // A Steam relaunch replaces the process; inject late into the replacement.
  if (WaitForSingleObject(pi.hProcess, fallback * 1000) == WAIT_OBJECT_0) {
    DWORD code;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("process %lu exited early (0x%lx); waiting for a relaunch\n", pi.dwProcessId, code);
    DWORD pid = 0;
    for (int i = 0; i < 600 && !pid; ++i, Sleep(100)) pid = find_pid(name.c_str(), pi.dwProcessId);
    if (!pid) return printf("no relaunch seen\n"), 1;
    Sleep(500);
    return attach(pid, dll);
  }
  printf("process %lu still running after %d s\n", pi.dwProcessId, fallback);
  return 0;
}
