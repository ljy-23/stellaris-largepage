/* lpshim.dll v2 - large-page allocator shim for Stellaris 4.5.x
 *
 * Loaded by a CFG-safe static patch at the game's entry point.
 *   1. enables SeLockMemoryPrivilege
 *   2. reserves one big MEM_LARGE_PAGES arena (2 MB pages)
 *   3. hooks VirtualAlloc/VirtualFree AND HeapAlloc/HeapFree/HeapReAlloc/HeapSize
 *      in the IAT of EVERY loaded module (re-scanned periodically, so modules
 *      loaded later are covered too)
 *   4. serves big private RW allocations from the arena and reports exactly how
 *      many bytes flow through each API, so coverage is measurable
 *
 * lpshim.cfg (next to this DLL):
 *   ArenaGB=4          arena size in GiB
 *   ThresholdMB=1      minimum request size served from the arena
 *   HookVA=1           hook VirtualAlloc/VirtualFree
 *   HookHeap=1         hook HeapAlloc/HeapFree/HeapReAlloc/HeapSize
 *   Log=<path>
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct { SIZE_T size; DWORD free; } HDR;

typedef struct {
    HANDLE log;
    CRITICAL_SECTION cs;
    BYTE  *arena; SIZE_T arenaSize; SIZE_T threshold;
    int hookVA, hookHeap, verboseAlloc, minimal, lazyArena; DWORD startDelayMs;
    unsigned long long vaCalls, vaBytes, vaBigCalls, vaBigBytes;
    unsigned long long haCalls, haBytes, haBigCalls, haBigBytes;
    unsigned long long hfCalls, hrCalls, hsCalls;
    unsigned long long servedCalls, servedBytes, servedFail;
    unsigned long long arenaFrees, foreignFrees, activeBlocks;
    int verifiedLarge, modulesHooked; unsigned long long slotMismatch, liveBytes, peakBytes; unsigned long long liveBytes, peakBytes;
} SHIM;

typedef struct { char name[64]; SIZE_T size; } BIGGEST;
static SHIM S;
static HMODULE g_self;
static HANDLE g_instance_mutex;

/* Stellaris spawns a second copy of itself during startup.  That copy also
   loads this DLL (it is the same patched exe), so it must stay completely inert.
   Returns 1 when we are NOT the primary instance. */
static int secondary_instance(void)
{
    g_instance_mutex = CreateMutexA(NULL, TRUE, "Local\\StellarisLpShim_4x");
    if (g_instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) return 1;
    return 0;
}


typedef LPVOID (WINAPI *PFN_VA)(LPVOID, SIZE_T, DWORD, DWORD);
typedef BOOL   (WINAPI *PFN_VF)(LPVOID, SIZE_T, DWORD);
typedef LPVOID (WINAPI *PFN_HA)(HANDLE, DWORD, SIZE_T);
typedef BOOL   (WINAPI *PFN_HF)(HANDLE, DWORD, LPVOID);
typedef LPVOID (WINAPI *PFN_HR)(HANDLE, DWORD, LPVOID, SIZE_T);
typedef SIZE_T (WINAPI *PFN_HS)(HANDLE, DWORD, LPCVOID);

static PFN_VA real_VA; static PFN_VF real_VF;
static PFN_HA real_HA; static PFN_HF real_HF;
static PFN_HR real_HR; static PFN_HS real_HS;
static PFN_HA real_RtlA; static PFN_HF real_RtlF; static PFN_HR real_RtlR; static PFN_HS real_RtlS;

static void logline(const char *s)
{
    DWORD w;
    if (S.log == INVALID_HANDLE_VALUE) return;
    SetFilePointer(S.log, 0, NULL, FILE_END);
    WriteFile(S.log, s, (DWORD)lstrlenA(s), &w, NULL);
    WriteFile(S.log, "\r\n", 2, &w, NULL);
}
static void logf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt); vsprintf(buf, fmt, ap); va_end(ap);
    if (S.log != INVALID_HANDLE_VALUE) { logline(buf); FlushFileBuffers(S.log); }
}

