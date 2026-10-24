// selftest.cpp - off-device checks of the portable host logic (launch.json
// parsing, guest path rules, command-line quoting).
//
// A plain console program with no dependency on the layer, WinRT or even
// Windows: the host-uwp workflow builds and runs it on the CI runner before the
// package is made, so the one piece of host logic that does not need a console
// is exercised for real. Exit code 0 = all checks passed.
#include "../guest_sidecar.h"
#include "../launch_config.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace w10x_uwp;

namespace
{
    int g_checks = 0;
    int g_failed = 0;

    void Check(bool ok, const char* what, int line)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failed;
            std::printf("FAIL  selftest.cpp:%d  %s\n", line, what);
        }
    }

#define CHECK(cond) Check((cond), #cond, __LINE__)

    bool Contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

    // Parse `text` and expect an error whose message contains `needle`.
    void ExpectError(const char* text, const char* needle, int line)
    {
        LaunchConfig c;
        std::string err;
        const bool ok = ParseLaunchConfig(text, &c, &err);
        ++g_checks;
        if (ok || !Contains(err, needle))
        {
            ++g_failed;
            std::printf("FAIL  selftest.cpp:%d  expected error containing \"%s\" for:\n%s\n  got: ok=%d error=\"%s\"\n", line,
                        needle, text, ok ? 1 : 0, err.c_str());
        }
    }
