#include "guest_sidecar.h"

#include "launch_config.h"   // IsLeafNameUtf8

namespace w10x_uwp
{
    namespace
    {
        // Calls fn(line) for every line, without the line ending, skipping blank and '#' lines.
        template <class Fn>
        bool ForEachLine(std::string_view text, Fn fn)
        {
            std::size_t pos = 0;
            bool first = true;
            while (pos < text.size())
            {
                std::size_t nl = text.find('\n', pos);
                if (nl == std::string_view::npos) nl = text.size();
                std::string_view line = text.substr(pos, nl - pos);
                pos = nl + 1;
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                if (first && line.size() >= 3 && line.substr(0, 3) == "\xEF\xBB\xBF") line.remove_prefix(3);
                first = false;
                if (line.empty() || line[0] == '#') continue;
                if (!fn(line)) return false;
            }
            return true;
        }

        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
            return s;
        }

        bool SplitKeyValue(std::string_view line, std::string* key, std::string* value)
        {
            const std::size_t eq = line.find('=');
            if (eq == std::string_view::npos) return false;
            const std::string_view k = Trim(line.substr(0, eq));
            if (k.empty()) return false;
            *key = std::string(k);
            *value = std::string(line.substr(eq + 1));
            return true;
        }
    }  // namespace

    void ParseArgsFile(std::string_view text, std::vector<std::string>* out)
    {
        out->clear();
        ForEachLine(text, [&](std::string_view line) {
            out->emplace_back(line);
            return true;
        });
    }

    bool ParseEnvFile(std::string_view text, std::vector<std::pair<std::string, std::string>>* out, std::string* error)
    {
        out->clear();
        return ForEachLine(text, [&](std::string_view line) {
            std::string k, v;
            if (!SplitKeyValue(line, &k, &v))
            {
                *error = "not KEY=VALUE: " + std::string(line);
                return false;
            }
            out->emplace_back(std::move(k), std::move(v));
            return true;
        });
    }

    std::string ParseTitleFile(std::string_view text)
    {
        std::string title;
        ForEachLine(text, [&](std::string_view line) {
            title = std::string(Trim(line));
            return title.empty();   // keep going past a line that is only blanks
        });
        return title;
    }

    bool IsMachineFolderName(std::string_view name)
    {
        return IsLeafNameUtf8(name) && name[0] != '.';
    }

    std::string ParseMachineFile(std::string_view text, bool* ok)
    {
        std::string name;
        *ok = true;
        ForEachLine(text, [&](std::string_view line) {
            name = std::string(Trim(line));
            return false;   // the first non-blank line only
        });
        if (!name.empty() && !IsMachineFolderName(name)) *ok = false;
        return *ok ? name : std::string();
    }

    const std::vector<std::string>& D3dMachineNativeDlls()
    {
        // prepare_machine.cmake: _d3d_native plus machine/guest_dlls_d3d.txt (D3D_EXTRA_DIR).
        static const std::vector<std::string> k = {
            "user32.dll", "win32u.dll", "gdi32.dll", "gdi32full.dll", "msvcp_win.dll", "imm32.dll",
            "d3d11.dll", "dxgi.dll", "d3dcompiler_47.dll", "d3d10warp.dll", "dxcore.dll", "dwmapi.dll",
            "d3d12.dll", "D3D12Core.dll", "D3DSCache.dll", "powrprof.dll", "umpdc.dll",
            "directxdatabasehelper.dll", "kernel.appcore.dll"};
        return k;
    }

    std::vector<std::string> SplitArgLine(std::string_view line)
    {
        std::vector<std::string> words;
        std::string cur;
        bool have = false, quoted = false;
        for (const char ch : line)
        {
            if (ch == '"') { quoted = !quoted; have = true; continue; }
            if (!quoted && (ch == ' ' || ch == '\t'))
            {
                if (have) words.push_back(cur);
                cur.clear();
                have = false;
                continue;
            }
            cur.push_back(ch);
            have = true;
        }
        if (have) words.push_back(cur);
        return words;
    }

    bool NormalizeRelativePath(std::string_view in, bool allow_dot, std::string* out)
    {
        in = Trim(in);
        if (allow_dot && in == ".")
        {
            *out = ".";
            return true;
        }
        std::string r;
        std::size_t pos = 0;
        while (true)
        {
            std::size_t e = in.find_first_of("\\/", pos);
            const std::string_view comp = in.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos);
            if (!IsLeafNameUtf8(comp)) return false;
            if (!r.empty()) r.push_back('\\');
            r.append(comp);
            if (e == std::string_view::npos) break;
            pos = e + 1;
        }
        *out = std::move(r);
        return true;
    }

    bool ParseLaunchFile(std::string_view text, GuestLaunch* out, std::string* error)
    {
        *out = GuestLaunch();
        const bool ok = ForEachLine(text, [&](std::string_view line) {
            std::string k, v;
            if (!SplitKeyValue(line, &k, &v))
            {
                *error = "not key=value: " + std::string(line);
                return false;
            }
            if (k == "exe")
            {
                out->exe = std::string(Trim(v));
            }
            else if (k == "cwd")
            {
                if (!NormalizeRelativePath(v, true, &out->cwd))
                {
                    *error = "cwd= must be a relative directory below the entry's directory: " + std::string(line);
                    return false;
                }
            }
            else if (k == "title")
            {
                out->title = std::string(Trim(v));
            }
            else if (k == "args")
            {
                for (std::string& w : SplitArgLine(v)) out->args.push_back(std::move(w));
            }
            else if (k == "machine")
            {
                out->machine = std::string(Trim(v));
                if (!IsMachineFolderName(out->machine))
                {
                    *error = "machine= must be a folder name under LocalState: " + std::string(line);
                    return false;
                }
            }
            else if (k == "env")
            {
                std::string ek, ev;
                if (!SplitKeyValue(v, &ek, &ev))
                {
                    *error = "env= needs KEY=VALUE: " + std::string(line);
                    return false;
                }
                out->env.emplace_back(std::move(ek), std::move(ev));
            }
            else
            {
                *error = "unknown key: " + k;
                return false;
            }
            return true;
        });
        if (!ok) return false;
        std::string norm;
        if (out->exe.empty() || !NormalizeRelativePath(out->exe, false, &norm))
        {
            *error = "exe= must name a guest program (a file name, or a relative path such as Binaries\\Win64\\App.exe)";
            return false;
        }
        out->exe = std::move(norm);
        return true;
    }
}  // namespace w10x_uwp
