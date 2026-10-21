// Reference-counted registry of the kernel objects the host created
// (DEVICE_OBJECT, DRIVER_OBJECT, FILE_OBJECT, pseudo KTHREAD/KEVENT ...).
// Kernel objects are plain structs in pool memory, so ObReferenceObject /
// ObDereferenceObject need a side table to find the count and the destructor.
#include "../ntoskrnl/internal.h"
#include <unordered_map>

namespace win10xbox::drivers::detail {
namespace {
struct ObjRec {
    ObjType type;
    LONG refs;
    void (*destroy)(void*);
};
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<void*, ObjRec>& Map() {
    static std::unordered_map<void*, ObjRec> m;
    return m;
}
}  // namespace

void ObjRegister(void* p, ObjType t, void (*destroy)(void*), LONG refs) {
    AcquireSRWLockExclusive(&g_lock);
    Map()[p] = ObjRec{t, refs, destroy};
    ReleaseSRWLockExclusive(&g_lock);
}

bool ObjLookup(void* p, ObjType* t) {
    AcquireSRWLockShared(&g_lock);
    auto it = Map().find(p);
    bool found = it != Map().end();
    if (found && t) *t = it->second.type;
    ReleaseSRWLockShared(&g_lock);
    return found;
}

LONG ObjRef(void* p) {
    AcquireSRWLockExclusive(&g_lock);
    auto it = Map().find(p);
    LONG n = it == Map().end() ? -1 : ++it->second.refs;
    ReleaseSRWLockExclusive(&g_lock);
    return n;
}

LONG ObjDeref(void* p) {
    void (*destroy)(void*) = nullptr;
    LONG n;
    AcquireSRWLockExclusive(&g_lock);
    auto it = Map().find(p);
    if (it == Map().end()) {
        n = -1;
    } else {
        n = --it->second.refs;
        if (n <= 0) {
            destroy = it->second.destroy;
            Map().erase(it);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (n == -1) WARN(driver, "dereference of unknown object %p", p);
    if (n == 0 && destroy) destroy(p);
    return n;
}

void ObjForget(void* p) {
    AcquireSRWLockExclusive(&g_lock);
    Map().erase(p);
    ReleaseSRWLockExclusive(&g_lock);
}

void ObjClearAll() {
    AcquireSRWLockExclusive(&g_lock);
    Map().clear();
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace win10xbox::drivers::detail
