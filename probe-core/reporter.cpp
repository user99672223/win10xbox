// reporter.cpp - Reporter implementation, JSON serialization, shared helpers.
#include "probe.h"

#include <cstdio>
#include <exception>

namespace probe {

const char* status_name(Status s) {
    switch (s) {
        case Status::OK:      return "OK";
        case Status::DENIED:  return "DENIED";
        case Status::NOTIMPL: return "NOTIMPL";
        case Status::MISSING: return "MISSING";
        case Status::CRASH:   return "CRASH";
        case Status::INFO:    return "INFO";
        case Status::WARN:    return "WARN";
        case Status::SKIPPED: return "SKIPPED";
    }
    return "UNKNOWN";
}

std::string narrow(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

void* resolve(const char* module, const char* symbol) {
    HMODULE h = GetModuleHandleA(module);
    if (!h) h = LoadLibraryA(module);
    if (!h) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(h, symbol));
}

std::string error_text(uint32_t code) {
    // Try as a Win32 / HRESULT message first.
    LPWSTR buf = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, (LPWSTR)&buf, 0, nullptr);
    std::string out;
    if (n && buf) {
        std::wstring w(buf, n);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' ||
                              w.back() == L'.' || w.back() == L' '))
            w.pop_back();
        out = narrow(w);
    }
    if (buf) LocalFree(buf);
    // Also try as an NTSTATUS via ntdll's message table.
    if (out.empty()) {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (nt) {
            buf = nullptr;
            n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                   FORMAT_MESSAGE_FROM_HMODULE |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nt, code, 0, (LPWSTR)&buf, 0, nullptr);
            if (n && buf) {
                std::wstring w(buf, n);
                while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' ||
                                      w.back() == L'.' || w.back() == L' '))
                    w.pop_back();
                out = narrow(w);
            }
            if (buf) LocalFree(buf);
        }
    }
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08X", code);
    if (out.empty()) return hex;
    return out + " (" + hex + ")";
}

Reporter::Reporter() = default;
Reporter::~Reporter() = default;

void Reporter::set_env(const EnvInfo& env) {
    std::lock_guard<std::mutex> lk(mu_);
    env_ = env;
}

void Reporter::set_json_path(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(mu_);
    json_path_ = path;
}

void Reporter::set_on_emit(std::function<void(const Record&)> cb) {
    std::lock_guard<std::mutex> lk(mu_);
    on_emit_ = std::move(cb);
}

void Reporter::emit(const Record& r) {
    std::lock_guard<std::mutex> lk(mu_);
    records_.push_back(r);
    if (on_emit_) {
        // Never let a host callback take down the run.
        try { on_emit_(records_.back()); } catch (...) {}
    }
    flush_locked();
}

namespace {
// Context handed to the SEH trampoline for Reporter::run.
struct RunCtx {
    const std::function<void(Record&)>* body;
    Record* rec;
    bool cpp_exception;
    std::string cpp_what;
};
void run_trampoline(void* p) {
    auto* c = static_cast<RunCtx*>(p);
    try {
        (*c->body)(*c->rec);
    } catch (const std::exception& e) {
        c->cpp_exception = true;
        c->cpp_what = e.what();
    } catch (...) {
        c->cpp_exception = true;
        c->cpp_what = "unknown C++ exception";
    }
}
}  // namespace

void Reporter::run(const std::string& category, const std::string& name,
                   const std::function<void(Record&)>& body) {
    Record r;
    r.category = category;
    r.name = name;
    r.status = Status::INFO;

    RunCtx ctx{&body, &r, false, {}};
    uint32_t code = 0;
    bool ok = run_guarded(run_trampoline, &ctx, &code);
    if (!ok) {
        r.status = Status::CRASH;
        r.errorcode = code;
        std::string why = "SEH exception " + error_text(code);
        r.detail = r.detail.empty() ? why : (r.detail + " | " + why);
    } else if (ctx.cpp_exception) {
        r.status = Status::CRASH;
        std::string why = "C++ exception: " + ctx.cpp_what;
        r.detail = r.detail.empty() ? why : (r.detail + " | " + why);
    }
    emit(r);
}

std::vector<Record> Reporter::records() const {
    std::lock_guard<std::mutex> lk(mu_);
    return records_;
}

std::string Reporter::to_json() const {
    // caller may or may not hold lock; this variant locks.
    std::lock_guard<std::mutex> lk(mu_);
    std::string o;
    o.reserve(records_.size() * 128 + 512);
    auto kv = [&](const char* k, const std::string& v, bool comma = true) {
        o += "\""; o += k; o += "\":\""; o += json_escape(v); o += "\"";
        if (comma) o += ",";
    };
    o += "{\n  \"schema\": \"xbox-probe/1\",\n  \"environment\": {";
    kv("host", env_.host);
    kv("environment", env_.environment);
    kv("device", env_.device);
    kv("os_build", env_.os_build);
    kv("arch", env_.arch);
    kv("note", env_.note);
    kv("started_utc", env_.started_utc, false);
    o += "},\n  \"records\": [\n";
    for (size_t i = 0; i < records_.size(); ++i) {
        const Record& r = records_[i];
        o += "    {";
        o += "\"category\":\"" + json_escape(r.category) + "\",";
        o += "\"name\":\"" + json_escape(r.name) + "\",";
        o += "\"status\":\"" + std::string(status_name(r.status)) + "\",";
        o += "\"errorcode\":" + std::to_string(r.errorcode) + ",";
        o += "\"detail\":\"" + json_escape(r.detail) + "\",";
        o += "\"value\":\"" + json_escape(r.value) + "\"";
        o += (i + 1 < records_.size()) ? "},\n" : "}\n";
    }
    o += "  ]\n}\n";
    return o;
}

void Reporter::flush() const {
    std::lock_guard<std::mutex> lk(mu_);
    flush_locked();
}

void Reporter::flush_locked() const {
    if (json_path_.empty()) return;
    // Build JSON without re-locking (we already hold mu_).
    std::string o;
    o.reserve(records_.size() * 128 + 512);
    o += "{\n  \"schema\": \"xbox-probe/1\",\n  \"environment\": {";
    auto kv = [&](const char* k, const std::string& v, bool comma = true) {
        o += "\""; o += k; o += "\":\""; o += json_escape(v); o += "\"";
        if (comma) o += ",";
    };
    kv("host", env_.host);
    kv("environment", env_.environment);
    kv("device", env_.device);
    kv("os_build", env_.os_build);
    kv("arch", env_.arch);
    kv("note", env_.note);
    kv("started_utc", env_.started_utc, false);
    o += "},\n  \"records\": [\n";
    for (size_t i = 0; i < records_.size(); ++i) {
        const Record& r = records_[i];
        o += "    {";
        o += "\"category\":\"" + json_escape(r.category) + "\",";
        o += "\"name\":\"" + json_escape(r.name) + "\",";
        o += "\"status\":\"" + std::string(status_name(r.status)) + "\",";
        o += "\"errorcode\":" + std::to_string(r.errorcode) + ",";
        o += "\"detail\":\"" + json_escape(r.detail) + "\",";
        o += "\"value\":\"" + json_escape(r.value) + "\"";
        o += (i + 1 < records_.size()) ? "},\n" : "}\n";
    }
    o += "  ]\n}\n";

    // Atomic-ish: write to temp then move over the target.
    std::wstring tmp = json_path_ + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, o.data(), (DWORD)o.size(), &wrote, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    MoveFileExW(tmp.c_str(), json_path_.c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace probe
