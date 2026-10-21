// probe_G_registry.cpp - category G: registry hive access matrix.
//
// Read probes (KEY_READ):
//   read:HKLM-SOFTWARE   HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion, value ProductName
//   read:HKLM-SYSTEM     HKLM\SYSTEM\CurrentControlSet\Control, subkey/value counts
//   read:HKU-DEFAULT     HKEY_USERS\.DEFAULT
//   read:HKCU-Software   HKCU\Software
//   enum:HKLM-SOFTWARE   RegEnumKeyExW the first 3 subkeys of HKLM\SOFTWARE
// Write probes (private temp key __probe_tmp_<pid>_<tick>, always removed):
//   write:HKCU  create key, set DWORD, read back, delete value, delete key
//   write:HKLM  create key (expected DENIED in a sandbox); if it succeeds, delete it
//
// Temp keys are created REG_OPTION_VOLATILE so that even a hard crash cannot leave
// them behind across a reboot, and an RAII guard deletes them on every exit path.
//
// Link deps: advapi32 only (already provided by the hosts).
// Sonnet
#include "probe.h"

#include <string>

namespace probe {
namespace {

Status classify(LONG e) {
    switch (e) {
        case ERROR_SUCCESS:
            return Status::OK;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
            return Status::DENIED;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return Status::MISSING;
        case ERROR_NOT_SUPPORTED:
        case ERROR_CALL_NOT_IMPLEMENTED:
            return Status::NOTIMPL;
        default:
            return Status::INFO;
    }
}

void fail(Record& rec, LONG e, const char* what, const std::string& target) {
    rec.status = classify(e);
    rec.errorcode = static_cast<uint32_t>(e);
    rec.detail = std::string(what) + " failed on " + target + ": " +
                 error_text(static_cast<uint32_t>(e));
}

// RAII registry handle.
struct Key {
    HKEY h = nullptr;
    ~Key() { close(); }
    void close() {
        if (h) {
            RegCloseKey(h);
            h = nullptr;
        }
    }
};

// Read a REG_SZ / REG_EXPAND_SZ value into *out.
LONG query_string(HKEY k, const wchar_t* name, std::wstring* out) {
    DWORD type = 0;
    DWORD cb = 0;
    LONG s = RegQueryValueExW(k, name, nullptr, &type, nullptr, &cb);
    if (s != ERROR_SUCCESS) return s;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return ERROR_UNSUPPORTED_TYPE;
    for (int i = 0; i < 3; ++i) {  // value may grow between the two calls
        std::wstring buf(cb / sizeof(wchar_t) + 2, L'\0');
        DWORD cb2 = static_cast<DWORD>((buf.size() - 1) * sizeof(wchar_t));
        s = RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<LPBYTE>(&buf[0]), &cb2);
        if (s == ERROR_MORE_DATA) {
            cb = cb2;
            continue;
        }
        if (s != ERROR_SUCCESS) return s;
        buf.resize(wcslen(buf.c_str()));  // trim at first NUL
        *out = buf;
        return ERROR_SUCCESS;
    }
    return ERROR_MORE_DATA;
}

// Open root\sub with KEY_READ and report subkey/value counts.
void read_key_counts(Record& rec, HKEY root, const wchar_t* sub, const char* display) {
    Key k;
    LONG s = RegOpenKeyExW(root, sub, 0, KEY_READ, &k.h);
    if (s != ERROR_SUCCESS) {
        fail(rec, s, "RegOpenKeyExW(KEY_READ)", display);
        return;
    }
    DWORD subkeys = 0;
    DWORD values = 0;
    s = RegQueryInfoKeyW(k.h, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values,
                         nullptr, nullptr, nullptr, nullptr);
    rec.detail = std::string("opened ") + display;
    if (s != ERROR_SUCCESS) {
        rec.status = Status::WARN;
        rec.errorcode = static_cast<uint32_t>(s);
        rec.value = "opened=1;queryinfo=failed";
        rec.detail += "; RegQueryInfoKeyW failed: " + error_text(static_cast<uint32_t>(s));
        return;
    }
    rec.status = Status::OK;
    rec.value = "opened=1;subkeys=" + std::to_string(subkeys) + ";values=" + std::to_string(values);
}

// ---- write probes ---------------------------------------------------------------------

// RAII owner of the private temp key: closes the handle and deletes the key on
// every exit path. 'armed' is set before the create call so even an ambiguous
// failure is cleaned up (deleting a key that does not exist is harmless).
struct TempKey {
    HKEY root;
    std::wstring sub;
    Key key;
    bool armed = false;

