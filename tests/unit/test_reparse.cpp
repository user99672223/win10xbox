// Reparse buffers rewritten on their way to and from the host (core/dispatch/nt_file.cpp RewriteReparseTarget): the
// target of a junction or absolute symbolic link is a guest name for the guest and a host name for the host's NTFS.
#include "check.h"
#include "win10xbox/dispatch.h"
#include "../../core/dispatch/internal.h"
#include <windows.h>
#include <cstring>
#include <string>
#include <vector>

using namespace win10xbox::dispatch::detail;

namespace {

constexpr std::uint32_t kMountPoint = 0xA0000003, kSymlink = 0xA000000C;

// REPARSE_DATA_BUFFER as a test (and Wine's own file.c) builds it: substitute name at 0, print name behind it.
std::vector<std::uint8_t> Build(std::uint32_t tag, std::uint32_t flags, const std::wstring& sub, const std::wstring& print) {
    const std::size_t hdr = tag == kSymlink ? 20 : 16;
    const std::size_t path_bytes = (sub.size() + 1 + print.size() + 1) * 2;
    std::vector<std::uint8_t> b(hdr + path_bytes, 0);
    const std::uint16_t len = static_cast<std::uint16_t>(b.size() - 8), so = 0, sl = static_cast<std::uint16_t>(sub.size() * 2),
                        po = static_cast<std::uint16_t>((sub.size() + 1) * 2), pl = static_cast<std::uint16_t>(print.size() * 2);
    std::memcpy(&b[0], &tag, 4);
    std::memcpy(&b[4], &len, 2);
    std::memcpy(&b[8], &so, 2);
    std::memcpy(&b[10], &sl, 2);
    std::memcpy(&b[12], &po, 2);
    std::memcpy(&b[14], &pl, 2);
    if (tag == kSymlink) std::memcpy(&b[16], &flags, 4);
    std::memcpy(&b[hdr], sub.data(), sub.size() * 2);
    std::memcpy(&b[hdr + po], print.data(), print.size() * 2);
    return b;
}

std::wstring Substitute(const std::vector<std::uint8_t>& b) {
    const std::size_t hdr = b.size() >= 8 && *reinterpret_cast<const std::uint32_t*>(b.data()) == kSymlink ? 20 : 16;
    std::uint16_t so, sl;
    std::memcpy(&so, &b[8], 2);
    std::memcpy(&sl, &b[10], 2);
    return std::wstring(reinterpret_cast<const wchar_t*>(&b[hdr + so]), sl / 2);
}

std::wstring Print(const std::vector<std::uint8_t>& b) {
    const std::size_t hdr = *reinterpret_cast<const std::uint32_t*>(b.data()) == kSymlink ? 20 : 16;
    std::uint16_t po, pl;
    std::memcpy(&po, &b[12], 2);
    std::memcpy(&pl, &b[14], 2);
    return std::wstring(reinterpret_cast<const wchar_t*>(&b[hdr + po]), pl / 2);
}

bool ToHost(std::wstring_view in, std::wstring* out) {
    if (in.substr(0, 7) != L"\\??\\C:\\") return false;
    *out = L"\\??\\D:\\m\\" + std::wstring(in.substr(7));
    return true;
}

}  // namespace

TEST(reparse_mount_point_target_mapped) {
    auto in = Build(kMountPoint, 0, L"\\??\\C:\\Users\\User\\d\\", L"bogus");
    std::vector<std::uint8_t> out;
    CHECK(RewriteReparseTarget(in.data(), in.size(), ToHost, &out));
    CHECK(Substitute(out) == L"\\??\\D:\\m\\Users\\User\\d\\");
    CHECK(Print(out) == L"bogus");
    std::uint16_t len;
    std::memcpy(&len, &out[4], 2);
    CHECK_EQ(static_cast<std::size_t>(len) + 8, out.size());   // DataLength ends at the terminated print name
    std::uint16_t so, po;
    std::memcpy(&so, &out[8], 2);
    std::memcpy(&po, &out[12], 2);
    CHECK_EQ(so, 0);
    CHECK_EQ(static_cast<std::size_t>(po), (Substitute(out).size() + 1) * 2);   // print name right behind the substitute
    CHECK_EQ(out.back(), 0);
    CHECK_EQ(out[out.size() - 2], 0);
    // and the way back is the inverse: the map need not know anything but the names
    std::vector<std::uint8_t> back;
    CHECK(RewriteReparseTarget(out.data(), out.size(),
                               [](std::wstring_view n, std::wstring* o) {
                                   if (n.substr(0, 9) != L"\\??\\D:\\m\\") return false;
                                   *o = L"\\??\\C:\\" + std::wstring(n.substr(9));
                                   return true;
                               },
                               &back));
    CHECK(back == in);
}

TEST(reparse_symlink_absolute_mapped_relative_kept) {
    auto abs = Build(kSymlink, 0, L"\\??\\C:\\x\\y", L"print");
    std::vector<std::uint8_t> out;
    CHECK(RewriteReparseTarget(abs.data(), abs.size(), ToHost, &out));
    CHECK(Substitute(out) == L"\\??\\D:\\m\\x\\y");
    std::uint32_t flags = 99;
    std::memcpy(&flags, &out[16], 4);
    CHECK_EQ(flags, 0u);
    // SYMLINK_FLAG_RELATIVE: nothing to map, the host gets the buffer as written
    auto rel = Build(kSymlink, 1, L"\\??\\C:\\x", L"print");
    out.clear();
    CHECK(!RewriteReparseTarget(rel.data(), rel.size(), ToHost, &out));
    CHECK(out.empty());
}

TEST(reparse_untouched_cases) {
    std::vector<std::uint8_t> out;
    auto foobar = Build(kMountPoint, 0, L"foobar", L"bogus");              // not an NT drive name: the map declines
    CHECK(!RewriteReparseTarget(foobar.data(), foobar.size(), ToHost, &out));
    auto other = Build(0x80000017, 0, L"\\??\\C:\\x", L"p");                 // some other tag
    CHECK(!RewriteReparseTarget(other.data(), other.size(), ToHost, &out));
    CHECK(!RewriteReparseTarget(nullptr, 0, ToHost, &out));
    CHECK(!RewriteReparseTarget(foobar.data(), 7, ToHost, &out));            // shorter than the header
    auto cut = Build(kMountPoint, 0, L"\\??\\C:\\x", L"p");
    CHECK(!RewriteReparseTarget(cut.data(), cut.size() - 4, ToHost, &out));  // DataLength beyond the buffer
    auto bad = Build(kMountPoint, 0, L"\\??\\C:\\x", L"p");
    bad[10] = 0xF0; bad[11] = 0x7F;                                          // SubstituteNameLength beyond DataLength
    CHECK(!RewriteReparseTarget(bad.data(), bad.size(), ToHost, &out));
    auto odd = Build(kMountPoint, 0, L"\\??\\C:\\x", L"p");
    odd[10] = 5;                                                             // odd byte count
    CHECK(!RewriteReparseTarget(odd.data(), odd.size(), ToHost, &out));
    CHECK(out.empty());
}
