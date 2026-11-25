#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <cstring>

#pragma comment(linker, "/SUBSYSTEM:CONSOLE")

typedef NTSTATUS(NTAPI* pNtCreateSection)(PHANDLE, ACCESS_MASK, PVOID, PLARGE_INTEGER, ULONG, ULONG, HANDLE);
typedef NTSTATUS(NTAPI* pNtMapViewOfSection)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T,
    PLARGE_INTEGER, PSIZE_T, DWORD, ULONG, ULONG);

static DWORD PidOf() {
    DWORD self = GetCurrentProcessId();
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD p = 0;
    for (BOOL o = Process32FirstW(s, &pe); o; o = Process32NextW(s, &pe)) {
        if (pe.th32ProcessID == self) continue;
        std::wstring n = pe.szExeFile;
        for (auto& c : n) c = (wchar_t)towlower(c);
        if (n.find(L"robrun") != std::wstring::npos) { p = pe.th32ProcessID; break; }
    }
    CloseHandle(s);
    return p;
}

static uintptr_t RemoteModule(DWORD pid, const char* name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (s == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me{ sizeof(me) };
    uintptr_t out = 0;
    for (BOOL o = Module32FirstW(s, &me); o; o = Module32NextW(s, &me)) {
        char n[MAX_MODULE_NAME32 + 1]{};
        WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, n, sizeof n, nullptr, nullptr);
        if (_stricmp(n, name) == 0) { out = (uintptr_t)me.modBaseAddr; break; }
    }
    CloseHandle(s);
    return out;
}

static bool ReadFileAll(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(f, nullptr);
    if (!sz || sz == INVALID_FILE_SIZE) { CloseHandle(f); return false; }
    out.resize(sz);
    DWORD rd = 0;
    bool ok = ReadFile(f, out.data(), sz, &rd, nullptr) && rd == sz;
    CloseHandle(f);
    return ok;
}

static void FixImports(HANDLE h, DWORD pid, std::vector<uint8_t>& img,
    IMAGE_NT_HEADERS* nt, uintptr_t base) {
    auto& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imp.Size) return;
    auto* d = (IMAGE_IMPORT_DESCRIPTOR*)(img.data() + imp.VirtualAddress);
    for (; d->Name; d++) {
        const char* dll = (const char*)(img.data() + d->Name);
        HMODULE local = GetModuleHandleA(dll);
        if (!local) local = LoadLibraryA(dll);
        if (!local) continue;
        uintptr_t rbase = RemoteModule(pid, dll);
        if (!rbase) continue;
        size_t n = 0;
        auto* thunk = (IMAGE_THUNK_DATA*)(img.data() + d->OriginalFirstThunk);
        for (; thunk->u1.AddressOfData; thunk++, n++) {
            FARPROC p = nullptr;
            if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                p = GetProcAddress(local, (LPCSTR)(thunk->u1.Ordinal & 0xFFFF));
            else {
                auto* ibn = (IMAGE_IMPORT_BY_NAME*)(img.data() + thunk->u1.AddressOfData);
                p = GetProcAddress(local, ibn->Name);
            }
            if (!p) continue;
            uint64_t ra = (uint64_t)rbase + ((uintptr_t)p - (uintptr_t)local);
            WriteProcessMemory(h, (void*)(base + d->FirstThunk + n * 8), &ra, 8, nullptr);
        }
    }
}

static void WipeHeaders(HANDLE h, uintptr_t base, size_t size) {
    size_t n = size < 0x1000 ? size : 0x1000;
    DWORD old = 0;
    if (!VirtualProtectEx(h, (void*)base, n, PAGE_READWRITE, &old)) return;
    std::vector<uint8_t> zero(n, 0);
    WriteProcessMemory(h, (void*)base, zero.data(), n, nullptr);
    VirtualProtectEx(h, (void*)base, n, PAGE_READONLY, &old);
}

/* szekcio-alapu mapping: MEM_MAPPED, nincs RWX, kernel allitja a jogosultsagokat */
static bool MapSection(HANDLE h, const wchar_t* path, std::vector<uint8_t>& img,
    IMAGE_NT_HEADERS* nt, DWORD pid, uintptr_t& base) {
    HMODULE ntd = GetModuleHandleW(L"ntdll.dll");
    if (!ntd) return false;
    auto create = (pNtCreateSection)GetProcAddress(ntd, "NtCreateSection");
    auto map = (pNtMapViewOfSection)GetProcAddress(ntd, "NtMapViewOfSection");
    if (!create || !map) return false;

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    HANDLE sec = nullptr;
    NTSTATUS st = create(&sec, SECTION_ALL_ACCESS, nullptr, nullptr, PAGE_READONLY, SEC_IMAGE, f);
    CloseHandle(f);
    if (st < 0 || !sec) return false;

    PVOID view = nullptr;
    SIZE_T vsize = 0;
    st = map(sec, h, &view, 0, 0, nullptr, &vsize, 1, 0, PAGE_READONLY);
    CloseHandle(sec);
    if (st < 0 || !view) return false;

    base = (uintptr_t)view;
    FixImports(h, pid, img, nt, base);
    WipeHeaders(h, base, nt->OptionalHeader.SizeOfImage);
    FlushInstructionCache(h, (void*)base, nt->OptionalHeader.SizeOfImage);
    return true;
}