static unsigned long long parse_u64(const char *s)
{
    unsigned long long v = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned long long)(*s - '0'); s++; }
    return v;
}
static unsigned long long cfg_u64(const char *t, const char *k, unsigned long long d)
{
    size_t kl = lstrlenA((char *)k);
    const char *p = t;
    while (p && *p) {
        if (!strncmp(p, k, kl) && p[kl] == '=') return parse_u64(p + kl + 1);
        p = strchr(p, '\n'); if (p) p++;
    }
    return d;
}
static void read_cfg(char *logPath, size_t cap)
{
    char dir[MAX_PATH], path[MAX_PATH], buf[4096];
    HANDLE h; DWORD got = 0;
    ZeroMemory(buf, sizeof(buf));
    GetModuleFileNameA(g_self, dir, MAX_PATH);
    { char *s = strrchr(dir, '\\'); if (s) *s = 0; }
    sprintf(path, "%s\\lpshim.cfg", dir);
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { ReadFile(h, buf, sizeof(buf) - 1, &got, NULL); buf[got] = 0; CloseHandle(h); }
    S.arenaSize = cfg_u64(buf, "ArenaGB", 4) * 1024ULL * 1024ULL * 1024ULL;
    { unsigned long long kb = cfg_u64(buf, "ThresholdKB", 0xFFFFFFFFFFFFFFFFULL); if (kb != 0xFFFFFFFFFFFFFFFFULL) S.threshold = kb * 1024ULL; else S.threshold = cfg_u64(buf, "ThresholdMB", 1) * 1024ULL * 1024ULL; }
    S.hookVA    = (int)cfg_u64(buf, "HookVA", 1);
    S.hookHeap  = (int)cfg_u64(buf, "HookHeap", 1);
    S.verboseAlloc = (int)cfg_u64(buf, "VerboseAlloc", 0);
    S.startDelayMs = (DWORD)cfg_u64(buf, "StartDelayMs", 0);
    S.minimal = (int)cfg_u64(buf, "Minimal", 0);
    S.lazyArena = (int)cfg_u64(buf, "LazyArena", 1);
    { char *p = strstr(buf, "Log=");
      if (p) { char *q = p + 4, *r = logPath; int n = 0;
               while (*q && *q != '\r' && *q != '\n' && n < 400) { *r++ = *q++; n++; } *r = 0; }
      else sprintf(logPath, "%s\\lpshim-log.txt", dir); }
}

static void enable_lock_pages(void)
{
    HANDLE tok; TOKEN_PRIVILEGES tp; LUID luid;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        logf("priv: OpenProcessToken failed %lu", GetLastError()); return;
    }
    if (!LookupPrivilegeValueA(NULL, "SeLockMemoryPrivilege", &luid)) {
        logf("priv: LookupPrivilegeValue failed %lu", GetLastError()); CloseHandle(tok); return;
    }
    tp.PrivilegeCount = 1; tp.Privileges[0].Luid = luid; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL) || GetLastError() == ERROR_NOT_ALL_ASSIGNED)
        logf("priv: SeLockMemoryPrivilege NOT assigned (%lu)", GetLastError());
    else logf("priv: SeLockMemoryPrivilege enabled");
    CloseHandle(tok);
}

typedef struct { PVOID addr; ULONG_PTR attrs; } WSXIN;

static void verify_large_page(void *p, const char *where)
{
    typedef BOOL (WINAPI *PFN_QWSX)(HANDLE, PVOID, DWORD);
    static PFN_QWSX qwsx;
    WSXIN in;
    unsigned long long v;
    if (!qwsx) qwsx = (PFN_QWSX)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32QueryWorkingSetEx");
    if (!qwsx || !p) return;
    *(volatile char *)p = 1;                 /* fault it in (p is inside block data, never the header) */
    in.addr = p; in.attrs = 0;
    if (qwsx(GetCurrentProcess(), &in, (DWORD)sizeof(in))) {
        v = (unsigned long long)in.attrs;
        logf("verify: %s at %p -> wss 0x%016llX (Valid=%d LargePage=%d)",
             where, p, v, (int)(v & 1), (int)((v >> 23) & 1));
        if ((v >> 23) & 1) S.verifiedLarge = 1;
    } else {
        logf("verify: K32QueryWorkingSetEx failed %lu", GetLastError());
    }
}

