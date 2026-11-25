#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

#define OFF_GOM_ACTIVE    0x10
#define OFF_NODE_NEXT     0x10
#define OFF_NODE_OBJ      0x18
#define OFF_GO_COMPONENTS 0x30
#define OFF_ARR_SIZE      0x18
#define OFF_ARR_DATA      0x20
#define OFF_COMP_CLASS    0x00
#define OFF_COMP_PTR      0x08
#define OFF_CAM_FOV       0x1B4
#define OFF_ENABLED       0x38
#define ESP_HEAD          1.75f
#define ESP_CHEST         1.20f

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
static inline Vec3 operator-(const Vec3& a, const Vec3& b) { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
static inline float Dot(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline float Len(const Vec3& a) { return sqrtf(a.x*a.x + a.y*a.y + a.z*a.z); }

template <class T> static T RD(uintptr_t a) { __try { return *(T*)a; } __except (1) { return T{}; } }
static bool RB(uintptr_t a, void* d, size_t n) { __try { memcpy(d, (void*)a, n); return true; } __except (1) { return false; } }
template <class T> static void WR(uintptr_t a, T v) { __try { *(T*)a = v; } __except (1) {} }
static std::string RStr(uintptr_t a) { char b[64]{}; RB(a, b, 60); return std::string(b); }

static HMODULE g_mod;
static uintptr_t Gom, TrCls, CamCls, g_matOff = 0, g_base;
static uintptr_t g_hpOff = 0, g_shOff = 0;
static int g_objs = 0, g_players = 0;
static bool g_stats = false;

/* ---------- osztaly / komponens ---------- */
static uintptr_t ClassOf(uintptr_t go, const char* want) {
    uintptr_t arr = RD<uintptr_t>(go + OFF_GO_COMPONENTS);
    if (!arr) return 0;
    int n = RD<int>(arr + OFF_ARR_SIZE);
    uintptr_t data = RD<uintptr_t>(arr + OFF_ARR_DATA);
    if (!data || n <= 0 || n > 400) return 0;
    for (int i = 0; i < n; i++) {
        uintptr_t cls = RD<uintptr_t>(data + i*16 + OFF_COMP_CLASS);
        if (!cls) continue;
        uintptr_t np = RD<uintptr_t>(cls + 0x10);
        if (np && RStr(np) == want) return cls;
    }
    return 0;
}
static uintptr_t ClassPart(uintptr_t go, const char* part) {
    uintptr_t arr = RD<uintptr_t>(go + OFF_GO_COMPONENTS);
    if (!arr) return 0;
    int n = RD<int>(arr + OFF_ARR_SIZE);
    uintptr_t data = RD<uintptr_t>(arr + OFF_ARR_DATA);
    if (!data || n <= 0 || n > 400) return 0;
    for (int i = 0; i < n; i++) {
        uintptr_t cls = RD<uintptr_t>(data + i*16 + OFF_COMP_CLASS);
        if (!cls) continue;
        uintptr_t np = RD<uintptr_t>(cls + 0x10);
        if (!np) continue;
        if (RStr(np).find(part) != std::string::npos) return cls;
    }
    return 0;
}
static uintptr_t CompOf(uintptr_t go, uintptr_t cls) {
    uintptr_t arr = RD<uintptr_t>(go + OFF_GO_COMPONENTS);
    if (!arr) return 0;
    int n = RD<int>(arr + OFF_ARR_SIZE);
    uintptr_t data = RD<uintptr_t>(arr + OFF_ARR_DATA);
    if (!data) return 0;
    for (int i = 0; i < n && i < 400; i++)
        if (RD<uintptr_t>(data + i*16 + OFF_COMP_CLASS) == cls)
            return RD<uintptr_t>(data + i*16 + OFF_COMP_PTR);
    return 0;
}
template <class F> static void EachObject(F fn) {
    if (!Gom) return;
    uintptr_t node = RD<uintptr_t>(Gom + OFF_GOM_ACTIVE);
    for (int g = 0; node && g < 15000; g++) {
        uintptr_t go = RD<uintptr_t>(node + OFF_NODE_OBJ);
        if (go) fn(go);
        node = RD<uintptr_t>(node + OFF_NODE_NEXT);
    }
}

/* ---------- matrix ---------- */
static bool DetectMat(uintptr_t tr, uintptr_t& out) {
    for (uintptr_t o = 0x30; o < 0x200; o += 4) {
        float f[16];
        if (!RB(tr + o, f, sizeof f)) continue;
        if (f[15] < 0.999f || f[15] > 1.001f) continue;
        if (!(f[12] > -100000.f && f[12] < 100000.f)) continue;
        if (!(f[13] > -100000.f && f[13] < 100000.f)) continue;
        if (!(f[14] > -100000.f && f[14] < 100000.f)) continue;
        if (fabsf(f[0]) < 0.01f && fabsf(f[5]) < 0.01f && fabsf(f[10]) < 0.01f) continue;
        out = o;
        return true;
    }
    return false;
}
static bool WorldPos(uintptr_t tr, Vec3& out) {
    if (!g_matOff) return false;
    float f[16];
    if (!RB(tr + g_matOff, f, sizeof f)) return false;
    out = { f[12], f[13], f[14] };
    return out.x > -1e6f && out.x < 1e6f;
}

/* ---------- hp / pajzs ---------- */
static bool DetectPair(uintptr_t comp, uintptr_t& off) {
    for (uintptr_t o = 0x20; o < 0x180; o += 4) {
        float a, b;
        if (!RB(comp + o, &a, 8)) continue;
        if (!(a > 0.f && a <= 1000.f)) continue;
        if (!(b > 0.f && b <= 1000.f)) continue;
        if (b < a * 0.5f) continue;
        off = o;
        return true;
    }
    return false;
}
static bool ReadPair(uintptr_t go, const char* part, uintptr_t off, float& cur, float& maxv) {
    if (!off || !go) return false;
    uintptr_t cls = ClassPart(go, part);
    if (!cls) return false;
    uintptr_t cmp = CompOf(go, cls);
    if (!cmp) return false;
    float a = 0, b = 0;
    if (!RB(cmp + off, &a, 8)) return false;
    if (!(a >= 0.f && a < 1e5f)) return false;
    if (!(b > 0.f && b < 1e5f)) return false;
    cur = a; maxv = b;
    return true;
}

/* ---------- kamera ---------- */
struct Cam { Vec3 pos, right, up, fwd; float fov; };
static bool GetCam(Cam& c) {
    bool got = false;
    EachObject([&](uintptr_t go) {
        if (got) return;
        uintptr_t cam = CompOf(go, CamCls);
        if (!cam) return;
        uintptr_t tr = CompOf(go, TrCls);
        if (!tr) return;
        if (!g_matOff && !DetectMat(tr, g_matOff)) return;
        float f[16];
        if (!RB(tr + g_matOff, f, sizeof f)) return;
        c.pos = { f[12], f[13], f[14] };
        c.right = { f[0], f[1], f[2] };
        c.up = { f[4], f[5], f[6] };
        c.fwd = { f[8], f[9], f[10] };
        c.fov = RD<float>(cam + OFF_CAM_FOV);
        if (c.fov < 20.f || c.fov > 130.f) c.fov = 60.f;
        got = true;
    });
    return got;
}
static bool W2S(const Vec3& p, const Cam& c, int w, int h, Vec2& o) {
    Vec3 d = p - c.pos;
    float z = Dot(d, c.fwd);
    if (z < 0.2f) return false;
    float sy = 1.f / tanf(c.fov * 0.5f * 3.14159265f / 180.f);
    float sx = sy / ((float)w / (float)h);
    o.x = (Dot(d, c.right) * sx / z * 0.5f + 0.5f) * w;
    o.y = (0.5f - Dot(d, c.up) * sy / z * 0.5f) * h;
    return o.x > -200 && o.x < w + 200 && o.y > -200 && o.y < h + 200;
}

/* ---------- vanish ---------- */
struct Restore { uintptr_t addr; uint8_t old; };
static std::vector<Restore> g_hidden;
static bool g_vanish = false;

static uintptr_t LocalPlayer(const Cam& c) {
    uintptr_t best = 0;
    float bd = 3.0f;
    EachObject([&](uintptr_t go) {
        uintptr_t tr = CompOf(go, TrCls);
        Vec3 p;
        if (!tr || !WorldPos(tr, p)) return;
        float d = Len(p - c.pos);
        if (d < bd) { bd = d; best = go; }
    });
    return best;
}
static void SetVanish(bool on, const Cam& cam) {
    if (on == g_vanish) return;
    g_vanish = on;
    if (on) {
        uintptr_t go = LocalPlayer(cam);
        if (!go) { g_vanish = false; return; }
        uintptr_t arr = RD<uintptr_t>(go + OFF_GO_COMPONENTS);
        if (!arr) return;
        int n = RD<int>(arr + OFF_ARR_SIZE);
        uintptr_t data = RD<uintptr_t>(arr + OFF_ARR_DATA);
        if (!data) return;
        for (int i = 0; i < n && i < 400; i++) {
            uintptr_t cls = RD<uintptr_t>(data + i*16 + OFF_COMP_CLASS);
            uintptr_t cmp = RD<uintptr_t>(data + i*16 + OFF_COMP_PTR);
            if (!cls || !cmp) continue;
            uintptr_t np = RD<uintptr_t>(cls + 0x10);
            if (!np) continue;
            std::string nm = RStr(np);
            if (nm.find("Renderer") != std::string::npos || nm.find("Collider") != std::string::npos) {
                uint8_t old = RD<uint8_t>(cmp + OFF_ENABLED);
                if (old > 1) continue;
                g_hidden.push_back({ cmp + OFF_ENABLED, old });
                WR<uint8_t>(cmp + OFF_ENABLED, 0);
            }
        }
    } else {
        for (auto& r : g_hidden) WR<uint8_t>(r.addr, r.old);
        g_hidden.clear();
    }
}

/* ---------- config ---------- */
struct Bind { int vk; };
static Bind g_bAim{ VK_XBUTTON2 }, g_bSilent{ VK_LSHIFT }, g_bTrig{ VK_MENU };

static bool g_menu = true, g_esp = true, g_radar = true;
static bool g_box = true, g_hp = true, g_sh = true, g_name = true, g_dist = true, g_snap = false;
static bool g_aim = false, g_silent = false, g_trig = false;
static float g_fov = 110.f, g_smooth = 4.5f, g_tfov = 18.f;
static int g_bone = 0, g_trigDelay = 35;
static int g_page = 0, g_sel = 0;
static Bind* g_cap = nullptr;

static bool Held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static bool Hit(int vk) { return (GetAsyncKeyState(vk) & 1) != 0; }

static const wchar_t* KeyName(int vk) {
    switch (vk) {
    case VK_LBUTTON: return L"LMB";
    case VK_RBUTTON: return L"RMB";
    case VK_MBUTTON: return L"MMB";
    case VK_XBUTTON1: return L"MB4";
    case VK_XBUTTON2: return L"MB5";
    case VK_LSHIFT:  return L"LSHIFT";
    case VK_RSHIFT:  return L"RSHIFT";
    case VK_MENU:    return L"ALT";
    case VK_SPACE:   return L"SPACE";
    case VK_CONTROL: return L"CTRL";
    case VK_CAPITAL: return L"CAPS";
    case VK_TAB:     return L"TAB";
    }
    static wchar_t b[8];
    if ((vk >= 0x41 && vk <= 0x5A) || (vk >= 0x30 && vk <= 0x39)) {
        b[0] = (wchar_t)vk; b[1] = 0; return b;
    }
    swprintf_s(b, L"0x%02X", vk);
    return b;
}

struct Row { const wchar_t* label; int kind; void* p; };
static Row P0[] = {
    { L"ESP",      0, &g_esp }, { L"Box",      0, &g_box  },
    { L"Health",   0, &g_hp  }, { L"Shield",   0, &g_sh   },
    { L"Name",     0, &g_name}, { L"Distance", 0, &g_dist },
    { L"Snapline", 0, &g_snap}, { L"Radar",    0, &g_radar},
    { L"Vanish",   0, &g_vanish }
};
static Row P1[] = {
    { L"Aimbot",        0, &g_aim    }, { L"Aim bind",    3, &g_bAim     },
    { L"Smoothing",     1, &g_smooth }, { L"Aim FOV",     1, &g_fov      },
    { L"Bone",          2, &g_bone   }, { L"Silent aim",  0, &g_silent   },
    { L"Silent bind",   3, &g_bSilent}, { L"Triggerbot",  0, &g_trig     },
    { L"Trigger bind",  3, &g_bTrig  }, { L"Trigger FOV", 1, &g_tfov     },
    { L"Trigger delay", 2, &g_trigDelay }
};
static Row* PageRows() { return g_page ? P1 : P0; }
static int PageCount() { return g_page ? 11 : 9; }

static void Adjust(Row& r, int dir) {
    switch (r.kind) {
    case 0:
        *(bool*)r.p = !*(bool*)r.p;
        break;
    case 1: {
        float* f = (float*)r.p;
        float step = (f == &g_smooth) ? 0.5f : (f == &g_fov ? 5.f : 1.f);
        float lo = (f == &g_smooth) ? 1.f : (f == &g_fov ? 20.f : 2.f);
        float hi = (f == &g_smooth) ? 15.f : (f == &g_fov ? 400.f : 60.f);
        *f += step * (float)dir;
        if (*f < lo) *f = lo;
        if (*f > hi) *f = hi;
        break;
    }
    case 2: {
        int* i = (int*)r.p;
        if (i == &g_bone) *i = !*i;
        else {
            *i += 5 * dir;
            if (*i < 10) *i = 10;
            if (*i > 200) *i = 200;
        }
        break;
    }
    case 3:
        g_cap = (Bind*)r.p;
        break;
    }
}

/* ---------- d2d ---------- */
static ID2D1Factory* g_fac;
static IDWriteFactory* g_dw;
static ID2D1RenderTarget* g_rt;
static IDWriteTextFormat *g_tS, *g_TL;
static IDXGISwapChain* g_sc;
static bool g_hooked = false;
static int g_sw = 1920, g_sh = 1080;

static void MakeRT() {
    if (g_rt || !g_sc || !g_fac) return;
    IDXGISurface* surf = nullptr;
    if (FAILED(g_sc->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surf)) || !surf) return;
    D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    g_fac->CreateDxgiSurfaceRenderTarget(surf, &rp, &g_rt);
    surf->Release();
    if (g_rt) {
        g_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
}
static void Txt(float x, float y, const wchar_t* s, D2D1_COLOR_F c, bool big = false) {
    if (!g_rt) return;
    ID2D1SolidColorBrush* b{};
    g_rt->CreateSolidColorBrush(c, &b);
    g_rt->DrawTextW(s, (UINT32)wcslen(s), big ? g_TL : g_tS,
        D2D1::RectF(x, y, x + 900, y + 40), b);
    b->Release();
}
static void Box(float l, float t, float r, float b, D2D1_COLOR_F c, float th = 1.5f) {
    if (!g_rt) return;
    ID2D1SolidColorBrush* br{};
    g_rt->CreateSolidColorBrush(c, &br);
    g_rt->DrawRectangle(D2D1::RectF(l, t, r, b), br, th);
    br->Release();
}
static void Fill(float l, float t, float r, float b, D2D1_COLOR_F c) {
    if (!g_rt) return;
    ID2D1SolidColorBrush* br{};
    g_rt->CreateSolidColorBrush(c, &br);
    g_rt->FillRectangle(D2D1::RectF(l, t, r, b), br);
    br->Release();
}
static void Dot2(float x, float y, float rad, D2D1_COLOR_F c) {
    if (!g_rt) return;
    ID2D1SolidColorBrush* br{};
    g_rt->CreateSolidColorBrush(c, &br);
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), rad, rad), br);
    br->Release();
}
static void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float th = 1.f) {
    if (!g_rt) return;
    ID2D1SolidColorBrush* br{};
    g_rt->CreateSolidColorBrush(c, &br);
    g_rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), br, th);
    br->Release();
}

