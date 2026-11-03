// Android arm64 internal .so — il2cpp ESP + ImGui (GLES3). Resolves everything by name at runtime (no static offsets).
// account-ban risk on live servers, test on a throwaway account.
#include <dlfcn.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "dobby.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "esp", __VA_ARGS__)

// ---------------------------------------------------------------- il2cpp api
static void* (*il2cpp_domain_get)();
static void* (*il2cpp_thread_attach)(void*);
static void** (*il2cpp_domain_get_assemblies)(void*, size_t*);
static void* (*il2cpp_assembly_get_image)(void*);
static void* (*il2cpp_class_from_name)(void*, const char*, const char*);
static void* (*il2cpp_class_get_method_from_name)(void*, const char*, int);
static void* (*il2cpp_class_get_field_from_name)(void*, const char*);
static size_t (*il2cpp_field_get_offset)(void*);
static void* (*il2cpp_class_get_type)(void*);
static void* (*il2cpp_type_get_object)(void*);
static uint32_t (*il2cpp_gchandle_new)(void*, bool);
static void (*il2cpp_gchandle_free)(uint32_t);

struct Vec3 { float x, y, z; };
struct Touch { int id; float px, py, rx, ry, dx, dy, dt; int tap, phase, type; float pr, mpr, rad, radv, alt, azi; };

struct Meth { void* mi = nullptr; void* fn = nullptr; };
template <class R, class... A> static R call(const Meth& m, A... a) { return ((R(*)(A..., void*))m.fn)(a..., m.mi); }

static void* FindClass(const char* ns, const char* name) {
    size_t n = 0;
    void** as = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &n);
    for (size_t i = 0; i < n; i++) {
        void* k = il2cpp_class_from_name(il2cpp_assembly_get_image(as[i]), ns, name);
        if (k) return k;
    }
    return nullptr;
}
static Meth GetM(void* k, const char* name, int argc) {
    Meth m;
    if (!k) return m;
    m.mi = il2cpp_class_get_method_from_name(k, name, argc);
    if (m.mi) m.fn = *(void**)m.mi;
    if (!m.fn) LOG("method miss: %s/%d", name, argc);
    return m;
}
static int GetF(void* k, const char* name) {
    void* f = k ? il2cpp_class_get_field_from_name(k, name) : nullptr;
    if (!f) { LOG("field miss: %s", name); return -1; }
    return (int)il2cpp_field_get_offset(f);
}
template <class T> static T rd(void* base, int off) { return base && off >= 0 ? *(T*)((char*)base + off) : T{}; }
static bool alive(void* o) { return o && *(void**)((char*)o + 0x10); } // UnityEngine.Object::m_CachedPtr

static std::string U8(void* s) {
    if (!s) return "?";
    int len = *(int*)((char*)s + 0x10);
    const uint16_t* c = (const uint16_t*)((char*)s + 0x14);
    std::string o;
    for (int i = 0; i < len && i < 31; i++) {
        uint16_t ch = c[i];
        if (ch < 0x80) o += (char)ch;
        else if (ch < 0x800) { o += (char)(0xC0 | ch >> 6); o += (char)(0x80 | (ch & 63)); }
        else { o += (char)(0xE0 | ch >> 12); o += (char)(0x80 | ((ch >> 6) & 63)); o += (char)(0x80 | (ch & 63)); }
    }
    return o;
}

// ---------------------------------------------------------------- resolved game api
static Meth mFindObjs, mTouchCount, mGetTouch, mGetTransform, mGetPos, mW2S, mPixW, mPixH;
static Meth mPC_Photon, mPC_Health, mPC_IsDeath, mPC_Player, mPC_MainCam, mPC_Anim, mPV_IsMine, mRP_Nick;
static void* kPlayerController;
static int fAnim_Skel, fAnim_Ct;
static int fSk_Bone[7]; // head chest pelvis lUpArm rUpArm lThigh rThigh
static const char* kBoneNames[7] = { "_headBone", "_chestBone", "_pelvisBone", "_leftUpperArm", "_rightUpperArm", "_leftThigh", "_rightThigh" };