    TempKey(HKEY r, std::wstring s) : root(r), sub(std::move(s)) {}
    ~TempKey() {
        LONG ignored = 0;
        remove(&ignored);
    }

    // Close + delete. True when the key is gone afterwards.
    bool remove(LONG* err) {
        key.close();
        if (!armed) return true;
        LONG s = RegDeleteKeyExW(root, sub.c_str(), 0, 0);
        if (s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND || s == ERROR_PATH_NOT_FOUND) {
            armed = false;
            return true;
        }
        *err = s;
        return false;
    }

    // Verify the key no longer opens.
    bool verify_gone() const {
        HKEY h = nullptr;
        LONG s = RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &h);
        if (s == ERROR_SUCCESS) {
            RegCloseKey(h);
            return false;
        }
        return s == ERROR_FILE_NOT_FOUND || s == ERROR_PATH_NOT_FOUND;
    }
};

std::wstring make_temp_subkey(const wchar_t* parent) {
    return std::wstring(parent) + L"\\__probe_tmp_" + std::to_wstring(GetCurrentProcessId()) +
           L"_" + std::to_wstring(static_cast<unsigned long long>(GetTickCount64()));
}

// Create root\parent\__probe_tmp_*. When 'full', also set/read back/delete a DWORD
// value; either way the key is deleted and its removal verified.
void write_probe(Record& rec, HKEY root, const wchar_t* parent, const char* root_name,
                 bool full) {
    TempKey tk(root, make_temp_subkey(parent));
    const std::string display = std::string(root_name) + "\\" + narrow(tk.sub);

    tk.armed = true;
    DWORD disposition = 0;
    LONG s = RegCreateKeyExW(root, tk.sub.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
                             KEY_READ | KEY_WRITE | DELETE, nullptr, &tk.key.h, &disposition);
    if (s != ERROR_SUCCESS) {
        fail(rec, s, "RegCreateKeyExW", display);
        return;
    }

    std::string stage_what;
    LONG stage_err = 0;
    bool set_ok = false;
    bool readback_ok = false;
    bool valdel_ok = false;

    if (full) {
        const DWORD kData = 0x50524F42;  // "PROB"
        s = RegSetValueExW(tk.key.h, L"ProbeValue", 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&kData), sizeof kData);
        if (s != ERROR_SUCCESS) {
            stage_what = "RegSetValueExW";
            stage_err = s;
        } else {
            set_ok = true;
            DWORD type = 0;
            DWORD back = 0;
            DWORD cb = sizeof back;
            s = RegQueryValueExW(tk.key.h, L"ProbeValue", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(&back), &cb);
            if (s != ERROR_SUCCESS) {
                stage_what = "RegQueryValueExW(readback)";
                stage_err = s;
            } else {
                readback_ok = type == REG_DWORD && cb == sizeof back && back == kData;
            }
            s = RegDeleteValueW(tk.key.h, L"ProbeValue");
            valdel_ok = s == ERROR_SUCCESS;
            if (!valdel_ok && stage_what.empty()) {
                stage_what = "RegDeleteValueW";
                stage_err = s;
            }
        }
    }

    LONG del_err = 0;
    const bool deleted = tk.remove(&del_err) && tk.verify_gone();

    std::string value = "created=1";
    if (full) {
        value += std::string(";set=") + (set_ok ? "1" : "0") + ";readback=" +
                 (readback_ok ? "1" : "0") + ";valdel=" + (valdel_ok ? "1" : "0");
    }
    value += std::string(";keydel=") + (deleted ? "1" : "0");
    rec.value = value;

    if (!stage_what.empty()) {
        rec.status = classify(stage_err);
        rec.errorcode = static_cast<uint32_t>(stage_err);
        rec.detail = "created " + display + " but " + stage_what + " failed: " +
                     error_text(static_cast<uint32_t>(stage_err));
    } else if (full && !readback_ok) {
        rec.status = Status::WARN;
        rec.detail = "created and set " + display + " but readback did not match";
    } else {
        rec.status = Status::OK;
        rec.detail = full ? "create+set+readback+delete value+delete key at " + display
                          : "create SUCCEEDED (hive is writable), then deleted " + display;
    }
    if (!deleted) {
        rec.status = Status::WARN;
        rec.errorcode = static_cast<uint32_t>(del_err);
        rec.detail += "; CLEANUP FAILED, temp key may remain: " + display + " (" +
                      error_text(static_cast<uint32_t>(del_err)) + ")";
    }
}

}  // namespace

