// probe.h - frozen contract for the Xbox Dev Mode AppContainer capability probe.
//
// Every probe module includes ONLY this header. A module exposes a single
// register_<X>(Reporter&) function (declared at the bottom) and adds probes by
// calling reporter.run(category, name, body). The body receives a Record& it
// fills in; the Reporter wraps the body in SEH + C++ try/catch so a fault in one
// probe can never abort the run, and flushes the JSON report after every emit so
// a hard crash still leaves a partial report on disk.
//
// This file is the interface boundary between the orchestrator-owned core (JSON
// serialization, SEH trampoline, host integration) and the independently written
// probe modules. Do not add module-specific includes here.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace probe {

// Status of a single probe. Keep in sync with status_name() in reporter.cpp and
// the JSON schema in docs/schema.md.
enum class Status {
    OK,       // capability present / call succeeded
    DENIED,   // blocked by the sandbox (ACCESS_DENIED, E_ACCESSDENIED, etc.)
    NOTIMPL,  // present but not implemented / STATUS_NOT_IMPLEMENTED
    MISSING,  // symbol / DLL / API not found at all
    CRASH,    // body raised an SEH exception or C++ exception (caught by Reporter)
    INFO,     // informational measurement, no pass/fail meaning
    WARN,     // succeeded but with a caveat worth flagging
    SKIPPED,  // deliberately not attempted in this environment
};

const char* status_name(Status s);

// One structured probe result. 'value' is free-form (JSON-escaped on output):
// use it for measured data (CPUID words, feature-level strings, path, size...).
struct Record {
    std::string category;   // "A".."N" (see the task spec)
    std::string name;       // short stable identifier, unique within category
    Status      status = Status::INFO;
    std::string detail;     // human-readable explanation
    uint32_t    errorcode = 0;  // GetLastError / NTSTATUS / HRESULT as relevant
    std::string value;      // measured value, free-form
};

// Environment metadata captured once per run and emitted at the top of the JSON.
struct EnvInfo {
    std::string host;        // "win32" | "uwp"
    std::string environment; // "baseline-win32" | "laptop-uwp" | "xbox-uwp" | ...
    std::string device;      // machine / console name
    std::string os_build;    // e.g. "10.0.26100"
    std::string arch;        // "x64" | "arm64" | ...
    std::string note;        // free-form (set via PROBE_ENV env var or UI)
    std::string started_utc; // ISO-8601
};

// Reporter: thread-safe sink that every probe writes through.
//
//  * run(category, name, body) is the ONLY entry point a module should use.
//    It constructs a Record, runs 'body' under SEH + try/catch, and emits the
//    result. If body faults, the Record is marked CRASH with the exception code;
//    body is free to set status/detail/value/errorcode before (or instead of)
//    faulting.
//  * After each emit the full report is rewritten to the JSON path (if set), so
//    an unrecoverable crash in a later probe never loses earlier results.
//  * on_emit lets a host mirror records to screen / network as they arrive.
class Reporter {
public:
    Reporter();
    ~Reporter();

    void set_env(const EnvInfo& env);
    const EnvInfo& env() const { return env_; }

    // Where to incrementally flush the JSON report. Empty = no file flush.
    void set_json_path(const std::wstring& path);

    // Optional live callback invoked (under lock) for every emitted record.
    void set_on_emit(std::function<void(const Record&)> cb);

    // Run a probe body safely and emit its Record. Never throws.
    void run(const std::string& category,
             const std::string& name,
             const std::function<void(Record&)>& body);

    // Emit an already-filled record directly (no SEH wrap). Rarely needed.
    void emit(const Record& r);

    // Snapshot of everything collected so far.
    std::vector<Record> records() const;

    // Serialize the whole run (env + records) to a JSON string.
    std::string to_json() const;

    // Force a flush of the JSON file now.
    void flush() const;

private:
    void flush_locked() const;

    mutable std::mutex           mu_;
    EnvInfo                      env_;
    std::vector<Record>          records_;
    std::wstring                 json_path_;
    std::function<void(const Record&)> on_emit_;
};

// --- helpers shared across modules (implemented in reporter.cpp) ------------

// Resolve an export from an already-loaded module (e.g. "ntdll.dll").
// Returns nullptr if the module or symbol is absent.
void* resolve(const char* module, const char* symbol);

// Human-readable name for a Win32 error / NTSTATUS, best-effort.
std::string error_text(uint32_t code);

// UTF-16 <-> UTF-8 for building detail/value strings and paths.
std::string narrow(const std::wstring& ws);
std::wstring widen(const std::string& s);

// JSON-escape a UTF-8 string (no surrounding quotes).
std::string json_escape(const std::string& s);

// Low-level SEH trampoline (seh.cpp, compiled /EHa). Runs fn(ctx) under __try;
// on an SEH exception returns false and writes the exception code to *code.
// Used internally by Reporter::run. Exposed for probes that must distinguish an
// expected fault (e.g. the privileged-instruction #GP test) from success.
bool run_guarded(void (*fn)(void*), void* ctx, uint32_t* code);

// Convenience wrapper: run a capturing lambda under SEH, returning false + code
// on fault. Safe because the std::function lives in the caller frame and only a
// non-unwinding trampoline crosses into the __try.
template <class F>
bool guarded(F&& f, uint32_t* code) {
    std::function<void()> fn(std::forward<F>(f));
    return run_guarded(
        [](void* p) { (*static_cast<std::function<void()>*>(p))(); },
        &fn, code);
}

// --- module registration (orchestrator owns the dispatch table) -------------
// Each module implements exactly one of these in its own translation unit.
// Adding a module = add its .cpp + a line in core_run.cpp's table.
void register_A_dynamic_code(Reporter&);   // Opus: dynamic code exec, SEC_IMAGE, VA2
void register_B_dll_inventory(Reporter&);  // Haiku: System32 + export sweep
void register_B_ntdll_stubs(Reporter&);    // Opus: raw Nt* stub bytes
void register_C_syscalls(Reporter&);       // Haiku: NT syscall resolve + safe call
void register_D_veh_trap(Reporter&);       // Opus: VEH, priv insn, Get/SetThreadContext
void register_E_cpu_identity(Reporter&);   // Opus: CPUID / RDTSC
void register_F_filesystem(Reporter&);     // Sonnet: path read/write matrix
void register_G_registry(Reporter&);       // Sonnet: hive read/write
void register_H_token(Reporter&);          // Sonnet: token / AppContainer SIDs
void register_I_graphics(Reporter&);       // Sonnet: D3D11/12 + bridges
void register_J_audio(Reporter&);          // Sonnet: WASAPI / XAudio2
void register_K_network(Reporter&);        // Sonnet: Winsock connect/listen
void register_L_processes(Reporter&);      // Opus: multi-instance / CreateProcess
void register_M_system(Reporter&);         // Sonnet: memory / procfeatures / firmware
void register_N_winrt(Reporter&);          // Haiku: WinRT activation sweep

// Runs every registered module in order against the Reporter. Defined in
// core_run.cpp. Modules not yet delivered are stubbed so the tree always builds.
void run_all(Reporter&);

// Capture environment metadata for this run. host is "win32" or "uwp".
// Honors PROBE_ENVIRONMENT and PROBE_NOTE environment variables. (env.cpp)
EnvInfo capture_env(const std::string& host);

// Best-effort POST of a JSON report to url ("http(s)://host:port/path").
// Never throws; prints a one-line result. (http.cpp)
void http_post_report(const std::string& url, const std::string& json);

}  // namespace probe