/* ---------------- arena ---------------- */
#define ARENA_ALIGN 0x10000UL

static void arena_init(void)
{
    if (!S.arenaSize) return;
    SetLastError(0);
    S.arena = (BYTE *)VirtualAlloc(NULL, S.arenaSize, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE);
    if (!S.arena) {
        logf("arena: MEM_LARGE_PAGES %llu MB failed (%lu) - trying normal pages",
             (unsigned long long)(S.arenaSize / 1048576), GetLastError());
        S.arena = (BYTE *)VirtualAlloc(NULL, S.arenaSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!S.arena) { logf("arena: normal reserve failed too (%lu) -> pass-through", GetLastError()); return; }
    } else {
        logf("arena: RESERVED %llu MB with MEM_LARGE_PAGES at %p (2MB-aligned=%d)",
             (unsigned long long)(S.arenaSize / 1048576), S.arena,
             (int)(((ULONG_PTR)S.arena & 0x1FFFFF) == 0));
    }
    { HDR *h = (HDR *)S.arena; h->size = S.arenaSize; h->free = 1; }
    verify_large_page(S.arena + sizeof(HDR), "arena data");
}

static void *arena_alloc(SIZE_T n)
{
    HDR *h, *blk = NULL;
    SIZE_T need = (n + sizeof(HDR) + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1);
    EnterCriticalSection(&S.cs);
    for (h = (HDR *)S.arena; (BYTE *)h < S.arena + S.arenaSize; h = (HDR *)((BYTE *)h + h->size))
        if (h->free && h->size >= need) { blk = h; break; }
    if (blk) {
        if (blk->size >= need + sizeof(HDR) + ARENA_ALIGN) {
            HDR *rest = (HDR *)((BYTE *)blk + need);
            rest->size = blk->size - need; rest->free = 1; blk->size = need;
        }
        blk->free = 0; S.activeBlocks++; S.liveBytes += blk->size - sizeof(HDR);
        if (S.liveBytes > S.peakBytes) S.peakBytes = S.liveBytes; S.liveBytes += blk->size - sizeof(HDR);
    }
    if (S.liveBytes > S.peakBytes) S.peakBytes = S.liveBytes;
    LeaveCriticalSection(&S.cs);
    return blk ? (void *)((BYTE *)blk + sizeof(HDR)) : NULL;
}
static int arena_owns(void *p)
{
    return S.arena && (BYTE *)p > S.arena && (BYTE *)p < S.arena + S.arenaSize;
}
static SIZE_T arena_size_of(void *p)
{
    HDR *h = (HDR *)((BYTE *)p - sizeof(HDR));
    if ((BYTE *)h < S.arena || (BYTE *)h >= S.arena + S.arenaSize) return 0;
    if (h->free) return 0;
    return h->size - sizeof(HDR);
}
static int arena_free(void *p)
{
    HDR *h = (HDR *)((BYTE *)p - sizeof(HDR)), *prv = NULL, *q;
    EnterCriticalSection(&S.cs);
    if (h->free || (BYTE *)h < S.arena || (BYTE *)h >= S.arena + S.arenaSize) {
        LeaveCriticalSection(&S.cs); return 0;
    }
    { SIZE_T sz = h->size - sizeof(HDR); if (S.liveBytes >= sz) S.liveBytes -= sz; else S.liveBytes = 0; }
    h->free = 1; S.activeBlocks--; if (S.liveBytes >= h->size - sizeof(HDR)) S.liveBytes -= h->size - sizeof(HDR); else S.liveBytes = 0;
    q = (HDR *)((BYTE *)h + h->size);
    if ((BYTE *)q < S.arena + S.arenaSize && q->free) h->size += q->size;
    for (q = (HDR *)S.arena; (BYTE *)q < (BYTE *)h; q = (HDR *)((BYTE *)q + q->size)) prv = q;
    if (prv && prv->free) prv->size += h->size;
    LeaveCriticalSection(&S.cs);
    return 1;
}

