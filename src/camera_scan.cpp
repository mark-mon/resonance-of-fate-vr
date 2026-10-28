// Diagnostic camera finder (data only: never touches game code, safe with the exe's anti-tamper layer).
// Walks the process's committed private read/write memory, finds objects whose first qword is the
// vtable of one of the game's camera classes, and dumps 0x1000 bytes of each into so4_camscan<N>.bin:
//   record = { u64 address, u32 vtable RVA, u32 bytes } + bytes
// A text summary goes to the log. Offline analysis (re/camscan.py) compares two dumps.
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <vector>
#include "camera_scan.h"
#include "log.h"

namespace so4vr {
namespace {

struct VtName { uint32_t rva; const char* name; };
const VtName kVt[] = {
#include "camvt.inc"
};

std::wstring g_dir;

bool Readable(DWORD p) {
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (p & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) != 0;
}

// Any object laid out like an Aska camera: projection (+0x420) with w row (0,0,1,0), the same matrix
// unjittered at +0x5D0 (bit pattern of row 3), and a vtable inside the exe. Catches subclasses.
int ScanShapes(FILE* f, uintptr_t base, std::vector<uint32_t>& vtFound) {
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    uintptr_t modEnd = base + nt->OptionalHeader.SizeOfImage;
    int hits = 0;
    MEMORY_BASIC_INFORMATION m;
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && VirtualQuery((void*)a, &m, sizeof m); a = (uintptr_t)m.BaseAddress + m.RegionSize) {
        if (m.State != MEM_COMMIT || m.Type != MEM_PRIVATE || !Readable(m.Protect) || m.RegionSize < 0x700) continue;
        const uint8_t* rb = (const uint8_t*)m.BaseAddress;
        size_t n = m.RegionSize - 0x700;
        __try {
            for (size_t o = 0; o < n; o += 8) {
                const uint32_t* q = (const uint32_t*)(rb + o);
                // proj row 3 at +0x450: (0, 0, 1, 0)
                if (q[(0x458) / 4] != 0x3F800000u || q[0x45C / 4] != 0 || q[0x450 / 4] != 0 || q[0x454 / 4] != 0) continue;
                if (q[(0x5D0 + 0x38) / 4] != 0x3F800000u) continue;
                float m00 = *(const float*)(rb + o + 0x420), m11 = *(const float*)(rb + o + 0x434);
                if (!(m00 > 0.2f && m00 < 20.f && m11 > 0.2f && m11 < 40.f)) continue;
                uintptr_t vt = *(const uintptr_t*)(rb + o);
                if (vt < base || vt >= modEnd) continue;
                uint64_t a64 = (uintptr_t)(rb + o); uint32_t rva = (uint32_t)(vt - base), bytes = 0x800;
                fwrite(&a64, 8, 1, f); fwrite(&rva, 4, 1, f); fwrite(&bytes, 4, 1, f); fwrite(rb + o, 1, bytes, f);
                vtFound.push_back(rva);
                if (++hits >= 256) return hits;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return hits;
}

void ScanOnce(int idx) {
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::vector<uintptr_t> vts; for (auto& v : kVt) vts.push_back(base + v.rva);
    std::vector<uintptr_t> sorted = vts; std::sort(sorted.begin(), sorted.end());
    uintptr_t lo = sorted.front(), hi = sorted.back();
    wchar_t path[MAX_PATH]; swprintf_s(path, L"%s\\rof_camscan%d.bin", g_dir.c_str(), idx);
    FILE* f = _wfopen(path, L"wb");
    if (!f) { LOGE("camscan: cannot write file"); return; }
    LARGE_INTEGER t0, t1, qf; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&qf);
    int hits = 0; uint64_t scanned = 0; int perClass[sizeof(kVt) / sizeof(kVt[0])] = {};
    MEMORY_BASIC_INFORMATION m;
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && VirtualQuery((void*)a, &m, sizeof m); a = (uintptr_t)m.BaseAddress + m.RegionSize) {
        if (m.State != MEM_COMMIT || m.Type != MEM_PRIVATE || !Readable(m.Protect)) continue;
        const uintptr_t* p = (const uintptr_t*)m.BaseAddress;
        size_t n = m.RegionSize / 8; scanned += m.RegionSize;
        __try {
            for (size_t i = 0; i < n; ++i) {
                uintptr_t v = p[i];
                if (v < lo || v > hi) continue;
                auto it = std::lower_bound(sorted.begin(), sorted.end(), v);
                if (it == sorted.end() || *it != v) continue;
                size_t k = std::find(vts.begin(), vts.end(), v) - vts.begin();
                uintptr_t obj = (uintptr_t)&p[i];
                uint32_t bytes = (uint32_t)std::min<uintptr_t>(0x1000, (uintptr_t)m.BaseAddress + m.RegionSize - obj);
                uint64_t a64 = obj; uint32_t rva = kVt[k].rva;
                fwrite(&a64, 8, 1, f); fwrite(&rva, 4, 1, f); fwrite(&bytes, 4, 1, f); fwrite((void*)obj, 1, bytes, f);
                perClass[k]++;
                if (++hits >= 4000) break;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (hits >= 4000) break;
    }
    {
        wchar_t p2[MAX_PATH]; swprintf_s(p2, L"%s\\rof_camshape%d.bin", g_dir.c_str(), idx);
        if (FILE* f2 = _wfopen(p2, L"wb")) {
            std::vector<uint32_t> vf;
            int sh = ScanShapes(f2, base, vf);
            fclose(f2);
            LOGI("camscan %d: %d camera-shaped objects", idx, sh);
            for (size_t i = 0; i < vf.size() && i < 32; ++i) LOGI("  shape #%zu vtable rva %X", i, vf[i]);
        }
    }
    fclose(f);
    QueryPerformanceCounter(&t1);
    LOGI("camscan %d: %d objects, %.0f MB scanned in %.2f s", idx, hits, scanned / 1048576.0, double(t1.QuadPart - t0.QuadPart) / qf.QuadPart);
    for (size_t k = 0; k < sizeof(kVt) / sizeof(kVt[0]); ++k)
        if (perClass[k]) LOGI("  %4d x %s (vt %X)", perClass[k], kVt[k].name, kVt[k].rva);
}

DWORD WINAPI Worker(LPVOID) {
    wchar_t req[MAX_PATH]; swprintf_s(req, L"%s\\rof_scan_now.txt", g_dir.c_str());
    int idx = 0;
    for (;;) {
        Sleep(500);
        if (GetFileAttributesW(req) == INVALID_FILE_ATTRIBUTES) continue;
        DeleteFileW(req);
        ScanOnce(idx++);
        wchar_t done[MAX_PATH]; swprintf_s(done, L"%s\\rof_scan_done%d.txt", g_dir.c_str(), idx - 1);
        if (FILE* f = _wfopen(done, L"wb")) fclose(f);
    }
}

} // namespace

void CameraScanStart(const std::wstring& gameDir) {
    g_dir = gameDir;
    if (HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(h);
}

} // namespace so4vr
