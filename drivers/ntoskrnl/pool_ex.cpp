// ntoskrnl Ex* surface: pool, resources, lookaside lists, work items,
// interlocked list helpers, time conversions. MIT.
//
// The pool is a private Win32 heap; every block carries a 16-byte header
// (magic, tag, size) so frees are validated and per-tag leaks are visible
// to tests (PoolBlocksOutstandingForTag). All pool memory is zeroed: Windows
// does not guarantee that, but drivers that rely on it are common and
// zeroing hides no bug a driver is allowed to depend on.
#include "internal.h"
#include <atomic>
#include <unordered_map>

namespace win10xbox::drivers::detail {
namespace {

struct PoolHdr {
    std::uint32_t magic;
    std::uint32_t tag;
    std::uint64_t size;
};
static_assert(sizeof(PoolHdr) == 16);
constexpr std::uint32_t kLive = 0xA110C8EDu, kDead = 0xDEADF00Du;

HANDLE g_heap = nullptr;
std::atomic<std::uint64_t> g_allocs{0}, g_frees{0}, g_blocks{0}, g_bytes{0};
SRWLOCK g_tag_lock = SRWLOCK_INIT;
std::unordered_map<std::uint32_t, std::int64_t>& TagBlocks() {
    static std::unordered_map<std::uint32_t, std::int64_t> m;
    return m;
}

void TagAdjust(std::uint32_t tag, int d) {
    AcquireSRWLockExclusive(&g_tag_lock);
    TagBlocks()[tag] += d;
    ReleaseSRWLockExclusive(&g_tag_lock);
}

}  // namespace

bool PoolInit() {
    if (g_heap) return true;
    g_heap = HeapCreate(0, 0, 0);
    return g_heap != nullptr;
}

void PoolShutdown() {
    if (!g_heap) return;
    if (g_blocks.load()) WARN(driver, "pool: %llu blocks (%llu bytes) still allocated at shutdown",
                              static_cast<unsigned long long>(g_blocks.load()), static_cast<unsigned long long>(g_bytes.load()));
    HeapDestroy(g_heap);
    g_heap = nullptr;
    g_allocs = g_frees = g_blocks = g_bytes = 0;
    AcquireSRWLockExclusive(&g_tag_lock);
    TagBlocks().clear();
    ReleaseSRWLockExclusive(&g_tag_lock);
}

void* PoolAlloc(SIZE_T bytes, ULONG tag) {
    if (!g_heap) return nullptr;
    if (bytes == 0) bytes = 1;
    if (bytes > (SIZE_T{1} << 40)) return nullptr;
    auto* h = static_cast<PoolHdr*>(HeapAlloc(g_heap, HEAP_ZERO_MEMORY, bytes + sizeof(PoolHdr)));
    if (!h) return nullptr;
    h->magic = kLive;
    h->tag = tag;
    h->size = bytes;
    ++g_allocs;
    ++g_blocks;
    g_bytes += bytes;
    TagAdjust(tag, +1);
    return h + 1;
}

void PoolFree(void* p, ULONG tag, bool check_tag) {
    if (!p) return;
    auto* h = static_cast<PoolHdr*>(p) - 1;
    if (!ProbeRange(h, sizeof(*h), false) || h->magic != kLive) {
        ERR(driver, "ExFreePool(%p): not a live pool block (header magic %08x)", p,
            ProbeRange(h, sizeof(*h), false) ? h->magic : 0u);
        return;
    }
    if (check_tag && tag && h->tag != tag)
        WARN(driver, "ExFreePoolWithTag(%p): tag %.4s does not match the allocation tag %.4s", p,
             reinterpret_cast<const char*>(&tag), reinterpret_cast<const char*>(&h->tag));
    const std::uint64_t size = h->size;
    const std::uint32_t t = h->tag;
    h->magic = kDead;
    HeapFree(g_heap, 0, h);
    ++g_frees;
    --g_blocks;
    g_bytes -= size;
    TagAdjust(t, -1);
}

}  // namespace win10xbox::drivers::detail