static char *where_is(void *addr, char *nm, int cap);
static void hook_all_modules(void);
static DWORD g_lastScan, g_lastStats; static unsigned long long g_tick;
static void lazy_maintenance(void)
{
    DWORD now = GetTickCount();
    if (!g_lastScan) { g_lastScan = now; g_lastStats = now; return; }
    if (now - g_lastScan > 3000) { g_lastScan = now; hook_all_modules(); }
    if (now - g_lastStats > 15000) {
        char b[400];
        g_lastStats = now;
        sprintf(b, "stats: VA=%llu(%.1fMB big %llu/%.1fMB) HeapAlloc=%llu(%.1fMB big %llu/%.1fMB) | "
                   "arenaServed=%llu(%.1fMB) fail=%llu LIVE=%.1fMB peak=%.1fMB frees=%llu",
                S.vaCalls, S.vaBytes / 1048576.0, S.vaBigCalls, S.vaBigBytes / 1048576.0,
                S.haCalls, S.haBytes / 1048576.0, S.haBigCalls, S.haBigBytes / 1048576.0,
                S.servedCalls, S.servedBytes / 1048576.0, S.servedFail,
                S.liveBytes / 1048576.0, S.peakBytes / 1048576.0, S.hfCalls);
        logf("%s", b);
    }
}

static void ensure_arena(void)
{
    if (!S.arena && S.lazyArena && S.arenaSize) {
        logf("lpshim: lazy arena reservation triggered");
        arena_init();
    }
}
/* ---------------- shims ---------------- */
static LPVOID WINAPI shim_VA(LPVOID addr, SIZE_T size, DWORD type, DWORD prot)
{
    if ((++g_tick & 0x1FFF) == 0) lazy_maintenance();
    int big = (!addr && size >= S.threshold &&
               (type & (MEM_COMMIT | MEM_RESERVE)) == (MEM_COMMIT | MEM_RESERVE) &&
               (prot == PAGE_READWRITE || prot == PAGE_READONLY) && !(type & MEM_LARGE_PAGES));
    S.vaCalls++; S.vaBytes += size;
    if (size >= S.threshold) { S.vaBigCalls++; S.vaBigBytes += size; }
    if (big && S.arena) {
        void *p = arena_alloc(size);
        if (p) { S.servedCalls++; S.servedBytes += size; return p; }
        S.servedFail++;
    }
    return real_VA(addr, size, type, prot);
}
static BOOL WINAPI shim_VF(LPVOID addr, SIZE_T size, DWORD type)
{
    if (addr && arena_owns(addr)) { S.arenaFrees++; return arena_free(addr) ? TRUE : real_VF(addr, size, type); }
    S.foreignFrees++;
    return real_VF(addr, size, type);
}
static LPVOID WINAPI shim_HA(HANDLE heap, DWORD flags, SIZE_T size)
{
    if ((++g_tick & 0x1FFF) == 0) lazy_maintenance();
    ensure_arena();
    S.haCalls++; S.haBytes += size;
    if (size >= S.threshold) { S.haBigCalls++; S.haBigBytes += size; }
    if (S.arena && heap == GetProcessHeap() && size >= S.threshold && !(flags & HEAP_ZERO_MEMORY)) {
        void *p = arena_alloc(size);
        if (p) {
            char nm[64];
            S.servedCalls++; S.servedBytes += size;
            if (S.verboseAlloc)
                logf("alloc: %llu KB -> %p  caller=%s", (unsigned long long)(size / 1024), p,
                     where_is(__builtin_return_address(0), nm, sizeof(nm)));
            return p;
        }
        S.servedFail++;
    }
    return real_HA(heap, flags, size);
}
static BOOL WINAPI shim_HF(HANDLE heap, DWORD flags, LPVOID p)
{
    S.hfCalls++;
    if (p && arena_owns(p)) {
        char nm[64];
        S.arenaFrees++;
        if (S.verboseAlloc)
            logf("free: %p  by %s", p, where_is(__builtin_return_address(0), nm, sizeof(nm)));
        return arena_free(p) ? TRUE : real_HF(heap, flags, p);
    }
    S.foreignFrees++;
    return real_HF(heap, flags, p);
}
static LPVOID WINAPI shim_HR(HANDLE heap, DWORD flags, LPVOID p, SIZE_T size)
{
    S.hrCalls++;
    if (p && arena_owns(p)) {
        SIZE_T old = arena_size_of(p);
        void *n = (size <= old) ? p : arena_alloc(size);
        if (n == p) return p;
        if (n) { if (old) memcpy(n, p, old < size ? old : size); arena_free(p);
                 S.servedCalls++; S.servedBytes += size; return n; }
    }
    return real_HR(heap, flags, p, size);
}
static SIZE_T WINAPI shim_HS(HANDLE heap, DWORD flags, LPCVOID p)
{
    S.hsCalls++;
    if (p && arena_owns(p)) { SIZE_T z = arena_size_of((void *)p); if (z) return z; }
    return real_HS(heap, flags, p);
}

