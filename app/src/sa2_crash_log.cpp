// Windows: logs fatal host exceptions that the SDK does not report (illegal instruction from a __builtin_trap in the
// generated code, divide by zero, stack overflow, ...) with the module and offset, so the crash can be symbolized
// afterwards: llvm-symbolizer --obj=sa2.exe 0x<0x140000000 + offset>. Access violations are left alone: the SDK
// handles those itself (guest memory, MMIO) and reports the unhandled ones.

#if defined(_WIN32)

#include <windows.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include <rex/logging.h>
#include <rex/logging/api.h>

namespace {

LONG CALLBACK LogFatalException(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  switch (code) {
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case STATUS_STACK_BUFFER_OVERRUN:
      break;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
  const auto pc = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
  HMODULE module = nullptr;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(pc), &module) && module) {
    GetModuleFileNameA(module, path, MAX_PATH);
  }
  const char* name = std::strrchr(path, '\\');
  REXLOG_CRITICAL("[crash] host exception {:08X} at {:016X} ({}+0x{:X}) on thread {}", uint32_t(code), pc,
                  name ? name + 1 : path, pc - reinterpret_cast<uintptr_t>(module), GetCurrentThreadId());
  rex::FlushLogging();
  return EXCEPTION_CONTINUE_SEARCH;
}

// Installed before main, first in the chain; it only logs and never handles.
const PVOID g_handler = AddVectoredExceptionHandler(1, LogFatalException);

// Dunwall (2026-11-08): abort() and unhandled C++ exceptions end the process through the CRT's fast fail
// (c0000409, code 7) without reaching the handler above or the log. Both now log the reason and the host stack as
// module+offset (symbolize as above), then end the process as before.
void LogStack(const char* what) {
  void* frames[32];
  const USHORT n = CaptureStackBackTrace(2, 32, frames, nullptr);
  REXLOG_CRITICAL("[crash] {} on thread {}; host stack ({} frames):", what, GetCurrentThreadId(), n);
  for (USHORT i = 0; i < n; ++i) {
    const auto pc = reinterpret_cast<uintptr_t>(frames[i]);
    HMODULE module = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(pc), &module) && module) {
      GetModuleFileNameA(module, path, MAX_PATH);
    }
    const char* name = std::strrchr(path, '\\');
    REXLOG_CRITICAL("[crash]   #{} {}+0x{:X}", i, name ? name + 1 : path, pc - reinterpret_cast<uintptr_t>(module));
  }
  rex::FlushLogging();
}

void OnAbort(int) { LogStack("abort()"); }

void OnTerminate() {
  const char* reason = "std::terminate (no active exception)";
  std::string message;
  if (const std::exception_ptr e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& x) {
      message = std::string("unhandled exception: ") + x.what();
      reason = message.c_str();
    } catch (...) {
      reason = "unhandled exception of unknown type";
    }
  }
  LogStack(reason);
  std::abort();
}

const auto g_abort_handler = std::signal(SIGABRT, OnAbort);
const auto g_terminate_handler = std::set_terminate(OnTerminate);

}  // namespace

#endif