/* ---------- input ---------- */
static void MoveMouse(float dx, float dy) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    in.mi.dx = (LONG)dx;
    in.mi.dy = (LONG)dy;
    SendInput(1, &in, sizeof in);
}
static void ClickMouse() {
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

struct Target { uintptr_t go; Vec3 feet; Vec3 aim; Vec2 scr; float dist; bool valid; };

/* ---------- menu ---------- */
static void DrawMenu() {
    Row* rows = PageRows();
    int n = PageCount();
    float top = 12.f;
    float h = 52.f + n * 18.f + 46.f;
    Box(12, top, 372, top + h, D2D1::ColorF(0.06f, 0.06f, 0.06f, 0.88f));
    Fill(12, top, 372, top + 26, D2D1::ColorF(0.10f, 0.32f, 0.20f, 0.95f));
    Txt(20, top + 5, g_page ? L"VANISH  /  AIM" : L"VANISH  /  VISUALS",
        D2D1::ColorF(0.5f, 1, 0.6f, 1), true);

    for (int i = 0; i < n; i++) {
        float y = top + 32 + i * 18.f;
        bool sel = (i == g_sel);
        if (sel) Fill(14, y - 1, 370, y + 15, D2D1::ColorF(0.2f, 0.4f, 0.55f, 0.55f));
        D2D1_COLOR_F c = sel ? D2D1::ColorF(1, 1, 1, 1) : D2D1::ColorF(0.82f, 0.82f, 0.82f, 1);
        Row& r = rows[i];
        wchar_t line[128];
        if (r.kind == 0) {
            bool on = *(bool*)r.p;
            swprintf_s(line, L"%s  %s", r.label, on ? L"[ON]" : L"[off]");
            if (on) c = D2D1::ColorF(0.45f, 1, 0.55f, 1);
        } else if (r.kind == 1) {
            swprintf_s(line, L"%s  %.1f", r.label, *(float*)r.p);
        } else if (r.kind == 2) {
            if (r.p == &g_bone) swprintf_s(line, L"%s  %s", r.label, *(int*)r.p ? L"chest" : L"head");
            else swprintf_s(line, L"%s  %d ms", r.label, *(int*)r.p);
        } else {
            Bind* b = (Bind*)r.p;
            if (g_cap == b) swprintf_s(line, L"%s  < press key >", r.label);
            else swprintf_s(line, L"%s  %s", r.label, KeyName(b->vk));
        }
        Txt(20, y, line, c);
    }

    float fy = top + 34 + n * 18.f;
    Txt(20, fy, L"F10 menu  UP/DOWN  LEFT/RIGHT  ENTER  F7 rebind  TAB",
        D2D1::ColorF(0.65f, 0.65f, 0.65f, 1));

    D2D1_COLOR_F ok = D2D1::ColorF(0.5f, 0.9f, 0.5f, 1);
    D2D1_COLOR_F bad = D2D1::ColorF(1, 0.7f, 0.2f, 1);
    wchar_t d[192];
    swprintf_s(d, L"gom:%s tr:%s cam:%s mat:%s stat:%s obj:%d pl:%d",
        Gom ? L"ok" : L"--", TrCls ? L"ok" : L"--", CamCls ? L"ok" : L"--",
        g_matOff ? L"ok" : L"--", g_stats ? L"ok" : L"--", g_objs, g_players);
    Txt(20, fy + 20, d, (Gom && TrCls && CamCls && g_matOff) ? ok : bad);
    Txt(20, fy + 38, L"END = unhook + unload", D2D1::ColorF(0.6f, 0.6f, 0.6f, 1));
}

/* ---------- draw ---------- */
static void Draw() {
    Cam cam{};
    bool hasCam = GetCam(cam);
    if (g_vanish && !hasCam) g_vanish = false;

    std::vector<Target> list;
    if (hasCam) {
        EachObject([&](uintptr_t go) {
            g_objs++;
            uintptr_t tr = CompOf(go, TrCls);
            Vec3 p;
            if (!tr || !WorldPos(tr, p)) return;
            float d = Len(p - cam.pos);
            if (d < 1.2f || d > 350.f) return;
            Target t{};
            t.go = go;
            t.feet = p;
            t.dist = d;
            t.aim = { p.x, p.y + (g_bone ? ESP_CHEST : ESP_HEAD), p.z };
            t.valid = W2S(p, cam, g_sw, g_sh, t.scr);
            list.push_back(t);
        });
    }
    g_players = (int)list.size();

    if (!g_stats && hasCam && !list.empty()) {
        uintptr_t any = 0;
        EachObject([&](uintptr_t go) { if (!any) any = go; });
        if (any) {
            uintptr_t hcls = ClassPart(any, "Health");
            if (hcls) {
                uintptr_t hcmp = CompOf(any, hcls);
                if (hcmp && DetectPair(hcmp, g_hpOff)) {
                    uintptr_t scls = ClassPart(any, "Shield");
                    if (!scls) scls = ClassPart(any, "Armor");
                    if (!scls) scls = ClassPart(any, "Barrier");
                    if (scls) {
                        uintptr_t scmp = CompOf(any, scls);
                        DetectPair(scmp, g_shOff);
                    }
                    g_stats = true;
                }
            }
        }
    }

    g_rt->BeginDraw();

    if (g_esp) {
        for (auto& t : list) {
            if (!t.valid) continue;
            Vec2 head;
            Vec3 hp{ t.feet.x, t.feet.y + ESP_HEAD, t.feet.z };
            if (!W2S(hp, cam, g_sw, g_sh, head)) continue;
            float hgt = t.scr.y - head.y;
            if (hgt < 6.f) continue;
            float wid = hgt * 0.42f;
            float l = t.scr.x - wid / 2;
            float r = t.scr.x + wid / 2;

            if (g_box) Box(l, head.y, r, t.scr.y, D2D1::ColorF(0.9f, 0.25f, 0.25f, 0.9f));

            float cur = 0, maxv = 0;
            if (g_hp && ReadPair(t.go, "Health", g_hpOff, cur, maxv) && maxv > 0.f) {
                float frac = cur / maxv;
                if (frac > 1.f) frac = 1.f;
                Fill(l - 6, head.y, l - 2, t.scr.y, D2D1::ColorF(0, 0, 0, 0.55f));
                Fill(l - 6, t.scr.y - (t.scr.y - head.y) * frac, l - 2, t.scr.y,
                    D2D1::ColorF(1.f - frac, frac, 0.15f, 0.95f));
            }
            float scur = 0, smax = 0;
            if (g_sh && ReadPair(t.go, "Shield", g_shOff, scur, smax) && smax > 0.f) {
                float frac = scur / smax;
                if (frac > 1.f) frac = 1.f;
                Fill(r + 2, head.y, r + 6, t.scr.y, D2D1::ColorF(0, 0, 0, 0.55f));
                Fill(r + 2, t.scr.y - (t.scr.y - head.y) * frac, r + 6, t.scr.y,
                    D2D1::ColorF(0.25f, 0.6f, 1.f, 0.95f));
            }
            if (g_snap) Line(g_sw / 2.f, (float)g_sh, t.scr.x, t.scr.y, D2D1::ColorF(1, 1, 1, 0.28f));
            if (g_name) Txt(l, head.y - 16, L"player", D2D1::ColorF(1, 1, 1, 0.9f));
            if (g_dist) {
                wchar_t d[32];
                swprintf_s(d, L"%.0fm", t.dist);
                Txt(t.scr.x - 12, t.scr.y + 2, d, D2D1::ColorF(1, 1, 1, 0.85f));
            }
        }
    }

    if (g_radar && hasCam) {
        float cx = 160, cy = 160, rad = 120, range = 150.f;
        Dot2(cx, cy, rad, D2D1::ColorF(0, 0, 0, 0.35f));
        float yaw = atan2f(cam.fwd.x, cam.fwd.z);
        float s = sinf(-yaw), co = cosf(-yaw);
        for (auto& e : list) {
            float dx = e.feet.x - cam.pos.x, dz = e.feet.z - cam.pos.z;
            if (fabsf(dx) > range || fabsf(dz) > range) continue;
            float rx = dx * co - dz * s;
            float rz = dx * s + dz * co;
            float k = rad / range;
            float px = cx + rx * k, py = cy - rz * k;
            float l = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
            if (l > rad) {
                px = cx + (px - cx) / l * rad;
                py = cy + (py - cy) / l * rad;
            }
            Dot2(px, py, 3.f, D2D1::ColorF(1, 0.3f, 0.3f, 1));
        }
        Box(cx - rad, cy - rad, cx + rad, cy + rad, D2D1::ColorF(0.4f, 0.4f, 0.4f, 0.5f));
    }

    if (hasCam && (g_aim || g_silent || g_trig)) {
        float f = (g_aim || g_silent) ? g_fov : g_tfov;
        ID2D1SolidColorBrush* br{};
        g_rt->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.8f, 1, 0.55f), &br);
        g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(g_sw / 2.f, g_sh / 2.f), f, f), br, 1.2f);
        br->Release();
    }

    Target* best = nullptr;
    float bestScore = 1e9f;
    if (hasCam) {
        for (auto& t : list) {
            if (!t.valid) continue;
            float dx = t.scr.x - g_sw / 2.f;
            float dy = t.scr.y - g_sh / 2.f;
            float d = sqrtf(dx * dx + dy * dy);
            float lim = (g_aim || g_silent) ? g_fov : g_tfov;
            if (d > lim) continue;
            float score = d + t.dist * 0.05f;
            if (score < bestScore) { bestScore = score; best = &t; }
        }
    }

    if (best) {
        float dx = best->scr.x - g_sw / 2.f;
        float dy = best->scr.y - g_sh / 2.f;
        Box(best->scr.x - 7, best->scr.y - 7, best->scr.x + 7, best->scr.y + 7,
            D2D1::ColorF(0.2f, 1, 0.4f, 1));

        if (g_aim && Held(g_bAim.vk)) MoveMouse(dx / g_smooth, dy / g_smooth);

        if (g_silent && Held(g_bSilent.vk)) {
            static DWORD last = 0;
            DWORD now = GetTickCount();
            if (now - last > 90) { MoveMouse(dx, dy); last = now; }
        }
        if (g_trig && Held(g_bTrig.vk)) {
            static DWORD lastClick = 0;
            DWORD now = GetTickCount();
            if (now - lastClick > (DWORD)g_trigDelay) {
                ClickMouse();
                lastClick = now;
            }
        }
    }

    if (g_menu) DrawMenu();
    g_rt->EndDraw();
}