static LPVOID WINAPI shim_RtlA(HANDLE heap, DWORD flags, SIZE_T size)
{
    if ((++g_tick & 0x1FFF) == 0) lazy_maintenance();
    ensure_arena();
    S.haCalls++; S.haBytes += size;
    if (size >= S.threshold) { S.haBigCalls++; S.haBigBytes += size; }
    if (S.arena && heap == GetProcessHeap() && size >= S.threshold && !(flags & HEAP_ZERO_MEMORY)) {
        void *p = arena_alloc(size);
        if (p) {
            char nm[64];
            S.servedCalls++; S.servedBytes += size;
            if (S.verboseAlloc)
                logf("alloc(Rtl): %llu KB -> %p  caller=%s", (unsigned long long)(size / 1024), p,
                     where_is(__builtin_return_address(0), nm, sizeof(nm)));
            return p;
        }
        S.servedFail++;
    }
    return real_RtlA(heap, flags, size);
}
static BOOL WINAPI shim_RtlF(HANDLE heap, DWORD flags, LPVOID p)
{
    S.hfCalls++;
    if (p && arena_owns(p)) { S.arenaFrees++; return arena_free(p) ? TRUE : real_RtlF(heap, flags, p); }
    S.foreignFrees++;
    return real_RtlF(heap, flags, p);
}
static LPVOID WINAPI shim_RtlR(HANDLE heap, DWORD flags, LPVOID p, SIZE_T size)
{
    S.hrCalls++;
    if (p && arena_owns(p)) {
        SIZE_T old = arena_size_of(p);
        void *n = (size <= old) ? p : arena_alloc(size);
        if (n == p) return p;
        if (n) { if (old) memcpy(n, p, old < size ? old : size); arena_free(p);
                 S.servedCalls++; S.servedBytes += size; return n; }
    }
    return real_RtlR(heap, flags, p, size);
}
static SIZE_T WINAPI shim_RtlS(HANDLE heap, DWORD flags, LPCVOID p)
{
    S.hsCalls++;
    if (p && arena_owns(p)) { SIZE_T z = arena_size_of((void *)p); if (z) return z; }
    return real_RtlS(heap, flags, p);
}

/* ---------------- module / import-table walking ---------------- */
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } USTR;
typedef struct _LE { struct _LE *Flink, *Blink; } LE;
typedef struct {
    LE InLoadOrderLinks; LE InMemoryOrderLinks; LE InInitializationOrderLinks;
    PVOID DllBase; PVOID EntryPoint; ULONG SizeOfImage; ULONG pad;
    USTR FullDllName; USTR BaseDllName;
} LDRENTRY;
typedef struct { ULONG Length; ULONG Initialized; PVOID SsHandle;
                 LE InLoadOrderModuleList; LE InMemoryOrderModuleList; LE InInitOrderModuleList; } PEBLDR;
typedef struct { BYTE Reserved[0x18]; PEBLDR *Ldr; } PEBT;
typedef struct { LONG ExitStatus; ULONG pad; PVOID PebBaseAddress; ULONG_PTR AffinityMask;
                 LONG BasePriority; ULONG pad2; ULONG_PTR UniqueProcessId; ULONG_PTR Inherited; } PBI;
typedef LONG (WINAPI *PFN_NtQIP)(HANDLE, ULONG, PVOID, ULONG, PULONG);

typedef struct { DWORD OFT, TimeDateStamp, ForwarderChain, Name, FT; } MY_IMPDESC;
typedef struct { union { ULONGLONG AddressOfData; ULONGLONG Ordinal; } u1; } THUNK64;
typedef struct { WORD Hint; CHAR Name[1]; } IBN;