/* tartalek: privat allokacio, szekcionkent jogosultsag */
static bool MapPrivate(HANDLE h, std::vector<uint8_t>& img, IMAGE_NT_HEADERS* nt,
    DWORD pid, uintptr_t& base) {
    size_t size = nt->OptionalHeader.SizeOfImage;
    void* remote = VirtualAllocEx(h, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    base = (uintptr_t)remote;

    if (!WriteProcessMemory(h, remote, img.data(), nt->OptionalHeader.SizeOfHeaders, nullptr))
        return false;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (sec->SizeOfRawData)
            WriteProcessMemory(h, (void*)(base + sec->VirtualAddress),
                img.data() + sec->PointerToRawData, sec->SizeOfRawData, nullptr);

    int64_t delta = (int64_t)base - (int64_t)nt->OptionalHeader.ImageBase;
    auto& rel = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (delta && rel.Size) {
        auto* blk = (IMAGE_BASE_RELOCATION*)(img.data() + rel.VirtualAddress);
        uintptr_t end = rel.VirtualAddress + rel.Size;
        while ((uintptr_t)blk < end && blk->SizeOfBlock) {
            int cnt = (blk->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
            WORD* e = (WORD*)(blk + 1);
            for (int i = 0; i < cnt; i++) {
                int type = e[i] >> 12;
                int off = e[i] & 0xFFF;
                if (type != IMAGE_REL_BASED_DIR64) continue;
                uintptr_t a = base + blk->VirtualAddress + off;
                uint64_t v = 0;
                ReadProcessMemory(h, (void*)a, &v, 8, nullptr);
                v += (uint64_t)delta;
                WriteProcessMemory(h, (void*)a, &v, 8, nullptr);
            }
            blk = (IMAGE_BASE_RELOCATION*)((uint8_t*)blk + blk->SizeOfBlock);
        }
    }

    FixImports(h, pid, img, nt, base);
    WipeHeaders(h, base, size);

    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        DWORD prot = PAGE_READONLY;
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) prot = PAGE_EXECUTE_READ;
        else if (sec->Characteristics & IMAGE_SCN_MEM_WRITE) prot = PAGE_READWRITE;
        DWORD old = 0;
        VirtualProtectEx(h, (void*)(base + sec->VirtualAddress), sec->Misc.VirtualSize, prot, &old);
    }
    FlushInstructionCache(h, (void*)base, size);
    return true;
}

static bool Hijack(HANDLE h, DWORD pid, uintptr_t entry, uintptr_t base) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (s == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te{ sizeof(te) };
    bool done = false;
    for (BOOL o = Thread32First(s, &te); o && !done; o = Thread32Next(s, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
            FALSE, te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &c)) {
            uint64_t ret = c.Rip;
            uint64_t sp = c.Rsp - 8;
            if (WriteProcessMemory(h, (void*)sp, &ret, 8, nullptr)) {
                c.Rsp = sp;
                c.Rip = entry;
                c.Rcx = base;
                c.Rdx = 1;
                if (SetThreadContext(th, &c)) done = true;
            }
        }
        ResumeThread(th);
        CloseHandle(th);
    }
    CloseHandle(s);
    return done;
}

int wmain() {
    SetConsoleTitleW(L"VANISH  -  F10 = inject");

    wchar_t dll[MAX_PATH]{};
    GetFullPathNameW(L"vanish.dll", MAX_PATH, dll, nullptr);
    std::vector<uint8_t> img;
    if (!ReadFileAll(dll, img)) {
        wprintf(L"vanish.dll nincs az injector mellett.\n");
        getchar();
        return 1;
    }

    auto* dos = (IMAGE_DOS_HEADER*)img.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { wprintf(L"vanish.dll nem PE.\n"); getchar(); return 1; }
    auto* nt = (IMAGE_NT_HEADERS*)(img.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { wprintf(L"vanish.dll nem PE.\n"); getchar(); return 1; }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) { wprintf(L"vanish.dll nem x64.\n"); getchar(); return 1; }

    wprintf(L"VANISH\n\n");
    wprintf(L"1. Inditsd el a ROB RUN-t, lepj be egy szerverre, varj a betoltesre.\n");
    wprintf(L"2. Nyomd meg az F10-et.\n\n");

    for (;;) {
        wprintf(L"Varom az F10-et...\n");
        while (!(GetAsyncKeyState(VK_F10) & 0x8000)) Sleep(20);
        while (GetAsyncKeyState(VK_F10) & 0x8000) Sleep(20);
        wprintf(L"F10 -> injektalas...\n");

        DWORD pid = PidOf();
        if (!pid) {
            wprintf(L"ROB RUN nem fut. Inditsd el, lepj be egy szerverre, majd ujra F10.\n\n");
            continue;
        }

        HANDLE h = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
            PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) {
            wprintf(L"Nem sikerult megnyitni. Futtasd adminisztratorkent.\n\n");
            continue;
        }

        uintptr_t base = 0;
        bool ok = MapSection(h, dll, img, nt, pid, base);
        if (ok) wprintf(L"mod: szekcio (MEM_MAPPED)\n");
        else {
            wprintf(L"szekcio mapping nem ment, tartalek: privat\n");
            ok = MapPrivate(h, img, nt, pid, base);
        }
        if (!ok) {
            wprintf(L"Mapping hiba. Probald ujra az F10-et.\n\n");
            CloseHandle(h);
            continue;
        }

        uintptr_t entry = base + nt->OptionalHeader.AddressOfEntryPoint;
        if (!Hijack(h, pid, entry, base)) {
            wprintf(L"Thread hijack hiba. Probald ujra az F10-et.\n\n");
            CloseHandle(h);
            continue;
        }
        CloseHandle(h);

        wprintf(L"\nOK.\n");
        wprintf(L"F10 = menu, F1 ESP, F2 radar, F4 vanish.\n");
        wprintf(L"Aim: MB5, silent: LSHIFT, trigger: ALT. Kilepes elott END.\n");
        Sleep(1800);
        return 0;
    }
}