#define EXPECT_ERROR(text, needle) ExpectError((text), (needle), __LINE__)

    void TestDefaults()
    {
        LaunchConfig c;
        std::string err;
        CHECK(ParseLaunchConfig("{}", &c, &err));
        CHECK(c.guest == "C:\\guest\\hello.exe");
        CHECK(c.args.empty() && c.args_raw.empty() && c.cwd.empty());
        CHECK(c.trace.empty() && c.winedebug.empty());
        CHECK(c.trace_log_max_mb == 64 && c.timeout_sec == 0);
        CHECK(c.exit_when_done && c.input_pump && !c.has_expect_exit_code);
        CHECK(c.stage_native.empty());
        // The defaults of an empty document equal the built-in defaults.
        LaunchConfig d;
        CHECK(d.guest == c.guest && d.trace_log_max_mb == c.trace_log_max_mb);
    }

    void TestFull()
    {
        const char* text = "\xEF\xBB\xBF"   // BOM
                           "// leading comment\n"
                           "{\n"
                           "  /* block\n comment */\n"
                           "  \"$schema\": \"ignored\",\n"
                           "  \"_note\": \"ignored too\",\n"
                           "  \"guest\": \"C:\\\\Program Files\\\\App\\\\app.exe\",\n"
                           "  \"args\": [\"--port\", \"8080\", \"two words\", \"q\\\"uote\"],\n"
                           "  \"cwd\": \"c:/Program Files/App\",\n"
                           "  \"trace\": \"+loader,+process\",\n"
                           "  \"winedebug\": \"+loaddll,warn+module\",\n"
                           "  \"trace_log_max_mb\": 8,\n"
                           "  \"timeout_sec\": 120,\n"
                           "  \"exit_when_done\": false,\n"
                           "  \"expect_exit_code\": -1,\n"
                           "  \"stage_native\": [\"ws2_32.dll\", \"mswsock.dll\",],\n"   // trailing comma
                           "  \"input_pump\": false,\n"
                           "}\n";
        LaunchConfig c;
        std::string err;
        CHECK(ParseLaunchConfig(text, &c, &err));
        CHECK(err.empty());
        CHECK(c.guest == "C:\\Program Files\\App\\app.exe");
        CHECK(c.args.size() == 4 && c.args[0] == "--port" && c.args[2] == "two words" && c.args[3] == "q\"uote");
        CHECK(c.cwd == "C:\\Program Files\\App");
        CHECK(c.trace == "+loader,+process");
        CHECK(c.winedebug == "+loaddll,warn+module");
        CHECK(c.trace_log_max_mb == 8 && c.timeout_sec == 120);
        CHECK(!c.exit_when_done && !c.input_pump);
        CHECK(c.has_expect_exit_code && c.expect_exit_code == -1);
        CHECK(c.stage_native.size() == 2 && c.stage_native[1] == "mswsock.dll");
        CHECK(Contains(DescribeLaunchConfig(c), "guest=C:\\Program Files\\App\\app.exe"));
        CHECK(Contains(DescribeLaunchConfig(c), "args=--port 8080 \"two words\" \"q\\\"uote\""));
    }

    void TestGuestForms()
    {
        LaunchConfig c;
        std::string err;
        CHECK(ParseLaunchConfig("{\"guest\": \"hello_cui.exe\"}", &c, &err) && c.guest == "C:\\guest\\hello_cui.exe");
        CHECK(ParseLaunchConfig("{\"guest\": \"d:/games/x.exe\"}", &c, &err) && c.guest == "D:\\games\\x.exe");
        CHECK(ParseLaunchConfig("{\"args\": \"-a \\\"b c\\\" -d\"}", &c, &err) && c.args_raw == "-a \"b c\" -d" && c.args.empty());
        CHECK(ParseLaunchConfig("{\"cwd\": \"\"}", &c, &err) && c.cwd.empty());
        // Unicode escapes, including a surrogate pair (U+1F600 = F0 9F 98 80).
        CHECK(ParseLaunchConfig("{\"guest\": \"C:\\\\\\u00e4\\\\x.exe\"}", &c, &err) && c.guest == "C:\\\xC3\xA4\\x.exe");
        CHECK(ParseLaunchConfig("{\"args\": [\"\\ud83d\\ude00\"]}", &c, &err) && c.args.size() == 1 && c.args[0] == "\xF0\x9F\x98\x80");
    }

    void TestErrors()
    {
        // The classic mistake: single backslashes.
        EXPECT_ERROR("{\"guest\": \"C:\\guest\\app.exe\"}", "invalid escape");
        // ...and the one that parses but is wrong (\t is a TAB, \a is invalid): caught by the control-character rule.
        EXPECT_ERROR("{\"guest\": \"C:\\\\x\\temp.exe\"}", "control character");
        EXPECT_ERROR("{\"gest\": \"a.exe\"}", "unknown key \"gest\"");
        EXPECT_ERROR("{\n\n  \"gest\": \"a.exe\"}", "line 3");
        EXPECT_ERROR("{\"guest\": 5}", "\"guest\" must be a string");
        EXPECT_ERROR("{\"guest\": \"\"}", "must not be empty");
        EXPECT_ERROR("{\"guest\": \"sub\\\\a.exe\"}", "bare file name");
        EXPECT_ERROR("{\"guest\": \"C:\\\\dir\\\\\"}", "directory");
        EXPECT_ERROR("{\"guest\": \"a|b.exe\"}", "not valid in a Windows path");
        EXPECT_ERROR("{\"cwd\": \"work\"}", "absolute guest path");
        EXPECT_ERROR("{\"timeout_sec\": -1}", "between 0 and");
        EXPECT_ERROR("{\"timeout_sec\": 1.5}", "must be an integer");
        EXPECT_ERROR("{\"timeout_sec\": \"10\"}", "must be an integer");
        EXPECT_ERROR("{\"timeout_sec\": 99999999999999999999}", "must be an integer");
        EXPECT_ERROR("{\"trace_log_max_mb\": 5000}", "between 0 and 4096");
        EXPECT_ERROR("{\"exit_when_done\": \"yes\"}", "true or false");
        EXPECT_ERROR("{\"input_pump\": 1}", "true or false");
        EXPECT_ERROR("{\"args\": [1]}", "entries must be strings");
        EXPECT_ERROR("{\"args\": 7}", "array of strings or one string");
        EXPECT_ERROR("{\"stage_native\": \"ws2_32.dll\"}", "array of file names");
        EXPECT_ERROR("{\"stage_native\": [\"..\\\\x.dll\"]}", "bare file names");
        EXPECT_ERROR("{\"stage_native\": [\"sub/x.dll\"]}", "bare file names");
        EXPECT_ERROR("{\"trace\": \"+all\\n\"}", "printable ASCII");
        EXPECT_ERROR("{\"guest\": \"a.exe\", \"guest\": \"b.exe\"}", "duplicate key");
        EXPECT_ERROR("[]", "top level must be a JSON object");
        EXPECT_ERROR("\"x\"", "top level must be a JSON object");
        EXPECT_ERROR("", "a value is missing");
    }

    void TestSyntaxErrors()
    {
        EXPECT_ERROR("{", "unterminated object");
        EXPECT_ERROR("{\"guest\"", "expected ':'");
        EXPECT_ERROR("{\"guest\": ", "a value is missing");
        EXPECT_ERROR("{\"guest\": \"a.exe\" \"args\": []}", "expected ',' or '}'");
        EXPECT_ERROR("{, \"guest\": \"a.exe\"}", "expected a quoted key");
        EXPECT_ERROR("{\"guest\": \"a.exe\",, }", "expected a quoted key");
        EXPECT_ERROR("{guest: \"a.exe\"}", "expected a quoted key");
        EXPECT_ERROR("{\"guest\": 'a.exe'}", "unexpected character");
        EXPECT_ERROR("{\"guest\": \"unterminated}", "unterminated string");
        EXPECT_ERROR("{\"guest\": \"line\nbreak\"}", "unterminated string");
        EXPECT_ERROR("{\"args\": [\"a\" \"b\"]}", "expected ',' or ']'");
        EXPECT_ERROR("{\"args\": [,]}", "found ','");
        EXPECT_ERROR("{\"args\": [", "unterminated array");
        EXPECT_ERROR("{} trailing", "unexpected text after");
        EXPECT_ERROR("{} {}", "unexpected text after");
        EXPECT_ERROR("/* never closed {}", "unterminated /* comment");
        EXPECT_ERROR("{\"a\": tru}", "invalid value");
        EXPECT_ERROR("{\"a\": truex}", "invalid value");
        EXPECT_ERROR("{\"a\": 01}", "leading zeros");
        EXPECT_ERROR("{\"a\": -}", "invalid number");
        EXPECT_ERROR("{\"a\": 1.}", "invalid number");
        EXPECT_ERROR("{\"a\": 1e}", "invalid number");
        EXPECT_ERROR("{\"guest\": \"\\u12\"}", "invalid \\u escape");
        EXPECT_ERROR("{\"guest\": \"\\ud800\"}", "surrogate");
        EXPECT_ERROR("{\"guest\": \"\\udc00\"}", "surrogate");
        EXPECT_ERROR("{\"guest\": \"\\u0000\"}", "\\u0000");
        EXPECT_ERROR("{\"a\": \x01}", "unexpected character");

        // Line numbers follow newlines in comments and values.
        EXPECT_ERROR("// one\n/* two\nthree */\n{\n  \"guest\": 1\n}", "line 5");
    }

    void TestLimits()
    {
        // Deep nesting is refused instead of recursing.
        std::string deep;
        for (int i = 0; i < 100; ++i) deep += "[";
        EXPECT_ERROR(("{\"args\": " + deep).c_str(), "nesting is too deep");
        // A huge input is refused up front.
        const std::string huge(300 * 1024, ' ');
        LaunchConfig c;
        std::string err;
        CHECK(!ParseLaunchConfig(huge + "{}", &c, &err) && Contains(err, "larger than 256 KiB"));
        // A failed parse leaves the output untouched.
        LaunchConfig keep;
        keep.guest = "C:\\keep.exe";
        CHECK(!ParseLaunchConfig("{\"guest\": 1}", &keep, &err));
        CHECK(keep.guest == "C:\\keep.exe");
        // No error pointer is fine.
        CHECK(ParseLaunchConfig("{}", &c, nullptr));
        CHECK(!ParseLaunchConfig("{", &c, nullptr));
    }

    void TestPathHelpers()
    {
        std::string out, why;
        CHECK(NormalizeGuestPath("app.exe", true, &out, &why) && out == "C:\\guest\\app.exe");
        CHECK(!NormalizeGuestPath("app.exe", false, &out, &why));
        CHECK(NormalizeGuestPath("c:\\a\\b.exe", true, &out, &why) && out == "C:\\a\\b.exe");
        CHECK(NormalizeGuestPath("E:/a/b", false, &out, &why) && out == "E:\\a\\b");
        CHECK(!NormalizeGuestPath("\\a\\b.exe", true, &out, &why));
        CHECK(!NormalizeGuestPath("C:b.exe", true, &out, &why));
        CHECK(!NormalizeGuestPath("C:\\a:b", true, &out, &why));
        CHECK(!NormalizeGuestPath("\\\\srv\\share\\x.exe", true, &out, &why));
        CHECK(!NormalizeGuestPath("..", true, &out, &why));
        CHECK(IsLeafNameUtf8("ws2_32.dll") && IsLeafNameUtf8("c_1252.nls"));
        CHECK(!IsLeafNameUtf8("") && !IsLeafNameUtf8(".") && !IsLeafNameUtf8("..") && !IsLeafNameUtf8("a\\b") &&
              !IsLeafNameUtf8("a/b") && !IsLeafNameUtf8("C:x") && !IsLeafNameUtf8("a*b") && !IsLeafNameUtf8(std::string(129, 'a')));
    }

    void TestQuoting()
    {
        CHECK(QuoteArg(std::string("plain")) == "plain");
        CHECK(QuoteArg(std::string("")) == "\"\"");
        CHECK(QuoteArg(std::string("two words")) == "\"two words\"");
        CHECK(QuoteArg(std::string("tab\there")) == "\"tab\there\"");
        CHECK(QuoteArg(std::string("say \"hi\"")) == "\"say \\\"hi\\\"\"");
        CHECK(QuoteArg(std::string("C:\\dir\\")) == "C:\\dir\\");                       // no quoting needed, backslashes untouched
        CHECK(QuoteArg(std::string("C:\\my dir\\")) == "\"C:\\my dir\\\\\"");           // trailing backslashes doubled before the closing quote
        CHECK(QuoteArg(std::string("a\\\"b")) == "\"a\\\\\\\"b\"");                      // backslash before a quote: doubled, then the quote escaped
        CHECK(QuoteArg(std::wstring(L"two words")) == L"\"two words\"");
        CHECK(QuoteArg(std::wstring(L"C:\\my dir\\")) == L"\"C:\\my dir\\\\\"");
    }

    void TestLauncherKeys()
    {
        LaunchConfig c;
        std::string err;
        CHECK(ParseLaunchConfig("{}", &c, &err) && !c.has_picker && c.autoselect.empty());
        CHECK(ParseLaunchConfig("{\"picker\": true, \"autoselect\": \"hello_cui.exe\"}", &c, &err));
        CHECK(c.has_picker && c.picker && c.autoselect.size() == 1 && c.autoselect[0] == "hello_cui.exe");
        CHECK(Contains(DescribeLaunchConfig(c), "autoselect=hello_cui.exe"));
        CHECK(ParseLaunchConfig("{\"autoselect\": [\"hello_cui.exe\", \"guest_hello.exe\"]}", &c, &err));
        CHECK(c.autoselect.size() == 2 && c.autoselect[1] == "guest_hello.exe");
        CHECK(Contains(DescribeLaunchConfig(c), "autoselect=hello_cui.exe,guest_hello.exe"));
        CHECK(ParseLaunchConfig("{\"autoselect\": []}", &c, &err) && c.autoselect.empty());
        EXPECT_ERROR("{\"autoselect\": [\"a.exe\", \"C:\\\\x.exe\"]}", "bare file names");
        EXPECT_ERROR("{\"autoselect\": [1]}", "bare file names");
        EXPECT_ERROR("{\"picker\": \"yes\"}", "true or false");
        EXPECT_ERROR("{\"autoselect\": \"C:\\\\x.exe\"}", "bare file name");
    }

    void TestReportAndAll()
    {
        LaunchConfig c;
        std::string err;
        CHECK(ParseLaunchConfig("{\"autoselect\": \"all\", \"autoselect_timeout_sec\": 30}", &c, &err));
        CHECK(c.autoselect_all && c.autoselect.empty() && c.autoselect_timeout_sec == 30);
        CHECK(ParseLaunchConfig("{\"autoselect\": \"all\"}", &c, &err) && c.autoselect_timeout_sec == 60);
        EXPECT_ERROR("{\"autoselect_timeout_sec\": 0}", "autoselect_timeout_sec");
        CHECK(FormatRestartArgs(3, "a.exe", 2) == "picker run=3 all=2 select=a.exe");
        RestartArgs r = ParseRestartArgs("picker run=3 all=2 select=a b.exe");
        CHECK(r.valid && r.run == 3 && r.all_k == 2 && r.select == "a b.exe");
        r = ParseRestartArgs("picker run=3 select=a.exe");
        CHECK(r.valid && r.all_k == -1);
        CHECK(!ParseRestartArgs("picker all=x").valid);
        int code = -1;
        ExpectedTable t;
        CHECK(FindExpected(t, "Hello_CUI.exe") && (*FindExpected(t, "Hello_CUI.exe"))[0].code == 42);
        CHECK(FindExpected(t, "guest_cpuid.exe") && (*FindExpected(t, "guest_cpuid.exe"))[0].code == 0);
        CHECK(FindExpected(t, "guest_xinput.exe") && FindExpected(t, "guest_xinput.exe")->size() == 2);
        CHECK(!FindExpected(t, "guest_net.exe"));
        CHECK(ParseExpectedTable("# c\nguest_wasapi.exe 42=endpoint 43=no-endpoint  # tail\nchocolate-doom.exe TIMEOUT=interactive\n"
                                 "doom-timedemo.launch -1=timedemo\n", &t, &err));
        const std::vector<ExpectedRow>* w = FindExpected(t, "GUEST_WASAPI.exe");
        CHECK(w && w->size() == 2 && (*w)[1].code == 43 && (*w)[1].label == "no-endpoint");
        CHECK(ReportVerdict(false, 43, w) == "PASS (exit code 43 = no-endpoint)");
        CHECK(VerdictWord(ReportVerdict(false, 3, w)) == "FAIL");
        CHECK(ReportVerdict(false, -1, FindExpected(t, "doom-timedemo.launch")) == "PASS (exit code -1 = timedemo)");
        CHECK(VerdictWord(ReportVerdict(true, 0, FindExpected(t, "chocolate-doom.exe"))) == "TIMEOUT" &&
              Contains(ReportVerdict(true, 0, FindExpected(t, "chocolate-doom.exe")), "expected: interactive"));
        CHECK(!Contains(ReportVerdict(true, 0, w), "expected"));
        CHECK(ParseExpectedTable("s.exe 42=presented KNOWNFAIL=2:uwp-no-hwnd(-uwpswap) KNOWNFAIL=-2147483392:x\n", &t, &err));
        const std::vector<ExpectedRow>* k = FindExpected(t, "s.exe");
        CHECK(k && k->size() == 3 && (*k)[1].known_fail && (*k)[1].code == 2 && (*k)[2].code == -2147483392LL);
        CHECK(ReportVerdict(false, 2, k) == "KNOWN-FAIL (exit code 2 = uwp-no-hwnd(-uwpswap))");
        CHECK(VerdictWord(ReportVerdict(false, 2, k)) == "KNOWN-FAIL" && VerdictWord(ReportVerdict(false, 42, k)) == "PASS");
        CHECK(VerdictWord(ReportVerdict(false, 3, k)) == "FAIL" && !Contains(ReportVerdict(false, 3, k), "or 2"));
        CHECK(!ParseExpectedTable("a.exe KNOWNFAIL=2\n", &t, &err) && !ParseExpectedTable("a.exe KNOWNFAIL=x:y\n", &t, &err));
        CHECK(!ParseExpectedTable("a.exe\n", &t, &err) && Contains(err, "line 1"));
        CHECK(!ParseExpectedTable("a.exe 4x\n", &t, &err));
        CHECK(!ParseExpectedTable("a.exe 1\nA.exe 2\n", &t, &err) && Contains(err, "twice"));
        CHECK(VerdictWord(ReportVerdict(false, 3, nullptr)) == "EXITED");
        CHECK(VerdictWord(ReportVerdict(false, static_cast<long>(0xC0000005u), nullptr)) == "CRASH");
        CHECK(VerdictWord(ReportVerdict(true, 0, nullptr)) == "TIMEOUT");
        ReportBlock b;
        b.program = "x.exe";
        b.exit_code = 42;
        b.verdict = ReportVerdict(false, 42, FindExpected(ExpectedTable(), "window.exe"));
        b.screen = "x.bmp";
        b.crash = true;
        b.crash_lines = {"c1"};
        b.stdout_lines = {"hello", "world"};
        ReportBlock b2 = b;
        b2.program = "y.exe";
        b2.crash = false;
        b2.crash_lines.clear();
        const std::vector<ReportBlock> back = ParseReport("# header\nprograms=2\n\n" + FormatReportBlock(b) + FormatReportBlock(b2));
        CHECK(back.size() == 2 && back[0].program == "x.exe" && back[0].exit_code == 42 && back[0].crash &&
              back[0].crash_lines.size() == 1 && back[0].stdout_lines.size() == 2 && back[0].screen == "x.bmp" &&
              VerdictWord(back[0].verdict) == "PASS" && back[1].program == "y.exe" && !back[1].crash);
    }

    void TestRestartArgs()
    {
        CHECK(FormatRestartArgs(1, "hello_cui.exe") == "picker run=1 select=hello_cui.exe");
        CHECK(FormatRestartArgs(2, "") == "picker run=2");
        RestartArgs r = ParseRestartArgs("picker run=1 select=hello_cui.exe");
        CHECK(r.valid && r.run == 1 && r.select == "hello_cui.exe");
        r = ParseRestartArgs(FormatRestartArgs(7, "my prog.exe"));
        CHECK(r.valid && r.run == 7 && r.select == "my prog.exe");
        r = ParseRestartArgs("  picker \r\n");
        CHECK(r.valid && r.run == 0 && r.select.empty());
        r = ParseRestartArgs("picker select=a.exe");
        CHECK(r.valid && r.run == 0 && r.select == "a.exe");
        CHECK(!ParseRestartArgs("").valid);
        CHECK(!ParseRestartArgs("--restart").valid);
        CHECK(!ParseRestartArgs("pickerx run=1").valid);
        CHECK(!ParseRestartArgs("picker run=").valid);
        CHECK(!ParseRestartArgs("picker run=x").valid);
        CHECK(!ParseRestartArgs("picker run=99999").valid);
        CHECK(!ParseRestartArgs("picker run=1x select=a.exe").valid);
        CHECK(!ParseRestartArgs("picker select=C:\\x.exe").valid);
        CHECK(!ParseRestartArgs("picker bogus=1").valid);
    }

    // The files the package actually ships (payload\launch.json is what a fresh install runs):
    // a parse error there would only show up on the console.
    void TestPackagedPayload(const std::string& dir)
    {
        auto parse = [&](const char* name, LaunchConfig* c) {
            std::ifstream f(dir + "/" + name, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string err;
            const bool ok = f.good() || f.eof() ? ParseLaunchConfig(text, c, &err) : false;
            if (!ok) std::printf("FAIL  payload/%s: %s\n", name, text.empty() ? "cannot read" : err.c_str());
            return ok && !text.empty();
        };
        LaunchConfig c;
        CHECK(parse("launch.json", &c));
        CHECK(c.guest == "C:\\guest\\window.exe");
        CHECK(c.has_expect_exit_code && c.expect_exit_code == 42);
        CHECK(!c.exit_when_done && c.timeout_sec == 600);
        CHECK(c.trace == "+user,+gdi");
        LaunchConfig e;
        CHECK(parse("launch.example.json", &e));
        CHECK(e.guest == "C:\\guest\\hello.exe");   // documents the built-in defaults
    }

    std::string ReadAll(const std::string& path, bool* ok)
    {
        std::ifstream f(path, std::ios::binary);
        *ok = f.good();
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    void TestSidecars()
    {
        // .args: one argument per line, verbatim, CRLF tolerated, blank and '#' lines skipped, BOM dropped.
        std::vector<std::string> a;
        ParseArgsFile("\xEF\xBB\xBF-iwad\r\ndoom1.wad\r\n\r\n# comment\n-title\nTwo Words\n  lead\n", &a);
        CHECK(a.size() == 5 && a[0] == "-iwad" && a[1] == "doom1.wad" && a[2] == "-title" && a[3] == "Two Words" && a[4] == "  lead");
        ParseArgsFile("", &a);
        CHECK(a.empty());
        ParseArgsFile("last-without-newline", &a);
        CHECK(a.size() == 1 && a[0] == "last-without-newline");

        // .env
        std::vector<std::pair<std::string, std::string>> e;
        std::string err;
        CHECK(ParseEnvFile("SDL_RENDER_DRIVER=software\r\n# x\n\n A = b=c \nEMPTY=\n", &e, &err));
        CHECK(e.size() == 3);
        CHECK(e.size() == 3 && e[0].first == "SDL_RENDER_DRIVER" && e[0].second == "software");
        CHECK(e.size() == 3 && e[1].first == "A" && e[1].second == " b=c ");
        CHECK(e.size() == 3 && e[2].first == "EMPTY" && e[2].second.empty());
        CHECK(!ParseEnvFile("OK=1\nnoequals\n", &e, &err) && Contains(err, "noequals"));
        CHECK(!ParseEnvFile("=v\n", &e, &err));

        // .title
        CHECK(ParseTitleFile("Doom\r\n") == "Doom");
        CHECK(ParseTitleFile("\n  \n  Spaced Title \nsecond\n") == "Spaced Title");
        CHECK(ParseTitleFile("# only a comment\n").empty());

        // argument line of a .launch file
        const std::vector<std::string> w = SplitArgLine("-a  \"b c\" d\"e f\"g \"\" -z");
        CHECK(w.size() == 5 && w[0] == "-a" && w[1] == "b c" && w[2] == "de fg" && w[3].empty() && w[4] == "-z");
        CHECK(SplitArgLine("   ").empty());

        // .launch
        GuestLaunch g;
        CHECK(ParseLaunchFile("# t\nexe=chocolate-doom.exe\r\ntitle=Doom (timedemo)\nargs=-iwad doom1.wad -window\n"
                              "args=-nosound\nenv=SDL_RENDER_DRIVER=software\nenv=K=a=b\n", &g, &err));
        CHECK(g.exe == "chocolate-doom.exe" && g.title == "Doom (timedemo)");
        CHECK(g.args.size() == 4 && g.args[0] == "-iwad" && g.args[2] == "-window" && g.args[3] == "-nosound");
        CHECK(g.env.size() == 2 && g.env[0].first == "SDL_RENDER_DRIVER" && g.env[1].second == "a=b");
        CHECK(!ParseLaunchFile("title=x\n", &g, &err) && Contains(err, "exe="));
        // relative exe= with subdirectories, cwd= (directory installs)
        CHECK(ParseLaunchFile("exe=Binaries/Win64\\App.exe\ncwd=Binaries\\Win64\n", &g, &err));
        CHECK(g.exe == "Binaries\\Win64\\App.exe" && g.cwd == "Binaries\\Win64");
        CHECK(ParseLaunchFile("exe=App.exe\ncwd=.\n", &g, &err) && g.cwd == ".");
        CHECK(ParseLaunchFile("exe=App.exe\n", &g, &err) && g.cwd.empty());
        CHECK(!ParseLaunchFile("exe=sub\\\\x.exe\n", &g, &err));          // empty component
        CHECK(!ParseLaunchFile("exe=..\\x.exe\n", &g, &err));              // no way out of the directory
        CHECK(!ParseLaunchFile("exe=a\\..\\x.exe\n", &g, &err));
        CHECK(!ParseLaunchFile("exe=C:\\x.exe\n", &g, &err));              // absolute
        CHECK(!ParseLaunchFile("exe=\\x.exe\n", &g, &err));
        CHECK(!ParseLaunchFile("exe=sub\\\n", &g, &err));
        CHECK(!ParseLaunchFile("exe=a.exe\ncwd=..\n", &g, &err) && Contains(err, "cwd="));
        CHECK(!ParseLaunchFile("exe=a.exe\ncwd=C:\\x\n", &g, &err) && Contains(err, "cwd="));
        CHECK(!ParseLaunchFile("exe=a.exe\ncwd=\n", &g, &err) && Contains(err, "cwd="));
        CHECK(!ParseLaunchFile("exe=a.exe\nbogus=1\n", &g, &err) && Contains(err, "bogus"));
        CHECK(!ParseLaunchFile("exe=a.exe\nenv=NOEQ\n", &g, &err));
        CHECK(!ParseLaunchFile("exe a.exe\n", &g, &err));

        // machine folder: .launch machine=, .machine file, launch.json "machine"
        CHECK(ParseLaunchFile("exe=a.exe\nmachine=machine-native-d3d\n", &g, &err) && g.machine == kD3dMachineName);
        CHECK(ParseLaunchFile("exe=a.exe\n", &g, &err) && g.machine.empty());
        CHECK(!ParseLaunchFile("exe=a.exe\nmachine=..\\x\n", &g, &err) && Contains(err, "machine="));
        CHECK(!ParseLaunchFile("exe=a.exe\nmachine=.hidden\n", &g, &err));
        bool mok = false;
        CHECK(ParseMachineFile("# why\r\n\n  machine-native-d3d \nignored\n", &mok) == "machine-native-d3d" && mok);
        CHECK(ParseMachineFile("# only a comment\n", &mok).empty() && mok);
        CHECK(ParseMachineFile("a/b\n", &mok).empty() && !mok);
        CHECK(ParseMachineFile("..\n", &mok).empty() && !mok);
        LaunchConfig lc;
        CHECK(ParseLaunchConfig("{\"machine\": \"machine-native-d3d\"}", &lc, &err) && lc.machine == "machine-native-d3d");
        CHECK(Contains(DescribeLaunchConfig(lc), "machine=machine-native-d3d"));
        CHECK(ParseLaunchConfig("{}", &lc, &err) && lc.machine.empty() && Contains(DescribeLaunchConfig(lc), "machine=machine\n"));
        CHECK(ParseLaunchConfig("{\"display_mode\": \"2560x1440@120\", \"guest_display\": \"3840x2160@120\", \"output_scaling\": \"layer\"}", &lc, &err) &&
              lc.display_mode == "2560x1440@120" && lc.guest_display == "3840x2160@120" && lc.output_scaling == "layer");
        CHECK(ParseLaunchConfig("{}", &lc, &err) && lc.display_mode == "auto" && lc.guest_display == "auto" && lc.output_scaling == "system");
        CHECK(!ParseLaunchConfig("{\"display_mode\": \"big\"}", &lc, &err) && Contains(err, "WxH@Hz"));
        CHECK(!ParseLaunchConfig("{\"output_scaling\": \"gpu\"}", &lc, &err) && Contains(err, "system"));
        CHECK(!ParseLaunchConfig("{\"machine\": 3}", &lc, &err) && Contains(err, "must be a string"));
        CHECK(!ParseLaunchConfig("{\"machine\": \"sub\\\\x\"}", &lc, &err) && Contains(err, "folder name"));
        CHECK(!ParseLaunchConfig("{\"machine\": \"\"}", &lc, &err));
        const std::vector<std::string>& dn = D3dMachineNativeDlls();
        for (const char* need : {"user32.dll", "win32u.dll", "gdi32.dll", "d3d11.dll", "dxgi.dll", "d3dcompiler_47.dll", "d3d10warp.dll"})
            CHECK(std::find(dn.begin(), dn.end(), need) != dn.end());
    }

    // The sidecars host-uwp/payload/guest ships for Chocolate Doom.
    void TestPackagedSidecars(const std::string& dir)
    {
        bool ok = false;
        std::vector<std::string> a;
        ParseArgsFile(ReadAll(dir + "/guest/chocolate-doom.args", &ok), &a);
        CHECK(ok && a.size() == 4 && a[0] == "-iwad" && a[1] == "doom1.wad" && a[2] == "-fullscreen" && a[3] == "-nomouse");
        std::vector<std::pair<std::string, std::string>> e;
        std::string err;
        CHECK(ParseEnvFile(ReadAll(dir + "/guest/chocolate-doom.env", &ok), &e, &err) && ok);
        CHECK(e.size() == 1 && e[0].first == "SDL_RENDER_DRIVER" && e[0].second == "software");
        CHECK(!ParseTitleFile(ReadAll(dir + "/guest/chocolate-doom.title", &ok)).empty() && ok);
        GuestLaunch g;
        CHECK(ParseLaunchFile(ReadAll(dir + "/guest/doom-timedemo.launch", &ok), &g, &err) && ok);
        CHECK(g.exe == "chocolate-doom.exe" && g.title == "Doom (timedemo)");
        bool timedemo = false, nogui = false;
        for (const std::string& s : g.args) { timedemo |= s == "-timedemo"; nogui |= s == "-nogui"; }
        CHECK(timedemo && nogui);
        CHECK(g.env.size() == 1 && g.env[0].first == "SDL_RENDER_DRIVER");
        // The D3D11 guests run on the route-B + Direct3D 11 machine folder.
        bool mok = false;
        CHECK(ParseMachineFile(ReadAll(dir + "/guest/guest_d3d11_tri.machine", &ok), &mok) == kD3dMachineName && ok && mok);
        CHECK(ParseMachineFile(ReadAll(dir + "/guest/guest_imgui_d3d11.machine", &ok), &mok) == kEmuD3dMachineName && ok && mok);
        CHECK(ParseMachineFile(ReadAll(dir + "/guest/guest_d3d11_swap.machine", &ok), &mok) == kEmuD3dMachineName && ok && mok);
        CHECK(ParseMachineFile(ReadAll(dir + "/guest/guest_d3dsample.machine", &ok), &mok) == kEmuD3dMachineName && ok && mok);
        {
            // guest_d3dsample: present limit in .env, working directory C:\guest in .cwd, a title.
            std::vector<std::pair<std::string, std::string>> env;
            std::string why, cwd;
            CHECK(ParseEnvFile(ReadAll(dir + "/guest/guest_d3dsample.env", &ok), &env, &why) && ok && env.size() == 1 &&
                  env[0].first == "WIN10XBOX_TEST_PRESENT_LIMIT" && env[0].second == "60");
            CHECK(NormalizeRelativePath(ParseTitleFile(ReadAll(dir + "/guest/guest_d3dsample.cwd", &ok)), true, &cwd) && ok && cwd == ".");
            CHECK(!ParseTitleFile(ReadAll(dir + "/guest/guest_d3dsample.title", &ok)).empty() && ok);
        }
        // The packaged default launch.json keeps the default machine folder.
        LaunchConfig pl;
        CHECK(ParseLaunchConfig(ReadAll(dir + "/launch.json", &ok), &pl, &err) && ok && pl.machine.empty());
    }
}  // namespace

// tests/guest/expected_exit.txt (argv[2]) parses; the copy staged into the package (argv[3], when given) is identical.
void TestExpectedTable(const std::string& src, const std::string& staged)
{
    bool ok = false;
    const std::string text = ReadAll(src, &ok);
    ExpectedTable t;
    std::string err;
    CHECK(ok && ParseExpectedTable(text, &t, &err));
    if (!err.empty()) std::printf("expected_exit.txt: %s\n", err.c_str());
    CHECK(t.size() >= 35);
    const std::vector<ExpectedRow>* d = FindExpected(t, "doom-timedemo.launch");
    CHECK(d && d->size() == 1 && (*d)[0].code == -1 && (*d)[0].label == "timedemo");
    const std::vector<ExpectedRow>* c = FindExpected(t, "chocolate-doom.exe");
    CHECK(c && c->size() == 1 && (*c)[0].timeout);
    const std::vector<ExpectedRow>* sw = FindExpected(t, "guest_d3d11_swap.exe");
    CHECK(sw && sw->size() == 1 && (*sw)[0].code == 42);
    const std::vector<ExpectedRow>* ig = FindExpected(t, "guest_imgui_d3d11.exe");
    CHECK(ig && ig->size() == 1 && (*ig)[0].code == 0);
    for (const char* n : {"window.exe", "hello_cui.exe", "guest_hello.exe", "guest_cpuid.exe", "guest_xinput.exe"}) CHECK(t.count(n) == 1);
    if (!staged.empty())
    {
        bool ok2 = false;
        const std::string copy = ReadAll(staged, &ok2);
        if (ok2) CHECK(copy == text);   // not staged yet (a plain local ctest): the workflow checks the package itself
    }
}

int main(int argc, char** argv)
{
    TestDefaults();
    TestFull();
    TestGuestForms();
    TestErrors();
    TestSyntaxErrors();
    TestLimits();
    TestPathHelpers();
    TestQuoting();
    TestLauncherKeys();
    TestSidecars();
    if (argc > 1) TestPackagedSidecars(argv[1]);
    TestRestartArgs();
    TestReportAndAll();
    if (argc > 1) TestPackagedPayload(argv[1]);   // ctest passes host-uwp/payload
    if (argc > 2) TestExpectedTable(argv[2], argc > 3 ? argv[3] : "");
    std::printf("host_uwp_selftest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