static const char *TARGETS[] = { "VirtualAlloc", "VirtualFree", "HeapAlloc", "HeapFree", "HeapReAlloc", "HeapSize" };
#define NTARGETS 6

static void *slot_target(const char *fn)
{
    if (!strcmp(fn, "VirtualAlloc")) return (void *)shim_VA;
    if (!strcmp(fn, "VirtualFree"))  return (void *)shim_VF;
    if (!strcmp(fn, "HeapAlloc"))    return (void *)shim_HA;
    if (!strcmp(fn, "HeapFree"))     return (void *)shim_HF;
    if (!strcmp(fn, "HeapReAlloc"))  return (void *)shim_HR;
    if (!strcmp(fn, "HeapSize"))     return (void *)shim_HS;
    if (!strcmp(fn, "RtlAllocateHeap"))  return (void *)shim_RtlA;
    if (!strcmp(fn, "RtlFreeHeap"))      return (void *)shim_RtlF;
    if (!strcmp(fn, "RtlReAllocateHeap"))return (void *)shim_RtlR;
    if (!strcmp(fn, "RtlSizeHeap"))      return (void *)shim_RtlS;
    return NULL;
}
static int enabled_fn(const char *fn)
{
    if (!strncmp(fn, "Virtual", 7)) return S.hookVA;
    return S.hookHeap;
}

static int hook_module(BYTE *base, const char *modname)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_DATA_DIRECTORY imp;
    MY_IMPDESC *d;
    int hits = 0;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return 0;
    imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imp.Size) return 0;
    for (d = (MY_IMPDESC *)(base + imp.VirtualAddress); d->Name; d++) {
        const char *dll = (const char *)(base + d->Name);
        THUNK64 *oft, *ft;
        if (lstrcmpiA((char *)dll, "KERNEL32.dll") && lstrcmpiA((char *)dll, "KERNELBASE.dll") &&
            lstrcmpiA((char *)dll, "ntdll.dll")) continue;
        oft = (THUNK64 *)(base + (d->OFT ? d->OFT : d->FT));
        ft = (THUNK64 *)(base + d->FT);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            char *fn;
            void *want, *cur;
            DWORD old;
            int i;
            if (oft->u1.Ordinal & 0x8000000000000000ULL) continue;
            fn = (char *)(base + oft->u1.AddressOfData + 2);
            for (i = 0; i < NTARGETS; i++) {
                if (strcmp(fn, TARGETS[i])) continue;
                if (!enabled_fn(fn)) break;
                want = slot_target(fn);
                cur = (void *)ft->u1.AddressOfData;
                if (cur == want) break;                    /* already hooked */
                HMODULE expmod = GetModuleHandleA(dll);
                if (!expmod || cur != (void *)GetProcAddress(expmod, fn)) {
                    S.slotMismatch++;
                    break;                                 /* not the real API -> leave it alone */
                }
                if (!VirtualProtect(&ft->u1.AddressOfData, sizeof(void *), PAGE_READWRITE, &old)) break;
                ft->u1.AddressOfData = (ULONGLONG)want;
                VirtualProtect(&ft->u1.AddressOfData, sizeof(void *), old, &old);
                hits++;
                break;
            }
        }
    }
    if (hits) logf("hook: %s -> %d IAT slots", modname, hits);
    return hits;
}

static void hook_all_modules(void)
{
    PFN_NtQIP pNtQIP = (PFN_NtQIP)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    PBI pbi; PEBT *peb; PEBLDR *ldr; LE *head, *cur;
    if (!pNtQIP) return;
    ZeroMemory(&pbi, sizeof(pbi));
    if (pNtQIP(GetCurrentProcess(), 0, &pbi, sizeof(pbi), NULL) < 0) return;
    peb = (PEBT *)pbi.PebBaseAddress;
    ldr = peb->Ldr;
    head = &ldr->InLoadOrderModuleList;
    for (cur = head->Flink; cur != head; cur = cur->Flink) {
        LDRENTRY *e = (LDRENTRY *)cur;
        char name[64]; int n = e->BaseDllName.Length / 2, i;
        if (n > 60) n = 60;
        if (n < 0) n = 0;
        for (i = 0; i < n; i++) name[i] = (char)(e->BaseDllName.Buffer[i] & 0x7F);
        name[n] = 0;
        S.modulesHooked += hook_module((BYTE *)e->DllBase, name);
    }
}