void register_G_registry(Reporter& r) {
    // ---- reads -----------------------------------------------------------------
    r.run("G", "read:HKLM-SOFTWARE", [](Record& rec) {
        const char* display = "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
        Key k;
        LONG s = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                               L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ,
                               &k.h);
        if (s != ERROR_SUCCESS) {
            fail(rec, s, "RegOpenKeyExW(KEY_READ)", display);
            return;
        }
        std::wstring product;
        s = query_string(k.h, L"ProductName", &product);
        if (s != ERROR_SUCCESS) {
            fail(rec, s, "RegQueryValueExW(ProductName)", display);
            return;
        }
        rec.status = Status::OK;
        rec.value = narrow(product);
        rec.detail = std::string("read ProductName from ") + display;
    });

    r.run("G", "read:HKLM-SYSTEM", [](Record& rec) {
        read_key_counts(rec, HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control",
                        "HKLM\\SYSTEM\\CurrentControlSet\\Control");
    });

    r.run("G", "read:HKU-DEFAULT", [](Record& rec) {
        read_key_counts(rec, HKEY_USERS, L".DEFAULT", "HKEY_USERS\\.DEFAULT");
    });

    r.run("G", "read:HKCU-Software", [](Record& rec) {
        read_key_counts(rec, HKEY_CURRENT_USER, L"Software", "HKCU\\Software");
    });

    r.run("G", "enum:HKLM-SOFTWARE", [](Record& rec) {
        const char* display = "HKLM\\SOFTWARE";
        Key k;
        LONG s = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, KEY_READ, &k.h);
        if (s != ERROR_SUCCESS) {
            fail(rec, s, "RegOpenKeyExW(KEY_READ)", display);
            return;
        }
        unsigned count = 0;
        std::string names;
        LONG stop = ERROR_NO_MORE_ITEMS;
        for (DWORD i = 0; i < 3; ++i) {
            wchar_t name[256];
            DWORD cch = 256;
            s = RegEnumKeyExW(k.h, i, name, &cch, nullptr, nullptr, nullptr, nullptr);
            if (s != ERROR_SUCCESS) {
                stop = s;
                break;
            }
            if (!names.empty()) names += ",";
            names += narrow(std::wstring(name, cch));
            ++count;
        }
        rec.value = "count=" + std::to_string(count) + ";names=" + names;
        rec.detail = std::string("RegEnumKeyExW first 3 subkeys of ") + display;
        if (stop != ERROR_NO_MORE_ITEMS && stop != ERROR_SUCCESS) {
            // Enumeration itself failed (e.g. access denied after a successful open).
            rec.errorcode = static_cast<uint32_t>(stop);
            rec.status = count ? Status::WARN : classify(stop);
            rec.detail += "; stopped: " + error_text(static_cast<uint32_t>(stop));
        } else if (count == 0) {
            rec.status = Status::WARN;
            rec.detail += "; opened OK but no subkeys enumerated";
        } else {
            rec.status = Status::OK;
        }
    });

    // ---- writes (private temp key, always removed) --------------------------------
    r.run("G", "write:HKCU", [](Record& rec) {
        write_probe(rec, HKEY_CURRENT_USER, L"Software", "HKCU", /*full=*/true);
    });

    r.run("G", "write:HKLM", [](Record& rec) {
        write_probe(rec, HKEY_LOCAL_MACHINE, L"SOFTWARE", "HKLM", /*full=*/false);
    });
}

}  // namespace probe
