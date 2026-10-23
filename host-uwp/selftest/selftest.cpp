// selftest.cpp - off-device checks of the portable host logic (launch.json
// parsing, guest path rules, command-line quoting).
//
// A plain console program with no dependency on the layer, WinRT or even
// Windows: the host-uwp workflow builds and runs it on the CI runner before the
// package is made, so the one piece of host logic that does not need a console
// is exercised for real. Exit code 0 = all checks passed.
#include "../launch_config.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

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
        CHECK(KnownExpectedExit("Hello_CUI.exe", false, &code) && code == 42);
        CHECK(KnownExpectedExit("guest_cpuid.exe", false, &code) && code == 0);
        CHECK(KnownExpectedExit("guest_xinput.exe", false, &code) && code == 1);
        CHECK(KnownExpectedExit("guest_xinput.exe", true, &code) && code == 42);
        CHECK(!KnownExpectedExit("guest_net.exe", false, &code));
        CHECK(VerdictWord(ReportVerdict(false, 42, true, 42)) == "PASS");
        CHECK(VerdictWord(ReportVerdict(false, 3, true, 42)) == "FAIL");
        CHECK(VerdictWord(ReportVerdict(false, 3, false, 0)) == "EXITED");
        CHECK(VerdictWord(ReportVerdict(false, static_cast<long>(0xC0000005u), false, 0)) == "CRASH");
        CHECK(VerdictWord(ReportVerdict(true, 0, true, 0)) == "TIMEOUT");
        ReportBlock b;
        b.program = "x.exe";
        b.exit_code = 42;
        b.verdict = ReportVerdict(false, 42, true, 42);
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
}  // namespace

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
    TestRestartArgs();
    TestReportAndAll();
    if (argc > 1) TestPackagedPayload(argv[1]);   // ctest passes host-uwp/payload
    std::printf("host_uwp_selftest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