/* ---------- hook ---------- */
typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(__stdcall* Resize_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static Present_t oPresent;
static Resize_t oResize;
static void** g_vt;

static HRESULT __stdcall hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    g_sc = sc;
    if (!g_rt) {
        DXGI_SWAP_CHAIN_DESC d{};
        sc->GetDesc(&d);
        g_sw = d.BufferDesc.Width;
        g_sh = d.BufferDesc.Height;
        MakeRT();
    }
    if (g_rt) Draw();
    return oPresent(sc, sync, flags);
}
static HRESULT __stdcall hkResize(IDXGISwapChain* sc, UINT a, UINT b, UINT c, DXGI_FORMAT f, UINT fl) {
    if (g_rt) { g_rt->Release(); g_rt = nullptr; }
    return oResize(sc, a, b, c, f, fl);
}
static void Unhook() {
    if (!g_vt || !g_hooked) return;
    DWORD old = 0;
    if (!VirtualProtect(g_vt, 14 * sizeof(void*), PAGE_READWRITE, &old)) return;
    g_vt[8] = (void*)oPresent;
    g_vt[13] = (void*)oResize;
    VirtualProtect(g_vt, 14 * sizeof(void*), old, &old);
    g_hooked = false;
}
static bool Hook() {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"v_dummy";
    RegisterClassExW(&wc);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"v", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 100;
    sd.BufferDesc.Height = 100;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = h;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx))) return false;

    g_vt = *(void***)sc;
    oPresent = (Present_t)g_vt[8];
    oResize = (Resize_t)g_vt[13];
    DWORD old = 0;
    VirtualProtect(g_vt, 14 * sizeof(void*), PAGE_READWRITE, &old);
    g_vt[8] = (void*)hkPresent;
    g_vt[13] = (void*)hkResize;
    VirtualProtect(g_vt, 14 * sizeof(void*), old, &old);

    ctx->Release();
    dev->Release();
    sc->Release();
    DestroyWindow(h);
    g_hooked = true;
    return true;
}