// ---------------------------------------------------------------- settings
static struct {
    bool esp = true, box = true, skel = true, hp = true, name = true, dist = true, snap = false, teamCheck = true;
    float maxDist = 300.f, thick = 1.5f, uiScale = 2.4f;
    float colEnemy[3] = { 1.f, 0.25f, 0.25f }, colTeam[3] = { 0.25f, 0.6f, 1.f };
} cfg;
struct Opt { const char* k; int t; void* p; };
static Opt opts[] = {
    { "esp", 0, &cfg.esp }, { "box", 0, &cfg.box }, { "skeleton", 0, &cfg.skel }, { "health", 0, &cfg.hp },
    { "name", 0, &cfg.name }, { "distance", 0, &cfg.dist }, { "snaplines", 0, &cfg.snap }, { "teamCheck", 0, &cfg.teamCheck },
    { "maxDist", 1, &cfg.maxDist }, { "thickness", 1, &cfg.thick }, { "uiScale", 1, &cfg.uiScale },
    { "colEnemy", 2, cfg.colEnemy }, { "colTeam", 2, cfg.colTeam },
};
static std::string cfgPath() {
    char buf[256] = {}; FILE* f = fopen("/proc/self/cmdline", "r");
    if (f) { fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
    return std::string("/data/data/") + buf + "/files/esp.json";
}
static void SaveCfg() {
    FILE* f = fopen(cfgPath().c_str(), "w"); if (!f) return;
    fputs("{\n", f);
    for (size_t i = 0; i < sizeof(opts) / sizeof(*opts); i++) {
        auto& o = opts[i]; fprintf(f, "  \"%s\": ", o.k);
        if (o.t == 0) fputs(*(bool*)o.p ? "true" : "false", f);
        else if (o.t == 1) fprintf(f, "%g", *(float*)o.p);
        else fprintf(f, "[%g, %g, %g]", ((float*)o.p)[0], ((float*)o.p)[1], ((float*)o.p)[2]);
        fputs(i + 1 < sizeof(opts) / sizeof(*opts) ? ",\n" : "\n", f);
    }
    fputs("}\n", f); fclose(f);
}
static void LoadCfg() {
    FILE* f = fopen(cfgPath().c_str(), "r"); if (!f) return;
    std::string s; char b[512]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    for (auto& o : opts) {
        size_t p = s.find(std::string("\"") + o.k + "\""); if (p == std::string::npos) continue;
        p = s.find(':', p); if (p == std::string::npos) continue;
        const char* v = s.c_str() + p + 1; while (*v == ' ') v++;
        if (o.t == 0) *(bool*)o.p = !strncmp(v, "true", 4);
        else if (o.t == 1) *(float*)o.p = strtof(v, nullptr);
        else { if (*v == '[') v++; for (int i = 0; i < 3; i++) { char* e; ((float*)o.p)[i] = strtof(v, &e); v = e; while (*v == ',' || *v == ' ') v++; } }
    }
}

// ---------------------------------------------------------------- shared snapshot (game thread -> render thread)
struct Pt { float x, y; bool ok; };
struct Ent { Pt head, feet, bone[7]; int hp; bool enemy; float dist; char name[32]; };
static std::mutex gMu;
static std::vector<Ent> gEnts;
static float gCamW = 1, gCamH = 1;
static std::atomic<float> gTx{0}, gTy{0};
static std::atomic<bool> gTDown{false};

// ---------------------------------------------------------------- game-thread tick
static uint32_t gArrH = 0; static void* gArr = nullptr; static double gLastRefresh = 0, gLastTick = 0;
static double Now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static Vec3 PosOf(void* comp) { return call<Vec3>(mGetPos, call<void*>(mGetTransform, comp)); }

static int TeamOf(void* pc) { // 1 = ct skeleton active, 2 = tr, 0 = unknown. assumes PlayerAnimatorController._skeleton == active one
    void* anim = call<void*>(mPC_Anim, pc);
    if (!alive(anim)) return 0;
    void* sk = rd<void*>(anim, fAnim_Skel);
    if (!sk) return 0;
    return sk == rd<void*>(anim, fAnim_Ct) ? 1 : 2;
}
static void* SkelOf(void* pc) { void* a = call<void*>(mPC_Anim, pc); return alive(a) ? rd<void*>(a, fAnim_Skel) : nullptr; }

static Pt W2S(void* cam, Vec3 p) {
    Vec3 s = call<Vec3>(mW2S, cam, p);
    return { s.x, s.y, s.z > 0.01f };
}

static void Tick() {
    double now = Now();
    if (now - gLastTick < 0.008) return; // one tick per frame, Update fires per PlayerController
    gLastTick = now;

    // touch -> imgui mouse
    if (mTouchCount.fn && call<int>(mTouchCount) > 0) {
        Touch t = call<Touch>(mGetTouch, 0);
        gTx = t.px; gTy = t.py; gTDown = t.phase != 3 && t.phase != 4;
    } else gTDown = false;

    if (now - gLastRefresh > 0.5) {
        gLastRefresh = now;
        if (gArrH) il2cpp_gchandle_free(gArrH);
        gArr = call<void*>(mFindObjs, il2cpp_type_get_object(il2cpp_class_get_type(kPlayerController)));
        gArrH = gArr ? il2cpp_gchandle_new(gArr, false) : 0;
    }
    std::vector<Ent> out;
    if (gArr && cfg.esp) {
        size_t n = *(size_t*)((char*)gArr + 0x18);
        void** items = (void**)((char*)gArr + 0x20);
        void* local = nullptr;
        for (size_t i = 0; i < n; i++) {
            void* pc = items[i]; if (!alive(pc)) continue;
            void* pv = call<void*>(mPC_Photon, pc);
            if (alive(pv) && call<bool>(mPV_IsMine, pv)) { local = pc; break; }
        }
        void* cam = local ? call<void*>(mPC_MainCam, local) : nullptr;
        if (local && alive(cam)) {
            gCamW = (float)call<int>(mPixW, cam); gCamH = (float)call<int>(mPixH, cam);
            int myTeam = TeamOf(local);
            Vec3 me = PosOf(cam);
            for (size_t i = 0; i < n; i++) {
                void* pc = items[i];
                if (pc == local || !alive(pc) || call<bool>(mPC_IsDeath, pc)) continue;
                void* sk = SkelOf(pc); if (!sk) continue;
                int team = TeamOf(pc);
                bool enemy = !(myTeam && team == myTeam);
                if (cfg.teamCheck && !enemy) continue;
                Vec3 feet = PosOf(pc);
                float d = sqrtf((feet.x - me.x) * (feet.x - me.x) + (feet.y - me.y) * (feet.y - me.y) + (feet.z - me.z) * (feet.z - me.z));
                if (d > cfg.maxDist) continue;
                Ent e{}; e.enemy = enemy; e.dist = d; e.hp = call<int>(mPC_Health, pc);
                e.feet = W2S(cam, feet);
                bool bad = !e.feet.ok;
                for (int b = 0; b < 7; b++) {
                    void* rb = rd<void*>(sk, fSk_Bone[b]);
                    e.bone[b] = alive(rb) ? W2S(cam, PosOf(rb)) : Pt{ 0, 0, false };
                }
                e.head = e.bone[0];
                if (!e.head.ok) bad = true;
                if (bad) continue;
                void* pl = call<void*>(mPC_Player, pc);
                std::string nm = pl ? U8(call<void*>(mRP_Nick, pl)) : "?";
                strncpy(e.name, nm.c_str(), 31);
                out.push_back(e);
            }
        }
    }
    std::lock_guard<std::mutex> l(gMu);
    gEnts.swap(out);
}

static void (*orig_PC_Update)(void*, void*);
static void hk_PC_Update(void* self, void* mi) { orig_PC_Update(self, mi); Tick(); }

// ---------------------------------------------------------------- render thread
static bool gInit = false, gMenu = true;
static EGLBoolean (*orig_swap)(EGLDisplay, EGLSurface);

static ImU32 Col(const float* c, float a = 1.f) { return ImGui::ColorConvertFloat4ToU32({ c[0], c[1], c[2], a }); }

static void DrawESP(ImDrawList* dl, ImVec2 disp) {
    std::vector<Ent> ents; { std::lock_guard<std::mutex> l(gMu); ents = gEnts; }
    float rx = disp.x / gCamW, ry = disp.y / gCamH;
    auto P = [&](const Pt& p) { return ImVec2(p.x * rx, (gCamH - p.y) * ry); }; // unity screen origin = bottom-left
    for (auto& e : ents) {
        const float* c = e.enemy ? cfg.colEnemy : cfg.colTeam; ImU32 col = Col(c);
        ImVec2 h = P(e.head), f = P(e.feet);
        float top = h.y - fabsf(f.y - h.y) * 0.14f, hh = f.y - top, w = hh * 0.5f, cx = (h.x + f.x) * 0.5f;
        if (hh < 4) continue;
        if (cfg.box) {
            dl->AddRect({ cx - w / 2 - 1, top - 1 }, { cx + w / 2 + 1, f.y + 1 }, IM_COL32(0, 0, 0, 200), 0, 0, cfg.thick + 1);
            dl->AddRect({ cx - w / 2, top }, { cx + w / 2, f.y }, col, 0, 0, cfg.thick);
        }
        if (cfg.skel) {
            static const int L[][2] = { {0,1},{1,2},{1,3},{1,4},{2,5},{2,6} };
            for (auto& l : L) if (e.bone[l[0]].ok && e.bone[l[1]].ok) dl->AddLine(P(e.bone[l[0]]), P(e.bone[l[1]]), col, cfg.thick);
        }
        if (cfg.hp) {
            float t = fminf(fmaxf(e.hp / 100.f, 0.f), 1.f), x = cx - w / 2 - 5;
            dl->AddRectFilled({ x - 1, top - 1 }, { x + 3, f.y + 1 }, IM_COL32(0, 0, 0, 200));
            dl->AddRectFilled({ x, f.y - hh * t }, { x + 2, f.y }, IM_COL32((int)(255 * (1 - t)), (int)(255 * t), 0, 255));
        }
        float ty = top - ImGui::GetFontSize() - 2;
        if (cfg.name) { ImVec2 s = ImGui::CalcTextSize(e.name); dl->AddText({ cx - s.x / 2, ty }, IM_COL32(255, 255, 255, 255), e.name); }
        if (cfg.dist) { char b[16]; snprintf(b, sizeof b, "%dm", (int)e.dist); ImVec2 s = ImGui::CalcTextSize(b); dl->AddText({ cx - s.x / 2, f.y + 2 }, IM_COL32(220, 220, 220, 255), b); }
        if (cfg.snap) dl->AddLine({ disp.x / 2, disp.y }, { cx, f.y }, col, 1.f);
    }
}

static void DrawMenu() {
    ImGui::SetNextWindowPos({ 20, 20 }, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({ 520, 420 }, ImGuiCond_FirstUseEver);
    ImGui::Begin("esp", &gMenu, ImGuiWindowFlags_NoCollapse);
    if (ImGui::BeginTabBar("t")) {
        if (ImGui::BeginTabItem("ESP")) {
            ImGui::Checkbox("enable", &cfg.esp); ImGui::Checkbox("box", &cfg.box); ImGui::Checkbox("skeleton", &cfg.skel);
            ImGui::Checkbox("health", &cfg.hp); ImGui::Checkbox("name", &cfg.name); ImGui::Checkbox("distance", &cfg.dist);
            ImGui::Checkbox("snaplines", &cfg.snap); ImGui::Checkbox("team check", &cfg.teamCheck);
            ImGui::SliderFloat("max dist", &cfg.maxDist, 20, 600, "%.0f"); ImGui::SliderFloat("thickness", &cfg.thick, 1, 4);
            ImGui::ColorEdit3("enemy", cfg.colEnemy, ImGuiColorEditFlags_NoInputs); ImGui::ColorEdit3("team", cfg.colTeam, ImGuiColorEditFlags_NoInputs);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("CONFIG")) {
            ImGui::SliderFloat("ui scale", &cfg.uiScale, 1.f, 4.f);
            if (ImGui::Button("save")) SaveCfg(); ImGui::SameLine(); if (ImGui::Button("load")) LoadCfg();
            ImGui::TextDisabled("%s", cfgPath().c_str());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

static EGLBoolean hk_swap(EGLDisplay dpy, EGLSurface surf) {
    EGLint w = 0, h = 0; eglQuerySurface(dpy, surf, EGL_WIDTH, &w); eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 100 || h < 100) return orig_swap(dpy, surf);
    static double last = 0;
    if (!gInit) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr;
        ImGui::StyleColorsDark(); ImGui::GetStyle().ScaleAllSizes(2.f);
        ImGui_ImplOpenGL3_Init("#version 300 es");
        LoadCfg(); gInit = true; last = Now();
    }
    ImGuiIO& io = ImGui::GetIO();
    double n = Now(); io.DeltaTime = (float)fmax(n - last, 1e-4); last = n;
    io.DisplaySize = { (float)w, (float)h }; io.FontGlobalScale = cfg.uiScale / 2.f;
    ImGui_ImplOpenGL3_NewFrame();
    float rx = w / gCamW, ry = h / gCamH;
    io.AddMousePosEvent(gTx * rx, (gCamH - gTy) * ry); io.AddMouseButtonEvent(0, gTDown);
    ImGui::NewFrame();
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    if (cfg.esp) DrawESP(bg, io.DisplaySize);
    if (!gMenu) { // tiny reopen button, top-right
        ImGui::SetNextWindowPos({ io.DisplaySize.x - 90, 10 });
        ImGui::Begin("##b", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize);
        if (ImGui::Button("MENU")) gMenu = true;
        ImGui::End();
    } else DrawMenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return orig_swap(dpy, surf);
}

// ---------------------------------------------------------------- bootstrap
static void* MainThread(void*) {
    void* h = nullptr;
    while (!(h = dlopen("libil2cpp.so", RTLD_NOLOAD))) usleep(200000);
#define SYM(n) n = (decltype(n))dlsym(h, #n); if (!n) { LOG("sym miss " #n); return nullptr; }
    SYM(il2cpp_domain_get) SYM(il2cpp_thread_attach) SYM(il2cpp_domain_get_assemblies) SYM(il2cpp_assembly_get_image)
    SYM(il2cpp_class_from_name) SYM(il2cpp_class_get_method_from_name) SYM(il2cpp_class_get_field_from_name)
    SYM(il2cpp_field_get_offset) SYM(il2cpp_class_get_type) SYM(il2cpp_type_get_object) SYM(il2cpp_gchandle_new) SYM(il2cpp_gchandle_free)
    // wait for game assemblies
    while (true) { void* d = il2cpp_domain_get(); if (d) { il2cpp_thread_attach(d); if (FindClass("", "PlayerController")) break; } sleep(1); }
    sleep(3);

    kPlayerController = FindClass("", "PlayerController");
    void* kObj = FindClass("UnityEngine", "Object"); void* kComp = FindClass("UnityEngine", "Component");
    void* kTr = FindClass("UnityEngine", "Transform"); void* kCam = FindClass("UnityEngine", "Camera");
    void* kIn = FindClass("UnityEngine", "Input"); void* kPV = FindClass("Photon.Pun", "PhotonView");
    void* kRP = FindClass("Photon.Realtime", "Player"); void* kAnim = FindClass("ScarletWork.Photon.TPS", "PlayerAnimatorController");
    void* kSk = FindClass("ScarletWork.Game", "PlayerSkeleton");

    mFindObjs = GetM(kObj, "FindObjectsOfType", 1); mGetTransform = GetM(kComp, "get_transform", 0); mGetPos = GetM(kTr, "get_position", 0);
    mW2S = GetM(kCam, "WorldToScreenPoint", 1); mPixW = GetM(kCam, "get_pixelWidth", 0); mPixH = GetM(kCam, "get_pixelHeight", 0);
    mTouchCount = GetM(kIn, "get_touchCount", 0); mGetTouch = GetM(kIn, "GetTouch", 1);
    mPC_Photon = GetM(kPlayerController, "get_Photon", 0); mPC_Health = GetM(kPlayerController, "get_Health", 0);
    mPC_IsDeath = GetM(kPlayerController, "get_IsDeath", 0); mPC_Player = GetM(kPlayerController, "get_Player", 0);
    mPC_MainCam = GetM(kPlayerController, "get_MainCamera", 0); mPC_Anim = GetM(kPlayerController, "get_PlayerAnimatorController", 0);
    mPV_IsMine = GetM(kPV, "get_IsMine", 0); mRP_Nick = GetM(kRP, "get_NickName", 0);
    fAnim_Skel = GetF(kAnim, "_skeleton"); fAnim_Ct = GetF(kAnim, "_ctSkeleton");
    for (int i = 0; i < 7; i++) fSk_Bone[i] = GetF(kSk, kBoneNames[i]);

    Meth upd = GetM(kPlayerController, "Update", 0);
    if (!upd.fn || !mFindObjs.fn || !mW2S.fn || !mPC_Anim.fn || fAnim_Skel < 0) { LOG("resolve failed, abort"); return nullptr; }
    DobbyHook(upd.fn, (void*)hk_PC_Update, (void**)&orig_PC_Update);

    void* egl = dlopen("libEGL.so", RTLD_NOW);
    void* swap = egl ? dlsym(egl, "eglSwapBuffers") : nullptr;
    if (!swap) { LOG("no eglSwapBuffers"); return nullptr; }
    DobbyHook(swap, (void*)hk_swap, (void**)&orig_swap);
    LOG("hooked");
    return nullptr;
}

__attribute__((constructor)) static void ctor() { pthread_t t; pthread_create(&t, nullptr, MainThread, nullptr); }
