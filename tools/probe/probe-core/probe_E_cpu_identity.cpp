// probe_E_cpu_identity.cpp - category E: CPU identity as seen from user mode.
//
// The eventual loader patches CPUID/RDTSC in the guest; here we only *report*
// what the bare instructions return in an AppContainer so we know the starting
// point. CPUID and RDTSC are not privileged, so on a normal Windows user-mode
// thread they execute without faulting (CR4.TSD is clear); the privileged-
// instruction fault behavior is category D's job. We still wrap each read in the
// harness SEH guard so that if a given environment *does* trap them (e.g. a
// future locked-down console), we record CRASH instead of dying.
#include "probe.h"
#include <intrin.h>
#include <cstdio>

namespace probe {

namespace {
std::string hex32(uint32_t v) {
    char b[16]; std::snprintf(b, sizeof b, "0x%08X", v); return b;
}
}  // namespace

void register_E_cpu_identity(Reporter& r) {
    // Leaf 0: vendor + max standard leaf.
    r.run("E", "cpuid.0.vendor", [](Record& rec) {
        int regs[4] = {0};
        __cpuid(regs, 0);
        char vendor[13] = {0};
        *reinterpret_cast<int*>(vendor + 0) = regs[1];  // EBX
        *reinterpret_cast<int*>(vendor + 4) = regs[3];  // EDX
        *reinterpret_cast<int*>(vendor + 8) = regs[2];  // ECX
        rec.status = Status::OK;
        rec.value = std::string("vendor=") + vendor +
                    ";max_std_leaf=" + hex32((uint32_t)regs[0]);
        rec.detail = "CPUID leaf 0 executed in user mode without fault";
    });

    // Leaf 1: family/model/stepping + feature flags, incl. hypervisor-present.
    r.run("E", "cpuid.1.features", [](Record& rec) {
        int regs[4] = {0};
        __cpuid(regs, 1);
        uint32_t eax = regs[0], ecx = regs[2], edx = regs[3];
        uint32_t stepping = eax & 0xF;
        uint32_t model = (eax >> 4) & 0xF;
        uint32_t family = (eax >> 8) & 0xF;
        uint32_t ext_model = (eax >> 16) & 0xF;
        uint32_t ext_family = (eax >> 20) & 0xFF;
        uint32_t disp_family = family + ((family == 0xF) ? ext_family : 0);
        uint32_t disp_model = model + ((family == 0x6 || family == 0xF) ? (ext_model << 4) : 0);
        bool hv = (ecx >> 31) & 1;  // hypervisor-present (ECX bit 31)
        char b[256];
        std::snprintf(b, sizeof b,
            "family=%u;model=%u;stepping=%u;hypervisor_present=%d;"
            "sse3=%d;ssse3=%d;sse4_1=%d;sse4_2=%d;avx=%d;rdrand=%d;"
            "ecx=%s;edx=%s",
            disp_family, disp_model, stepping, hv ? 1 : 0,
            (ecx >> 0) & 1, (ecx >> 9) & 1, (ecx >> 19) & 1, (ecx >> 20) & 1,
            (ecx >> 28) & 1, (ecx >> 30) & 1,
            hex32(ecx).c_str(), hex32(edx).c_str());
        rec.status = Status::OK;
        rec.value = b;
        rec.detail = hv ? "HYPERVISOR-PRESENT bit SET (ECX[31]) - we run under a hypervisor"
                        : "hypervisor-present bit CLEAR - bare metal as seen by CPUID";
    });

    // Leaf 7/0: extended features (AVX2, etc.) for completeness.
    r.run("E", "cpuid.7.features", [](Record& rec) {
        int regs[4] = {0};
        __cpuidex(regs, 7, 0);
        uint32_t ebx = regs[1], ecx = regs[2];
        char b[160];
        std::snprintf(b, sizeof b,
            "avx2=%d;bmi1=%d;bmi2=%d;avx512f=%d;sha=%d;ebx=%s;ecx=%s",
            (ebx >> 5) & 1, (ebx >> 3) & 1, (ebx >> 8) & 1, (ebx >> 16) & 1,
            (ebx >> 29) & 1, hex32(ebx).c_str(), hex32(ecx).c_str());
        rec.status = Status::OK;
        rec.value = b;
        rec.detail = "CPUID leaf 7 sub 0";
    });

    // Hypervisor vendor leaf (0x40000000) - only meaningful if HV bit set, but
    // reading it is harmless either way.
    r.run("E", "cpuid.hypervisor-vendor", [](Record& rec) {
        int regs[4] = {0};
        __cpuid(regs, 0x40000000);
        char vendor[13] = {0};
        *reinterpret_cast<int*>(vendor + 0) = regs[1];
        *reinterpret_cast<int*>(vendor + 4) = regs[2];
        *reinterpret_cast<int*>(vendor + 8) = regs[3];
        bool printable = true;
        for (int i = 0; i < 12; ++i)
            if (vendor[i] && (vendor[i] < 0x20 || (unsigned char)vendor[i] > 0x7E)) printable = false;
        rec.status = Status::INFO;
        rec.value = std::string("max_hv_leaf=") + hex32((uint32_t)regs[0]) +
                    ";hv_vendor=" + (printable ? vendor : "<none>");
        rec.detail = "Hyper-V reports 'Microsoft Hv'; Xbox may report its own or nothing";
    });

    // Brand string (0x80000002..4).
    r.run("E", "cpuid.brand-string", [](Record& rec) {
        int regs[4];
        char brand[49] = {0};
        __cpuid(regs, 0x80000000);
        if ((uint32_t)regs[0] >= 0x80000004) {
            for (uint32_t i = 0; i < 3; ++i) {
                __cpuid(regs, 0x80000002 + i);
                std::memcpy(brand + i * 16, regs, 16);
            }
            rec.status = Status::OK;
            rec.value = std::string("brand=") + brand;
        } else {
            rec.status = Status::MISSING;
            rec.detail = "extended brand leaves not available";
        }
    });

    // RDTSC: does it run free in user mode? Read twice; report delta + monotonic.
    r.run("E", "rdtsc.read", [](Record& rec) {
        unsigned long long t0 = __rdtsc();
        volatile unsigned long long spin = 0;
        for (int i = 0; i < 100000; ++i) spin += i;
        unsigned long long t1 = __rdtsc();
        char b[128];
        std::snprintf(b, sizeof b, "t0=%llu;t1=%llu;delta=%llu;monotonic=%d",
                      t0, t1, t1 - t0, (t1 > t0) ? 1 : 0);
        rec.status = (t1 > t0) ? Status::OK : Status::WARN;
        rec.value = b;
        rec.detail = "RDTSC executed in user mode without fault (CR4.TSD clear); "
                     "runs free - loader must virtualize for timing-sensitive guests";
        (void)spin;
    });

    // RDTSCP (includes a serializing read of IA32_TSC_AUX) - also unprivileged.
    r.run("E", "rdtscp.read", [](Record& rec) {
        unsigned int aux = 0;
        unsigned long long t = __rdtscp(&aux);
        char b[96];
        std::snprintf(b, sizeof b, "tsc=%llu;tsc_aux=0x%X", t, aux);
        rec.status = Status::OK;
        rec.value = b;
        rec.detail = "RDTSCP ran; TSC_AUX typically encodes cpu/node";
    });

    // CPUID timing relationship to QPC - helps the loader pick a clock source.
    r.run("E", "tsc-vs-qpc", [](Record& rec) {
        LARGE_INTEGER f{}, a{}, b2{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        unsigned long long c0 = __rdtsc();
        Sleep(10);
        unsigned long long c1 = __rdtsc();
        QueryPerformanceCounter(&b2);
        double qpc_s = double(b2.QuadPart - a.QuadPart) / double(f.QuadPart ? f.QuadPart : 1);
        double tsc_hz = qpc_s > 0 ? double(c1 - c0) / qpc_s : 0;
        char buf[160];
        std::snprintf(buf, sizeof buf,
            "qpc_freq=%lld;measured_tsc_hz=%.0f;tsc_ticks=%llu;qpc_window_s=%.4f",
            f.QuadPart, tsc_hz, c1 - c0, qpc_s);
        rec.status = Status::INFO;
        rec.value = buf;
        rec.detail = "approx TSC frequency inferred from a 10ms QPC window";
    });
}

}  // namespace probe