/* ---------- resolve ---------- */
static void Resolve() {
    if (!g_base) {
        g_base = (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
        if (!g_base) return;
    }
    if (Gom) return;

    auto* dos = (IMAGE_DOS_HEADER*)g_base;
    auto* nt = (IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    size_t size = nt->OptionalHeader.SizeOfImage;

    static const unsigned char s1[] = { 0x48,0x8B,0x05,0,0,0,0,0x48,0x8B,0x88 };
    static const unsigned char s2[] = { 0x48,0x8B,0x0D,0,0,0,0,0x48,0x8B,0x91 };
    static const char msk[] = "xxx????xxx";

    for (int k = 0; k < 2 && !Gom; k++) {
        const unsigned char* sig = k ? s2 : s1;
        for (uintptr_t a = g_base; a + 10 < g_base + size; a++) {
            bool ok = true;
            for (int j = 0; j < 10; j++) {
                if (msk[j] == 'x' && *(unsigned char*)(a + j) != sig[j]) { ok = false; break; }
            }
            if (!ok) continue;
            int32_t rel = RD<int32_t>(a + 3);
            Gom = a + 7 + rel;
            break;
        }
    }
    if (!Gom) return;
    EachObject([](uintptr_t go) {
        if (!TrCls) TrCls = ClassOf(go, "Transform");
        if (!CamCls) CamCls = ClassOf(go, "Camera");
    });
}

/* ---------- cleanup ---------- */
static void Cleanup() {
    Unhook();
    Sleep(150);
    if (g_vanish) {
        Cam c{};
        if (GetCam(c)) SetVanish(false, c);
    }
    if (g_rt) { g_rt->Release(); g_rt = nullptr; }
    if (g_tS) { g_tS->Release(); g_tS = nullptr; }
    if (g_TL) { g_TL->Release(); g_TL = nullptr; }
    if (g_dw) { g_dw->Release(); g_dw = nullptr; }
    if (g_fac) { g_fac->Release(); g_fac = nullptr; }
    CoUninitialize();
    FreeLibraryAndExitThread(g_mod, 0);
}

/* ---------- boot ---------- */
static DWORD WINAPI Boot(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_fac);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    if (g_dw) {
        g_dw->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.f, L"hu", &g_tS);
        g_dw->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.f, L"hu", &g_TL);
    }
    Hook();
    Resolve();
    Sleep(400);

    DWORD lastTry = 0;
    while (true) {
        if (!Gom) {
            DWORD now = GetTickCount();
            if (now - lastTry > 2000) { lastTry = now; Resolve(); }
        }
        if (g_cap) {
            for (int vk = 1; vk < 0xFF; vk++) {
                if (Hit(vk)) { g_cap->vk = vk; g_cap = nullptr; break; }
            }
        } else {
            if (Hit(VK_F10)) g_menu = !g_menu;
            if (Hit(VK_F1)) g_esp = !g_esp;
            if (Hit(VK_F2)) g_radar = !g_radar;
            if (Hit(VK_F4)) {
                Cam c{};
                if (GetCam(c)) SetVanish(!g_vanish, c);
            }
            if (g_menu) {
                int n = PageCount();
                if (Hit(VK_TAB)) { g_page = !g_page; g_sel = 0; }
                if (Hit(VK_UP)) { g_sel--; if (g_sel < 0) g_sel = n - 1; }
                if (Hit(VK_DOWN)) { g_sel++; if (g_sel >= n) g_sel = 0; }
                if (Hit(VK_LEFT)) Adjust(PageRows()[g_sel], -1);
                if (Hit(VK_RIGHT)) Adjust(PageRows()[g_sel], 1);
                if (Hit(VK_RETURN)) Adjust(PageRows()[g_sel], 1);
                if (Hit(VK_F7)) g_cap = (Bind*)PageRows()[g_sel].p;
            }
            if (Hit(VK_END)) Cleanup();
        }
        g_objs = 0;
        Sleep(15);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_mod = mod;
        DisableThreadLibraryCalls(mod);
        CreateThread(nullptr, 0, Boot, nullptr, 0, nullptr);
    }
    return TRUE;
}