namespace win10xbox::drivers {
PoolStats GetPoolStats() {
    using namespace detail;
    return PoolStats{g_allocs.load(), g_frees.load(), g_blocks.load(), g_bytes.load()};
}
std::uint64_t PoolBlocksOutstandingForTag(std::uint32_t tag) {
    using namespace detail;
    AcquireSRWLockShared(&g_tag_lock);
    auto it = TagBlocks().find(tag);
    std::int64_t n = it == TagBlocks().end() ? 0 : it->second;
    ReleaseSRWLockShared(&g_tag_lock);
    return n < 0 ? 0 : static_cast<std::uint64_t>(n);
}
}  // namespace win10xbox::drivers

namespace ntk {
using namespace win10xbox::drivers::detail;

KIRQL NTAPI KeGetCurrentIrql(void);

namespace {
void CheckPagedAtDispatch(ULONG type) {
    // POOL_TYPE: bit 0 set = paged.
    if ((type & 1) && !(type & 0x200)) {
        KIRQL irql = CurrentIrql();
        if (irql >= DISPATCH_LEVEL) IrqlViolation("paged pool allocation at DISPATCH_LEVEL or above", irql, APC_LEVEL);
    }
}
}  // namespace

// ---- pool ----
PVOID NTAPI ExAllocatePoolWithTag(POOL_TYPE type, SIZE_T bytes, ULONG tag) {
    CheckPagedAtDispatch(static_cast<ULONG>(type));
    return PoolAlloc(bytes, tag);
}
PVOID NTAPI ExAllocatePool(POOL_TYPE type, SIZE_T bytes) {
    return ExAllocatePoolWithTag(type, bytes, NTK_TAG('N', 'o', 'n', 'e'));
}
PVOID NTAPI ExAllocatePoolWithQuotaTag(POOL_TYPE type, SIZE_T bytes, ULONG tag) { return ExAllocatePoolWithTag(type, bytes, tag); }
PVOID NTAPI ExAllocatePoolWithTagPriority(POOL_TYPE type, SIZE_T bytes, ULONG tag, int priority) {
    (void)priority;
    return ExAllocatePoolWithTag(type, bytes, tag);
}
PVOID NTAPI ExAllocatePool2(POOL_FLAGS flags, SIZE_T bytes, ULONG tag) {
    if (flags & POOL_FLAG_PAGED) CheckPagedAtDispatch(1);
    return PoolAlloc(bytes, tag);
}
PVOID NTAPI ExAllocatePoolZero(POOL_TYPE type, SIZE_T bytes, ULONG tag) { return ExAllocatePoolWithTag(type, bytes, tag); }
PVOID NTAPI ExAllocatePoolUninitialized(POOL_TYPE type, SIZE_T bytes, ULONG tag) { return ExAllocatePoolWithTag(type, bytes, tag); }
VOID NTAPI ExFreePoolWithTag(PVOID p, ULONG tag) { PoolFree(p, tag, true); }
VOID NTAPI ExFreePool(PVOID p) { PoolFree(p, 0, false); }

// ---- ERESOURCE ----
namespace {
struct ResImpl {
    DWORD excl_tid = 0;
    LONG excl_count = 0;
    std::unordered_map<DWORD, LONG> shared;
    LONG shared_total = 0;
};
ResImpl* Impl(PERESOURCE r) { return static_cast<ResImpl*>(r->OwnerTable); }
DWORD ThreadIdOf(PVOID resource_thread) {
    PseudoThread* t = static_cast<PseudoThread*>(resource_thread);
    return t ? t->tid : GetCurrentThreadId();
}
}  // namespace

NTSTATUS NTAPI ExInitializeResourceLite(PERESOURCE r) {
    std::memset(r, 0, sizeof(*r));
    r->OwnerTable = new ResImpl;
    InitializeListHead(&r->SystemResourcesList);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI ExReinitializeResourceLite(PERESOURCE r) {
    if (r->OwnerTable) delete Impl(r);
    return ExInitializeResourceLite(r);
}
NTSTATUS NTAPI ExDeleteResourceLite(PERESOURCE r) {
    delete Impl(r);
    r->OwnerTable = nullptr;
    return STATUS_SUCCESS;
}
BOOLEAN NTAPI ExAcquireResourceExclusiveLite(PERESOURCE r, BOOLEAN wait) {
    ResImpl* im = Impl(r);
    const DWORD me = GetCurrentThreadId();
    AcquireSRWLockExclusive(&g_disp_lock);
    for (;;) {
        if (im->excl_tid == me) { ++im->excl_count; break; }
        if (im->excl_tid == 0 && im->shared_total == 0) { im->excl_tid = me; im->excl_count = 1; break; }
        if (!wait) { ReleaseSRWLockExclusive(&g_disp_lock); return FALSE; }
        SleepConditionVariableSRW(&g_disp_cv, &g_disp_lock, INFINITE, 0);
    }
    r->ActiveCount = static_cast<SHORT>(im->excl_count + im->shared_total);
    ReleaseSRWLockExclusive(&g_disp_lock);
    return TRUE;
}
BOOLEAN NTAPI ExTryToAcquireResourceExclusiveLite(PERESOURCE r) { return ExAcquireResourceExclusiveLite(r, FALSE); }
BOOLEAN NTAPI ExAcquireResourceSharedLite(PERESOURCE r, BOOLEAN wait) {
    ResImpl* im = Impl(r);
    const DWORD me = GetCurrentThreadId();
    AcquireSRWLockExclusive(&g_disp_lock);
    for (;;) {
        if (im->excl_tid == me) { ++im->excl_count; break; }   // exclusive owner may re-enter shared
        if (im->excl_tid == 0) { ++im->shared[me]; ++im->shared_total; break; }
        if (!wait) { ReleaseSRWLockExclusive(&g_disp_lock); return FALSE; }
        SleepConditionVariableSRW(&g_disp_cv, &g_disp_lock, INFINITE, 0);
    }
    r->ActiveCount = static_cast<SHORT>(im->excl_count + im->shared_total);
    ReleaseSRWLockExclusive(&g_disp_lock);
    return TRUE;
}
BOOLEAN NTAPI ExAcquireSharedStarveExclusive(PERESOURCE r, BOOLEAN wait) { return ExAcquireResourceSharedLite(r, wait); }
BOOLEAN NTAPI ExAcquireSharedWaitForExclusive(PERESOURCE r, BOOLEAN wait) { return ExAcquireResourceSharedLite(r, wait); }
VOID NTAPI ExReleaseResourceForThreadLite(PERESOURCE r, PVOID resource_thread) {
    ResImpl* im = Impl(r);
    const DWORD tid = ThreadIdOf(resource_thread);
    AcquireSRWLockExclusive(&g_disp_lock);
    if (im->excl_tid == tid) {
        if (--im->excl_count == 0) im->excl_tid = 0;
    } else {
        auto it = im->shared.find(tid);
        if (it == im->shared.end()) {
            ERR(driver, "ExReleaseResourceLite: thread %lu does not hold %p", tid, static_cast<void*>(r));
        } else {
            if (--it->second == 0) im->shared.erase(it);
            --im->shared_total;
        }
    }
    r->ActiveCount = static_cast<SHORT>(im->excl_count + im->shared_total);
    ReleaseSRWLockExclusive(&g_disp_lock);
    DispSignalAll();
}
VOID NTAPI ExReleaseResourceLite(PERESOURCE r) { ExReleaseResourceForThreadLite(r, nullptr); }
VOID NTAPI ExConvertExclusiveToSharedLite(PERESOURCE r) {
    ResImpl* im = Impl(r);
    const DWORD me = GetCurrentThreadId();
    AcquireSRWLockExclusive(&g_disp_lock);
    if (im->excl_tid == me) {
        im->shared[me] += im->excl_count;
        im->shared_total += im->excl_count;
        im->excl_tid = 0;
        im->excl_count = 0;
    }
    ReleaseSRWLockExclusive(&g_disp_lock);
    DispSignalAll();
}
BOOLEAN NTAPI ExIsResourceAcquiredExclusiveLite(PERESOURCE r) { return Impl(r)->excl_tid == GetCurrentThreadId(); }
ULONG NTAPI ExIsResourceAcquiredSharedLite(PERESOURCE r) {
    ResImpl* im = Impl(r);
    const DWORD me = GetCurrentThreadId();
    if (im->excl_tid == me) return static_cast<ULONG>(im->excl_count);
    auto it = im->shared.find(me);
    return it == im->shared.end() ? 0 : static_cast<ULONG>(it->second);
}
ULONG NTAPI ExGetExclusiveWaiterCount(PERESOURCE) { return 0; }
ULONG NTAPI ExGetSharedWaiterCount(PERESOURCE) { return 0; }
PVOID NTAPI ExGetCurrentResourceThread(void) { return CurrentPseudoThread(); }

// ---- lookaside lists ----
namespace {
PVOID NTAPI DefaultLookasideAlloc(int type, SIZE_T bytes, ULONG tag) { return ExAllocatePoolWithTag(static_cast<POOL_TYPE>(type), bytes, tag); }
VOID NTAPI DefaultLookasideFree(PVOID p) { ExFreePool(p); }
void InitLookaside(PGENERAL_LOOKASIDE l, PALLOCATE_FUNCTION alloc, PFREE_FUNCTION free_fn, ULONG flags, SIZE_T size,
                   ULONG tag, USHORT depth, int type) {
    (void)flags; (void)depth;
    std::memset(l, 0, sizeof(*l));
    l->Size = static_cast<ULONG>(size);
    l->Tag = tag;
    l->Type = type;
    // Depth 0: inline Allocate/FreeTo...LookasideList helpers always call the
    // allocation functions below instead of caching in ListHead.
    l->Depth = 0;
    l->MaximumDepth = 0;
    l->Allocate = alloc ? alloc : &DefaultLookasideAlloc;
    l->Free = free_fn ? free_fn : &DefaultLookasideFree;
    InitializeListHead(&l->ListEntry);
}
}  // namespace
VOID NTAPI ExInitializeNPagedLookasideList(PNPAGED_LOOKASIDE_LIST l, PALLOCATE_FUNCTION alloc, PFREE_FUNCTION free_fn,
                                           ULONG flags, SIZE_T size, ULONG tag, USHORT depth) {
    InitLookaside(&l->L, alloc, free_fn, flags, size, tag, depth, NonPagedPoolNx);
}
VOID NTAPI ExInitializePagedLookasideList(PPAGED_LOOKASIDE_LIST l, PALLOCATE_FUNCTION alloc, PFREE_FUNCTION free_fn,
                                          ULONG flags, SIZE_T size, ULONG tag, USHORT depth) {
    InitLookaside(&l->L, alloc, free_fn, flags, size, tag, depth, PagedPool);
}
VOID NTAPI ExDeleteNPagedLookasideList(PNPAGED_LOOKASIDE_LIST l) { (void)l; }
VOID NTAPI ExDeletePagedLookasideList(PPAGED_LOOKASIDE_LIST l) { (void)l; }

// ---- work items ----
VOID NTAPI ExQueueWorkItem(PWORK_QUEUE_ITEM item, WORK_QUEUE_TYPE type) {
    (void)type;
    WorkerPost([item] { item->WorkerRoutine(item->Parameter); });
}

namespace {
struct IoWorkItem {
    PDEVICE_OBJECT dev;
    PIO_WORKITEM_ROUTINE routine;
    PVOID routine_ex;
    PVOID ctx;
};
}  // namespace
PIO_WORKITEM NTAPI IoAllocateWorkItem(PDEVICE_OBJECT dev) {
    auto* w = static_cast<IoWorkItem*>(PoolAlloc(sizeof(IoWorkItem), NTK_TAG('N', 'W', 'k', 'I')));
    if (w) w->dev = dev;
    return reinterpret_cast<PIO_WORKITEM>(w);
}
ULONG NTAPI IoSizeofWorkItem(void) { return sizeof(IoWorkItem); }
VOID NTAPI IoInitializeWorkItem(PDEVICE_OBJECT dev, PIO_WORKITEM item) {
    auto* w = reinterpret_cast<IoWorkItem*>(item);
    std::memset(w, 0, sizeof(*w));
    w->dev = dev;
}
VOID NTAPI IoUninitializeWorkItem(PIO_WORKITEM) {}
VOID NTAPI IoFreeWorkItem(PIO_WORKITEM item) { PoolFree(item, 0, false); }
VOID NTAPI IoQueueWorkItem(PIO_WORKITEM item, PIO_WORKITEM_ROUTINE routine, WORK_QUEUE_TYPE type, PVOID ctx) {
    (void)type;
    auto* w = reinterpret_cast<IoWorkItem*>(item);
    w->routine = routine;
    w->routine_ex = nullptr;
    w->ctx = ctx;
    PDEVICE_OBJECT dev = w->dev;
    if (dev) ObjRef(dev);
    WorkerPost([dev, routine, ctx] {
        routine(dev, ctx);
        if (dev) ObjDeref(dev);
    });
}
VOID NTAPI IoQueueWorkItemEx(PIO_WORKITEM item,
                             VOID (NTAPI *routine)(PVOID io_object, PVOID ctx, PIO_WORKITEM item), WORK_QUEUE_TYPE type,
                             PVOID ctx) {
    (void)type;
    auto* w = reinterpret_cast<IoWorkItem*>(item);
    PDEVICE_OBJECT dev = w->dev;
    if (dev) ObjRef(dev);
    WorkerPost([item, dev, routine, ctx] {
        routine(dev, ctx, item);
        if (dev) ObjDeref(dev);
    });
}

// ---- interlocked list helpers (spin lock guarded, like the originals) ----
PLIST_ENTRY NTAPI ExInterlockedInsertHeadList(PLIST_ENTRY head, PLIST_ENTRY entry, PKSPIN_LOCK lock) {
    KIRQL old = SpinAcquireRaise(lock, DISPATCH_LEVEL);
    PLIST_ENTRY first = IsListEmpty(head) ? nullptr : head->Flink;
    InsertHeadList(head, entry);
    SpinReleaseLower(lock, old);
    return first;
}
PLIST_ENTRY NTAPI ExInterlockedInsertTailList(PLIST_ENTRY head, PLIST_ENTRY entry, PKSPIN_LOCK lock) {
    KIRQL old = SpinAcquireRaise(lock, DISPATCH_LEVEL);
    PLIST_ENTRY last = IsListEmpty(head) ? nullptr : head->Blink;
    InsertTailList(head, entry);
    SpinReleaseLower(lock, old);
    return last;
}
PLIST_ENTRY NTAPI ExInterlockedRemoveHeadList(PLIST_ENTRY head, PKSPIN_LOCK lock) {
    KIRQL old = SpinAcquireRaise(lock, DISPATCH_LEVEL);
    PLIST_ENTRY e = IsListEmpty(head) ? nullptr : RemoveHeadList(head);
    SpinReleaseLower(lock, old);
    return e;
}
PSINGLE_LIST_ENTRY NTAPI ExInterlockedPopEntryList(PSINGLE_LIST_ENTRY head, PKSPIN_LOCK lock) {
    KIRQL old = SpinAcquireRaise(lock, DISPATCH_LEVEL);
    PSINGLE_LIST_ENTRY e = PopEntryList(head);
    SpinReleaseLower(lock, old);
    return e;
}
PSINGLE_LIST_ENTRY NTAPI ExInterlockedPushEntryList(PSINGLE_LIST_ENTRY head, PSINGLE_LIST_ENTRY entry, PKSPIN_LOCK lock) {
    KIRQL old = SpinAcquireRaise(lock, DISPATCH_LEVEL);
    PSINGLE_LIST_ENTRY first = head->Next;
    PushEntryList(head, entry);
    SpinReleaseLower(lock, old);
    return first;
}
ULONG NTAPI ExInterlockedAddUlong(PULONG addend, ULONG value, PKSPIN_LOCK lock) {
    (void)lock;
    return static_cast<ULONG>(InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(addend), static_cast<LONG>(value)));
}
LARGE_INTEGER NTAPI ExInterlockedAddLargeInteger(PLARGE_INTEGER addend, LARGE_INTEGER value, PKSPIN_LOCK lock) {
    (void)lock;
    LARGE_INTEGER r;
    r.QuadPart = InterlockedExchangeAdd64(reinterpret_cast<volatile LONG64*>(&addend->QuadPart), value.QuadPart);
    return r;
}
LONGLONG NTAPI ExInterlockedCompareExchange64(volatile LONGLONG* dest, PLONGLONG exchange, PLONGLONG comparand, PKSPIN_LOCK lock) {
    (void)lock;
    return InterlockedCompareExchange64(dest, *exchange, *comparand);
}

// ---- time ----
VOID NTAPI ExSystemTimeToLocalTime(PLARGE_INTEGER system_time, PLARGE_INTEGER local_time) {
    TIME_ZONE_INFORMATION tz;
    GetTimeZoneInformation(&tz);
    local_time->QuadPart = system_time->QuadPart - static_cast<LONGLONG>(tz.Bias) * 600000000LL;
}
VOID NTAPI ExLocalTimeToSystemTime(PLARGE_INTEGER local_time, PLARGE_INTEGER system_time) {
    TIME_ZONE_INFORMATION tz;
    GetTimeZoneInformation(&tz);
    system_time->QuadPart = local_time->QuadPart + static_cast<LONGLONG>(tz.Bias) * 600000000LL;
}

KPROCESSOR_MODE NTAPI ExGetPreviousMode(void) { return PreviousMode(); }
VOID NTAPI ExRaiseAccessViolation(void) { RaiseException(STATUS_ACCESS_VIOLATION, 0, 0, nullptr); }
VOID NTAPI ExRaiseDatatypeMisalignment(void) { RaiseException(0x80000002 /* STATUS_DATATYPE_MISALIGNMENT */, 0, 0, nullptr); }
VOID NTAPI ExRaiseStatus(NTSTATUS status) { RaiseException(static_cast<DWORD>(status), 0, 0, nullptr); }
VOID NTAPI ExSetTimerResolution_unused(void) {}
BOOLEAN NTAPI ExIsProcessorFeaturePresent(ULONG feature) { return IsProcessorFeaturePresent(feature) ? TRUE : FALSE; }
ULONG NTAPI ExIsProcessorFeaturePresent_dummy(void) { return 0; }

}  // namespace ntk