/* ---------- diagnostics: module lookup, backtrace, VEH ---------- */
static int find_module(void *addr, char *name, int cap, unsigned long long *rva)
{
    PFN_NtQIP p = (PFN_NtQIP)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    PBI pbi; PEBT *peb; LE *head, *cur;
    if (!p) return 0;
    ZeroMemory(&pbi, sizeof(pbi));
    if (p(GetCurrentProcess(), 0, &pbi, sizeof(pbi), NULL) < 0) return 0;
    peb = (PEBT *)pbi.PebBaseAddress;
    head = &peb->Ldr->InLoadOrderModuleList;
    for (cur = head->Flink; cur != head; cur = cur->Flink) {
        LDRENTRY *e = (LDRENTRY *)cur;
        BYTE *b = (BYTE *)e->DllBase;
        if ((BYTE *)addr >= b && (BYTE *)addr < b + e->SizeOfImage) {
            int n = e->BaseDllName.Length / 2, i;
            if (n > cap - 1) n = cap - 1;
            if (n < 0) n = 0;
            for (i = 0; i < n; i++) name[i] = (char)(e->BaseDllName.Buffer[i] & 0x7F);
            name[n] = 0;
            *rva = (unsigned long long)((BYTE *)addr - b);
            return 1;
        }
    }
    return 0;
}

static char *where_is(void *addr, char *nm, int cap)
{
    static char out[112];
    unsigned long long rva = 0;
    nm[0] = 0;
    if (!find_module(addr, nm, cap, &rva)) sprintf(out, "%p (unknown)", addr);
    else sprintf(out, "%s+0x%llX", nm, rva);
    return out;
}

static volatile LONG g_inveh;