namespace win10xbox::drivers::detail {
namespace {
const Export kExTable[] = {
#define X(n) {#n, reinterpret_cast<void*>(&ntk::n)}
    X(ExAllocatePoolWithTag), X(ExAllocatePool), X(ExAllocatePoolWithQuotaTag), X(ExAllocatePoolWithTagPriority),
    X(ExAllocatePool2), X(ExAllocatePoolZero), X(ExAllocatePoolUninitialized), X(ExFreePoolWithTag), X(ExFreePool),
    X(ExInitializeResourceLite), X(ExReinitializeResourceLite), X(ExDeleteResourceLite),
    X(ExAcquireResourceExclusiveLite), X(ExTryToAcquireResourceExclusiveLite), X(ExAcquireResourceSharedLite),
    X(ExAcquireSharedStarveExclusive), X(ExAcquireSharedWaitForExclusive), X(ExReleaseResourceForThreadLite),
    X(ExReleaseResourceLite), X(ExConvertExclusiveToSharedLite), X(ExIsResourceAcquiredExclusiveLite),
    X(ExIsResourceAcquiredSharedLite), X(ExGetExclusiveWaiterCount), X(ExGetSharedWaiterCount),
    X(ExGetCurrentResourceThread),
    X(ExInitializeNPagedLookasideList), X(ExInitializePagedLookasideList), X(ExDeleteNPagedLookasideList),
    X(ExDeletePagedLookasideList),
    X(ExQueueWorkItem), X(IoAllocateWorkItem), X(IoSizeofWorkItem), X(IoInitializeWorkItem), X(IoUninitializeWorkItem),
    X(IoFreeWorkItem), X(IoQueueWorkItem), X(IoQueueWorkItemEx),
    X(ExInterlockedInsertHeadList), X(ExInterlockedInsertTailList), X(ExInterlockedRemoveHeadList),
    X(ExInterlockedPopEntryList), X(ExInterlockedPushEntryList), X(ExInterlockedAddUlong),
    X(ExInterlockedAddLargeInteger), X(ExInterlockedCompareExchange64),
    X(ExSystemTimeToLocalTime), X(ExLocalTimeToSystemTime), X(ExGetPreviousMode), X(ExRaiseAccessViolation),
    X(ExRaiseDatatypeMisalignment), X(ExRaiseStatus), X(ExIsProcessorFeaturePresent),
#undef X
};
}  // namespace
ExportGroup ExExports() { return {kExTable, sizeof(kExTable) / sizeof(kExTable[0])}; }
}  // namespace win10xbox::drivers::detail