static LONG WINAPI veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 && !g_inveh) {
        g_inveh = 1;
        {
            void *ip  = (void *)ep->ExceptionRecord->ExceptionAddress;
            void *bad = (void *)ep->ExceptionRecord->ExceptionInformation[1];
            unsigned long long *sp;
            char nm[64], nm2[64];
            int i, shown = 0;
            logf("*** VEH AV: ip=%s %s bad=%p inArena=%d",
                 where_is(ip, nm, sizeof(nm)),
                 ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
                 bad, arena_owns(bad));
            sp = (unsigned long long *)&ip;
            for (i = 0; i < 1024 && shown < 12; i++) {
                void *v = (void *)sp[i];
                unsigned long long rva = 0;
                if (find_module(v, nm2, sizeof(nm2), &rva)) {
                    logf("      frame? %s+0x%llX", nm2, rva);
                    shown++;
                }
            }
        }
        g_inveh = 0;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
/* ---------------- reporting ---------------- */
static DWORD WINAPI reporter(LPVOID p)
{
    unsigned long long lastServed = 0;
    (void)p;
    for (;;) {
        char buf[400];
        Sleep(15000);
        EnterCriticalSection(&S.cs);
        sprintf(buf, "stats: VA calls=%llu(%.1fMB) big=%llu(%.1fMB) | HeapAlloc calls=%llu(%.1fMB) big=%llu(%.1fMB) | "
                       "HeapFree=%llu HeapReAlloc=%llu HeapSize=%llu | arenaServed=%llu(%.1fMB) fail=%llu lastServed=%.1fMB LIVE=%.1fMB peak=%.1fMB",
                  S.vaCalls, S.vaBytes / 1048576.0, S.vaBigCalls, S.vaBigBytes / 1048576.0,
                  S.haCalls, S.haBytes / 1048576.0, S.haBigCalls, S.haBigBytes / 1048576.0,
                  S.hfCalls, S.hrCalls, S.hsCalls,
                  S.servedCalls, S.servedBytes / 1048576.0, S.servedFail,
                  (S.servedBytes - lastServed) / 1048576.0, S.liveBytes / 1048576.0, S.peakBytes / 1048576.0);
        lastServed = S.servedBytes;
        LeaveCriticalSection(&S.cs);
        logf("%s", buf);
        hook_all_modules();          /* catch modules loaded later */
    }
    return 0;
}

static void lazy_maintenance(void);

static void init_sync(void)
{
    char logPath[MAX_PATH];
    read_cfg(logPath, sizeof(logPath));
    S.log = CreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (secondary_instance()) {
        logf("=== lpshim: pid=%lu is a SECONDARY instance (parent spawned copy) -> staying inert ===",
             (DWORD)GetCurrentProcessId());
        return;
    }
    logf("=== lpshim v2: pid=%lu hookVA=%d hookHeap=%d arena=%lluMB threshold=%lluMB ===",
         (DWORD)GetCurrentProcessId(), S.hookVA, S.hookHeap,
         (unsigned long long)(S.arenaSize / 1048576), (unsigned long long)(S.threshold / 1048576));
    if (S.minimal) {
        logf("lpshim: MINIMAL mode -> nothing else done (pid=%lu)", (DWORD)GetCurrentProcessId());
        return;
    }
    if (S.startDelayMs) {
        logf("lpshim: deferring init by %lu ms (leave the game's earliest startup alone)",
             (DWORD)S.startDelayMs);
        Sleep(S.startDelayMs);
    }
    enable_lock_pages();
    /* keep the real entry points before we patch any IAT */
    real_VA = (PFN_VA)GetProcAddress(GetModuleHandleA("kernel32.dll"), "VirtualAlloc");
    real_VF = (PFN_VF)GetProcAddress(GetModuleHandleA("kernel32.dll"), "VirtualFree");
    real_HA = (PFN_HA)GetProcAddress(GetModuleHandleA("kernel32.dll"), "HeapAlloc");
    real_HF = (PFN_HF)GetProcAddress(GetModuleHandleA("kernel32.dll"), "HeapFree");
    real_HR = (PFN_HR)GetProcAddress(GetModuleHandleA("kernel32.dll"), "HeapReAlloc");
    real_HS = (PFN_HS)GetProcAddress(GetModuleHandleA("kernel32.dll"), "HeapSize");
    {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        real_RtlA = (PFN_HA)GetProcAddress(nt, "RtlAllocateHeap");
        real_RtlF = (PFN_HF)GetProcAddress(nt, "RtlFreeHeap");
        real_RtlR = (PFN_HR)GetProcAddress(nt, "RtlReAllocateHeap");
        real_RtlS = (PFN_HS)GetProcAddress(nt, "RtlSizeHeap");
    }
    {
        typedef PVOID (WINAPI *PFN_AVEH)(ULONG, PVOID);
        PFN_AVEH aveh = (PFN_AVEH)GetProcAddress(GetModuleHandleA("kernel32.dll"), "AddVectoredExceptionHandler");
        if (aveh) aveh(1, (PVOID)veh);
    }
    if (!S.lazyArena) arena_init();
    else logf("lpshim: LazyArena -> arena will be reserved on first big request");
    hook_all_modules();
    logf("lpshim: init complete (no helper thread created)");
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hinst;
        {
            HANDLE h0 = CreateFileA("E:\\ai\\.rw\\attach-log.txt", FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, NULL);
            if (h0 != INVALID_HANDLE_VALUE) {
                char b[160]; DWORD w;
                sprintf(b, "[attach] pid=%lu tid=%lu", (DWORD)GetCurrentProcessId(), (DWORD)GetCurrentThreadId());
                SetFilePointer(h0, 0, NULL, FILE_END);
                WriteFile(h0, b, (DWORD)lstrlenA(b), &w, NULL);
                WriteFile(h0, "\r\n", 2, &w, NULL);
                FlushFileBuffers(h0); CloseHandle(h0);
            }
        }
        S.log = INVALID_HANDLE_VALUE; S.threshold = 1048576ULL;
        S.arenaSize = 4ULL * 1024ULL * 1024ULL * 1024ULL;
        S.hookVA = S.hookHeap = 1; S.verboseAlloc = 0; S.startDelayMs = 0;
        S.minimal = 0; S.lazyArena = 1;
        InitializeCriticalSection(&S.cs);
        DisableThreadLibraryCalls(hinst);
        init_sync();   /* synchronous: creating a thread this early makes Stellaris
                          spawn a helper copy of itself which then crashes */
    }
    return TRUE;
}
