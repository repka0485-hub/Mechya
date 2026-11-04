// libesp.so — Counter Flame (com.ScarletWork.CounterFlame), Unity il2cpp / Photon, arm64
// Реконструкция по строкам libesp.so: исходника у меня не было. Всё, что не подтверждено — FILL:/лог "miss".
// build (NDK r25+): см. build.sh внизу файла.

#include <android/log.h>
#include <EGL/egl.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <errno.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

extern "C" void A64HookFunction(void* symbol, void* replace, void** result);

// ───────────────────────────── config ─────────────────────────────
#define PKG          "com.ScarletWork.CounterFlame"
#define NS_GAME      "ScarletWork.Game"
#define LOG_TAG      "esp"
#define PLAYER_H     1.8f      // FILL: рост для бокса (голова = pos + H), если у скелета нет головы
#define MAX_HP       100.0f    // FILL: максимум HP для полоски
#define STALE_MS     150       // снапшот старше — не рисуем
#define DROP_MS      2000      // старше — удаляем
#define PHASE2_FRAMES 600      // ~10с рендера до первого касания il2cpp

// ───────────────────────────── time ─────────────────────────────
static uint64_t NowMs() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}
static const uint64_t g_t0 = NowMs();

// ───────────────────────────── logger ─────────────────────────────
// logcat (tag "esp") + файл <data>/files/esp.log (ротация 1MB -> .log.1) + кольцо на 512 строк для вкладки LOG.
enum LogLevel { LV_D = 0, LV_I, LV_W, LV_E };
static const char LV_CH[] = {'D', 'I', 'W', 'E'};

namespace Log {
    struct Line { uint64_t ms; uint32_t seq; uint8_t lv; char msg[200]; };
    static const int RING = 512;
    static std::mutex mtx;
    static Line ring[RING];
    static uint32_t seq = 0;                 // всего строк; индекс в кольце = seq % RING
    static FILE* fp = nullptr;
    static size_t fbytes = 0;
    static std::string path;
    static std::atomic<int> minLevel{LV_I};
    static std::atomic<uint32_t> nWarn{0}, nErr{0};

    static void fmtLine(char* out, size_t n, const Line& l) {
        snprintf(out, n, "%7.3f [%c] [%05u] %s\n", (l.ms - g_t0) / 1000.0, LV_CH[l.lv], l.seq, l.msg);
    }
    static void openFile() {                 // под mtx
        if (path.empty()) return;
        fp = fopen(path.c_str(), "w");
        fbytes = 0;
        if (!fp) { __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "log open failed: %s (errno %d)", path.c_str(), errno); return; }
        // дописываем всё, что накопилось до init
        uint32_t from = seq > (uint32_t)RING ? seq - RING : 0;
        char buf[320];
        for (uint32_t i = from; i < seq; i++) {
            fmtLine(buf, sizeof buf, ring[i % RING]);
            fbytes += fwrite(buf, 1, strlen(buf), fp);
        }
        fflush(fp);
    }
    // dir = каталог с правом записи; пробуем по очереди
    static void init(const char* const* dirs, int n) {
        std::lock_guard<std::mutex> g(mtx);
        for (int i = 0; i < n; i++) {
            std::string p = std::string(dirs[i]) + "/esp.log";
            FILE* t = fopen(p.c_str(), "a");
            if (t) { fclose(t); path = p; break; }
        }
        openFile();
    }
    static void write(int lv, const char* f, ...) __attribute__((format(printf, 2, 3)));
    static void write(int lv, const char* f, ...) {
        if (lv < minLevel.load(std::memory_order_relaxed)) return;
        Line l; l.ms = NowMs(); l.lv = (uint8_t)lv;
        va_list ap; va_start(ap, f); vsnprintf(l.msg, sizeof l.msg, f, ap); va_end(ap);
        static const int AP[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
        __android_log_print(AP[lv], LOG_TAG, "%s", l.msg);
        if (lv == LV_W) nWarn++; else if (lv == LV_E) nErr++;
        std::lock_guard<std::mutex> g(mtx);
        l.seq = seq;
        ring[seq % RING] = l; seq++;
        if (!fp) return;
        char buf[320]; fmtLine(buf, sizeof buf, l);
        fbytes += fwrite(buf, 1, strlen(buf), fp);
        fflush(fp);
        if (fbytes > (1u << 20)) {
            fclose(fp); fp = nullptr;
            std::string old = path + ".1";
            rename(path.c_str(), old.c_str());
            openFile();
        }
    }
    static void clear() { std::lock_guard<std::mutex> g(mtx); seq = 0; }
}
#define LOGD(...) Log::write(LV_D, __VA_ARGS__)
#define LOGI(...) Log::write(LV_I, __VA_ARGS__)
#define LOGW(...) Log::write(LV_W, __VA_ARGS__)
#define LOGE(...) Log::write(LV_E, __VA_ARGS__)
// не чаще раза в ms (на call-site) — для per-frame / per-update путей
#define LOG_EVERY(ms, lv, ...) do { static std::atomic<uint64_t> _t{0}; uint64_t _n = NowMs(), _p = _t.load(); \
    if (_n - _p >= (uint64_t)(ms) && _t.compare_exchange_strong(_p, _n)) Log::write(lv, __VA_ARGS__); } while (0)
#define LOG_ONCE(lv, ...) do { static std::atomic<bool> _d{false}; if (!_d.exchange(true)) Log::write(lv, __VA_ARGS__); } while (0)

// ───────────────────────────── config / json ─────────────────────────────
struct Cfg {
    bool enemy = true, team = false, teamCheck = true;
    bool box = true, skeleton = true, health = true, name = true, dist = true, snaplines = false;
    float maxDist = 300.f, uiScale = 2.5f;
    float colEnemy[4] = {1.f, 0.25f, 0.25f, 1.f};
    float colTeam[4]  = {0.25f, 0.8f, 1.f, 1.f};
    int   logLevel = LV_I;
};
static Cfg g_cfg;
static std::string g_dataDir, g_cfgPath;

static bool JNum(const char* s, const char* key, float* out) {
    char k[48]; snprintf(k, sizeof k, "\"%s\":", key);
    const char* p = strstr(s, k); if (!p) return false;
    *out = strtof(p + strlen(k), nullptr); return true;
}
static void CfgSave() {
    if (g_cfgPath.empty()) return;
    FILE* f = fopen(g_cfgPath.c_str(), "w");
    if (!f) { LOGE("cfg save failed: %s (errno %d)", g_cfgPath.c_str(), errno); return; }
    const Cfg& c = g_cfg;
    fprintf(f, "{\"enemy\":%d,\"team\":%d,\"teamCheck\":%d,\"box\":%d,\"skeleton\":%d,\"health\":%d,\"name\":%d,\"dist\":%d,\"snaplines\":%d,"
               "\"maxDist\":%.1f,\"uiScale\":%.2f,\"logLevel\":%d,"
               "\"colEnemy\":[%.3f,%.3f,%.3f,%.3f],\"colTeam\":[%.3f,%.3f,%.3f,%.3f]}\n",
            c.enemy, c.team, c.teamCheck, c.box, c.skeleton, c.health, c.name, c.dist, c.snaplines,
            c.maxDist, c.uiScale, c.logLevel,
            c.colEnemy[0], c.colEnemy[1], c.colEnemy[2], c.colEnemy[3], c.colTeam[0], c.colTeam[1], c.colTeam[2], c.colTeam[3]);
    fclose(f);
    LOGI("cfg saved: %s", g_cfgPath.c_str());
}
static void CfgLoad() {
    if (g_cfgPath.empty()) return;
    FILE* f = fopen(g_cfgPath.c_str(), "r");
    if (!f) { LOGI("cfg: нет %s, дефолты", g_cfgPath.c_str()); return; }
    char buf[1024]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    Cfg& c = g_cfg; float v;
    #define B(k) if (JNum(buf, #k, &v)) c.k = v != 0.f;
    B(enemy) B(team) B(teamCheck) B(box) B(skeleton) B(health) B(name) B(dist) B(snaplines)
    #undef B
    if (JNum(buf, "maxDist", &v)) c.maxDist = v;
    if (JNum(buf, "uiScale", &v)) c.uiScale = v;
    if (JNum(buf, "logLevel", &v)) c.logLevel = (int)v;
    auto arr = [&](const char* key, float* o) {
        const char* p = strstr(buf, key); if (!p || !(p = strchr(p, '['))) return;
        sscanf(p, "[%f,%f,%f,%f]", o, o + 1, o + 2, o + 3);
    };
    arr("\"colEnemy\"", c.colEnemy); arr("\"colTeam\"", c.colTeam);
    Log::minLevel = c.logLevel;
    LOGI("cfg loaded: %s", g_cfgPath.c_str());
}

// ───────────────────────────── il2cpp ─────────────────────────────
struct Vec3 { float x, y, z; };
struct Touch { int fingerId; float px, py; float rx, ry; float dx, dy; float dt; int tapCount; int phase; int type; float pressure, maxPressure, radius, radiusVar, altitude, azimuth; }; // Unity 2019+ layout, 68 байт

static void* (*il2cpp_domain_get)();
static void** (*il2cpp_domain_get_assemblies)(void*, size_t*);
static void* (*il2cpp_assembly_get_image)(void*);
static void* (*il2cpp_class_from_name)(void*, const char*, const char*);
static void* (*il2cpp_class_get_method_from_name)(void*, const char*, int);
static void* (*il2cpp_class_get_field_from_name)(void*, const char*);
static size_t (*il2cpp_field_get_offset)(void*);
static void* (*il2cpp_thread_attach)(void*);
static const void* (*il2cpp_method_get_return_type)(void*);
static int (*il2cpp_type_get_type)(const void*);

static void* g_il2cpp = nullptr;
static void* g_domain = nullptr;

static bool ResolveApi() {
    #define SYM(n) do { *(void**)&n = dlsym(g_il2cpp, #n); if (!n) { LOGE("sym miss %s", #n); ok = false; } } while (0)
    bool ok = true;
    SYM(il2cpp_domain_get); SYM(il2cpp_domain_get_assemblies); SYM(il2cpp_assembly_get_image);
    SYM(il2cpp_class_from_name); SYM(il2cpp_class_get_method_from_name); SYM(il2cpp_class_get_field_from_name);
    SYM(il2cpp_field_get_offset); SYM(il2cpp_thread_attach);
    *(void**)&il2cpp_method_get_return_type = dlsym(g_il2cpp, "il2cpp_method_get_return_type"); // опционально
    *(void**)&il2cpp_type_get_type = dlsym(g_il2cpp, "il2cpp_type_get_type");
    #undef SYM
    return ok;
}

static void* FindClass(const char* ns, const char* name) {
    size_t n = 0; void** as = il2cpp_domain_get_assemblies(g_domain, &n);
    for (size_t i = 0; i < n; i++) {
        void* img = il2cpp_assembly_get_image(as[i]);
        if (!img) continue;
        if (void* k = il2cpp_class_from_name(img, ns, name)) return k;
    }
    return nullptr;
}

struct Method { void* mi = nullptr; void* fn = nullptr; int ret = -1; explicit operator bool() const { return fn != nullptr; } };

static Method GetMethod(void* klass, const char* cls, const char* name, int argc) {
    Method m;
    if (!klass) { LOGW("method skip: %s/%s (класса нет)", cls, name); return m; }
    m.mi = il2cpp_class_get_method_from_name(klass, name, argc);
    if (!m.mi) { LOGW("method miss: %s.%s/%d", cls, name, argc); return m; }
    m.fn = *(void**)m.mi;                                   // MethodInfo::methodPointer — первый член
    if (il2cpp_method_get_return_type && il2cpp_type_get_type) m.ret = il2cpp_type_get_type(il2cpp_method_get_return_type(m.mi));
    LOGI("method ok: %s.%s/%d fn=%p ret=0x%x", cls, name, argc, m.fn, m.ret);
    return m;
}
static long GetField(void* klass, const char* cls, const char* name) {
    if (!klass) { LOGW("field skip: %s.%s (класса нет)", cls, name); return -1; }
    void* f = il2cpp_class_get_field_from_name(klass, name);
    if (!f) { LOGW("field miss: %s.%s", cls, name); return -1; }
    long off = (long)il2cpp_field_get_offset(f);
    LOGI("field ok: %s.%s +0x%lx", cls, name, off);
    return off;
}

// il2cpp: instance = fn(self, args..., MethodInfo*); static = fn(args..., MethodInfo*)
template <class R, class... A> static R CallI(const Method& m, void* self, A... a) { return ((R(*)(void*, A..., const void*))m.fn)(self, a..., m.mi); }
template <class R, class... A> static R CallS(const Method& m, A... a)             { return ((R(*)(A..., const void*))m.fn)(a..., m.mi); }

static void Utf16ToAscii(void* s, char* out, size_t n) {   // Il2CppString: int32 len @+0x10, char16 @+0x14
    out[0] = 0; if (!s) return;
    int len = *(int*)((char*)s + 0x10); const char16_t* c = (const char16_t*)((char*)s + 0x14);
    if (len < 0 || len > 256) len = 0;
    size_t j = 0; for (int i = 0; i < len && j + 1 < n; i++) out[j++] = c[i] < 0x80 ? (char)c[i] : '?';
    out[j] = 0;
}

// ───────────────────────────── game bindings ─────────────────────────────
static struct {
    void* kPlayer = nullptr;
    Method health, isDeath, photon, animCtl, getTransform, getPosition, isMine, nick;
    int nickOn = 0;   // 0 = PlayerController, 1 = PhotonView
    Method camMain, w2s, camPixW, touchCount, getTouch;
    long offTeam = -1, offSkel = -1, offCtSkel = -1;
    long offBone[6] = {-1, -1, -1, -1, -1, -1};   // chest, pelvis, lUpperArm, rUpperArm, lThigh, rThigh
    bool ready = false;
} G;
static const char* BONES[6] = {"_chestBone", "_pelvisBone", "_leftUpperArm", "_rightUpperArm", "_leftThigh", "_rightThigh"};

static bool BindGame() {
    void* kPC   = FindClass(NS_GAME, "PlayerController");
    if (!kPC) return false;
    void* kAC   = FindClass(NS_GAME, "PlayerAnimatorController");
    void* kSK   = FindClass(NS_GAME, "PlayerSkeleton");
    void* kPV   = FindClass("Photon.Pun", "PhotonView");
    void* kComp = FindClass("UnityEngine", "Component");
    void* kTr   = FindClass("UnityEngine", "Transform");
    void* kCam  = FindClass("UnityEngine", "Camera");
    void* kIn   = FindClass("UnityEngine", "Input");
    LOGI("classes: PC=%p AC=%p SK=%p PV=%p Comp=%p Tr=%p Cam=%p Input=%p", kPC, kAC, kSK, kPV, kComp, kTr, kCam, kIn);
    G.kPlayer = kPC;

    G.health       = GetMethod(kPC, "PlayerController", "get_Health", 0);
    G.isDeath      = GetMethod(kPC, "PlayerController", "get_IsDeath", 0);
    G.photon       = GetMethod(kPC, "PlayerController", "get_Photon", 0);
    G.animCtl      = GetMethod(kPC, "PlayerController", "get_PlayerAnimatorController", 0);
    G.getTransform = GetMethod(kComp, "Component", "get_transform", 0);
    G.getPosition  = GetMethod(kTr, "Transform", "get_position", 0);
    G.isMine       = GetMethod(kPV, "PhotonView", "get_IsMine", 0);
    G.nick         = GetMethod(kPC, "PlayerController", "get_NickName", 0);
    if (!G.nick) { G.nick = GetMethod(kPV, "PhotonView", "get_NickName", 0); G.nickOn = 1; }   // FILL: если ник тянется иначе (PhotonView.Owner.NickName)
    G.camMain      = GetMethod(kCam, "Camera", "get_main", 0);
    G.w2s          = GetMethod(kCam, "Camera", "WorldToScreenPoint", 1);
    G.camPixW      = GetMethod(kCam, "Camera", "get_pixelWidth", 0);
    G.touchCount   = GetMethod(kIn, "Input", "get_touchCount", 0);
    G.getTouch     = GetMethod(kIn, "Input", "GetTouch", 1);

    G.offTeam   = GetField(kPC, "PlayerController", "team");          // FILL: тип/имя поля команды (читаю как int32)
    G.offSkel   = GetField(kAC, "PlayerAnimatorController", "_skeleton");
    G.offCtSkel = GetField(kAC, "PlayerAnimatorController", "_ctSkeleton");
    for (int i = 0; i < 6; i++) G.offBone[i] = GetField(kSK, "PlayerSkeleton", BONES[i]);

    bool core = G.getTransform && G.getPosition && G.camMain && G.w2s && G.camPixW && G.isDeath;
    if (!core) LOGE("core binding incomplete — ESP не заработает, смотри 'miss' выше");
    if (!G.isMine) LOGW("IsMine miss — локальный игрок не определится, рисую всех");
    if (G.offTeam < 0) LOGW("team miss — teamCheck не работает, все не-local = enemy");
    if (!G.touchCount || !G.getTouch) LOGW("Input miss — меню не получит тач");
    G.ready = core;
    return core;
}

// ───────────────────────────── snapshot ─────────────────────────────
struct Snap {
    uint64_t seen = 0;
    ImVec2 top, bot;           // голова/ступни на экране
    ImVec2 bone[6]; bool boneOk[6] = {}; bool skel = false;
    float hp = 0, dist = 0; int team = 0; bool enemy = true; char nick[32] = {};
};
static std::mutex g_snapMtx;
static std::unordered_map<void*, Snap> g_snaps;

static std::atomic<int> g_surfW{0}, g_surfH{0};
static std::atomic<int> g_localTeam{-1};
static std::mutex g_lpMtx; static Vec3 g_localPos{0, 0, 0}; static std::atomic<uint64_t> g_localSeen{0};

static struct { std::atomic<uint32_t> upd{0}, frames{0}, exc{0}, w2sFail{0}, drawn{0}; } ST;

static bool W2S(void* cam, Vec3 w, float scale, ImVec2& o) {
    Vec3 s = CallI<Vec3, Vec3>(G.w2s, cam, w);          // Unity: origin bottom-left, z<0 = за камерой
    if (s.z <= 0.f) return false;
    o.x = s.x * scale;
    o.y = g_surfH.load() - s.y * scale;                 // Y-flip под ImGui (origin top-left)
    return true;
}

static void (*orig_Update)(void*, const void*);
static void Collect(void* self) {
    int sw = g_surfW.load(); if (sw <= 0) return;
    uint8_t dead = CallI<uint8_t>(G.isDeath, self);
    if (dead) { std::lock_guard<std::mutex> g(g_snapMtx); g_snaps.erase(self); return; }

    void* pv = G.photon ? CallI<void*>(G.photon, self) : nullptr;
    bool mine = false;
    if (G.isMine && pv) mine = CallI<uint8_t>(G.isMine, pv) != 0;
    int team = G.offTeam >= 0 ? *(int*)((char*)self + G.offTeam) : 0;

    void* tr = CallI<void*>(G.getTransform, self); if (!tr) return;
    Vec3 p = CallI<Vec3>(G.getPosition, tr);

    if (mine) {
        { std::lock_guard<std::mutex> g(g_lpMtx); g_localPos = p; }
        g_localSeen = NowMs(); g_localTeam = team;
        std::lock_guard<std::mutex> g(g_snapMtx); g_snaps.erase(self);
        return;
    }

    void* cam = CallS<void*>(G.camMain); if (!cam) { LOG_EVERY(5000, LV_W, "Camera.main == null"); return; }
    int cw = CallI<int>(G.camPixW, cam); if (cw <= 0) return;
    float scale = (float)sw / cw;

    Snap s; s.seen = NowMs(); s.team = team;
    { std::lock_guard<std::mutex> g(g_lpMtx); Vec3 d{p.x - g_localPos.x, p.y - g_localPos.y, p.z - g_localPos.z}; s.dist = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); }
    int lt = g_localTeam.load();
    s.enemy = G.offTeam < 0 || lt < 0 || team != lt;

    if (!W2S(cam, p, scale, s.bot) || !W2S(cam, {p.x, p.y + PLAYER_H, p.z}, scale, s.top)) { ST.w2sFail++; return; }

    if (G.health) {
        if (G.health.ret == 0x08) s.hp = (float)CallI<int>(G.health, self);   // IL2CPP_TYPE_I4
        else                      s.hp = CallI<float>(G.health, self);        // R4 (дефолт)
    }
    if (G.nick) { void* r = G.nickOn ? pv : self; if (r) Utf16ToAscii(CallI<void*>(G.nick, r), s.nick, sizeof s.nick); }

    // скелет: PlayerAnimatorController -> _skeleton | _ctSkeleton -> bone transforms
    if (G.animCtl && G.offBone[0] >= 0 && (G.offSkel >= 0 || G.offCtSkel >= 0)) {
        void* ac = CallI<void*>(G.animCtl, self);
        void* sk = nullptr;
        if (ac) {
            if (G.offSkel   >= 0) sk = *(void**)((char*)ac + G.offSkel);
            if (!sk && G.offCtSkel >= 0) sk = *(void**)((char*)ac + G.offCtSkel);
        }
        if (sk) {
            for (int i = 0; i < 6; i++) {
                if (G.offBone[i] < 0) continue;
                void* bt = *(void**)((char*)sk + G.offBone[i]); if (!bt) continue;
                Vec3 bp = CallI<Vec3>(G.getPosition, bt);
                s.boneOk[i] = W2S(cam, bp, scale, s.bone[i]);
            }
            s.skel = true;
        }
    }
    std::lock_guard<std::mutex> g(g_snapMtx); g_snaps[self] = s;
}
static void hk_Update(void* self, const void* mi) {
    orig_Update(self, mi);
    ST.upd++;
    if (!self || !G.ready) return;
    try { Collect(self); }
    catch (...) { ST.exc++; LOG_EVERY(2000, LV_W, "managed exception в Collect (exc=%u)", ST.exc.load()); }
}

// ───────────────────────────── render ─────────────────────────────
static bool g_menuOpen = false;
static bool g_imInit = false;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static bool g_attached = false;
static ImGuiStyle g_baseStyle; static float g_lastScale = 0.f;
static bool g_prevDown = false;
static int g_logLv = LV_D; static bool g_logAuto = true;

static ImU32 Col(const float* c, float a = 1.f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3] * a)); }

static void PollTouch(ImGuiIO& io, int H) {
    if (!G.touchCount || !G.getTouch) return;
    if (!g_attached) { il2cpp_thread_attach(g_domain); g_attached = true; LOGI("render thread attached to il2cpp"); }
    int n = 0; float x = 0, y = 0; bool down = false;
    try {
        n = CallS<int>(G.touchCount);
        if (n > 0) {
            Touch t = CallS<Touch, int>(G.getTouch, 0);
            x = t.px; y = H - t.py; down = t.phase <= 2;          // 0 began, 1 moved, 2 stationary
        }
    } catch (...) { LOG_EVERY(5000, LV_W, "exception в PollTouch"); return; }
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    if (n > 0) io.AddMousePosEvent(x, y);
    if (down != g_prevDown) { io.AddMouseButtonEvent(0, down); g_prevDown = down; }
    if (!down && n == 0) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);   // не оставляем hover на тачскрине
}

static void DrawEsp(ImDrawList* dl) {
    std::vector<Snap> v; uint64_t now = NowMs();
    {
        std::lock_guard<std::mutex> g(g_snapMtx);
        for (auto it = g_snaps.begin(); it != g_snaps.end();) {
            if (now - it->second.seen > DROP_MS) { it = g_snaps.erase(it); continue; }
            if (now - it->second.seen <= STALE_MS) v.push_back(it->second);
            ++it;
        }
    }
    const Cfg& c = g_cfg; int W = g_surfW, H = g_surfH; char buf[64]; uint32_t drawn = 0;
    for (const Snap& s : v) {
        if (s.dist > c.maxDist) continue;
        if (s.enemy ? !c.enemy : !c.team) continue;
        if (c.teamCheck && !s.enemy && !c.team) continue;
        const float* col = s.enemy ? c.colEnemy : c.colTeam; ImU32 u = Col(col);
        float h = s.bot.y - s.top.y; if (h < 4.f) continue;
        float w = h * 0.45f, cx = (s.top.x + s.bot.x) * 0.5f;
        ImVec2 a(cx - w / 2, s.top.y), b(cx + w / 2, s.bot.y);
        float th = std::max(1.f, h / 120.f);
        if (c.box) { dl->AddRect({a.x - 1, a.y - 1}, {b.x + 1, b.y + 1}, IM_COL32(0, 0, 0, 200), 0, 0, th + 1); dl->AddRect(a, b, u, 0, 0, th); }
        if (c.health) {
            float f = std::clamp(s.hp / MAX_HP, 0.f, 1.f), bx = a.x - 4 - th;
            dl->AddRectFilled({bx - 1, a.y - 1}, {bx + th + 1, b.y + 1}, IM_COL32(0, 0, 0, 180));
            dl->AddRectFilled({bx, b.y - h * f}, {bx + th, b.y}, ImGui::ColorConvertFloat4ToU32(ImVec4(1 - f, f, 0.1f, 1)));
        }
        if (c.skeleton && s.skel) {
            static const int E[5][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 4}, {1, 5}};   // chest-pelvis, chest-arms, pelvis-thighs
            for (auto& e : E) if (s.boneOk[e[0]] && s.boneOk[e[1]]) dl->AddLine(s.bone[e[0]], s.bone[e[1]], u, th);
        }
        if (c.name && s.nick[0]) { ImVec2 ts = ImGui::CalcTextSize(s.nick); dl->AddText({cx - ts.x / 2, a.y - ts.y - 2}, u, s.nick); }
        if (c.dist) { snprintf(buf, sizeof buf, "%dm", (int)s.dist); ImVec2 ts = ImGui::CalcTextSize(buf); dl->AddText({cx - ts.x / 2, b.y + 2}, IM_COL32(255, 255, 255, 230), buf); }
        if (c.snaplines) dl->AddLine({W * 0.5f, (float)H}, {cx, b.y}, Col(col, 0.6f), 1.f);
        drawn++;
    }
    ST.drawn = drawn;
}

static void DrawMenu() {
    ImGuiIO& io = ImGui::GetIO(); Cfg& c = g_cfg;
    ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Always);
    ImGui::Begin("##btn", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::Button(g_menuOpen ? "ESP [x]" : "ESP")) g_menuOpen = !g_menuOpen;
    ImGui::End();
    if (!g_menuOpen) return;

    ImGui::SetNextWindowSize({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.7f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.05f, io.DisplaySize.y * 0.15f}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("ESP", &g_menuOpen) && ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("ESP")) {
            ImGui::Checkbox("enemy", &c.enemy); ImGui::SameLine(); ImGui::Checkbox("team", &c.team); ImGui::SameLine(); ImGui::Checkbox("teamCheck", &c.teamCheck);
            ImGui::Checkbox("box", &c.box); ImGui::SameLine(); ImGui::Checkbox("skeleton", &c.skeleton); ImGui::SameLine(); ImGui::Checkbox("health", &c.health);
            ImGui::Checkbox("name", &c.name); ImGui::SameLine(); ImGui::Checkbox("dist", &c.dist); ImGui::SameLine(); ImGui::Checkbox("snaplines", &c.snaplines);
            ImGui::SliderFloat("max dist", &c.maxDist, 20.f, 1000.f, "%.0f");
            ImGui::ColorEdit4("colEnemy", c.colEnemy, ImGuiColorEditFlags_NoInputs);
            ImGui::ColorEdit4("colTeam", c.colTeam, ImGuiColorEditFlags_NoInputs);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("CONFIG")) {
            ImGui::SliderFloat("ui scale", &c.uiScale, 1.f, 5.f, "%.1f");
            if (ImGui::Button("save")) CfgSave(); ImGui::SameLine(); if (ImGui::Button("load")) CfgLoad();
            ImGui::TextDisabled("%s", g_cfgPath.c_str());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("LOG")) {
            static const char* LN[] = {"debug", "info", "warn", "error"};
            int lvl = Log::minLevel.load();
            if (ImGui::Combo("capture", &lvl, LN, 4)) { Log::minLevel = lvl; c.logLevel = lvl; }
            ImGui::Combo("view", &g_logLv, LN, 4);
            ImGui::Checkbox("auto", &g_logAuto); ImGui::SameLine(); if (ImGui::Button("clear")) Log::clear();
            ImGui::TextDisabled("%s  W:%u E:%u", Log::path.empty() ? "(только logcat)" : Log::path.c_str(), Log::nWarn.load(), Log::nErr.load());
            ImGui::Text("upd %u  drawn %u  exc %u  w2sFail %u", ST.upd.load(), ST.drawn.load(), ST.exc.load(), ST.w2sFail.load());
            ImGui::BeginChild("##log", {0, 0}, true, ImGuiWindowFlags_HorizontalScrollbar);
            {
                std::lock_guard<std::mutex> g(Log::mtx);
                uint32_t from = Log::seq > (uint32_t)Log::RING ? Log::seq - Log::RING : 0;
                char b[320];
                for (uint32_t i = from; i < Log::seq; i++) {
                    const Log::Line& l = Log::ring[i % Log::RING]; if (l.lv < g_logLv) continue;
                    Log::fmtLine(b, sizeof b, l);
                    ImVec4 col = l.lv == LV_E ? ImVec4(1, .35f, .35f, 1) : l.lv == LV_W ? ImVec4(1, .8f, .3f, 1) : l.lv == LV_D ? ImVec4(.6f, .6f, .6f, 1) : ImVec4(.9f, .9f, .9f, 1);
                    ImGui::PushStyleColor(ImGuiCol_Text, col); ImGui::TextUnformatted(b); ImGui::PopStyleColor();
                }
            }
            if (g_logAuto && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.f) ImGui::SetScrollHereY(1.f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

static void* Phase2(void*);
static std::atomic<bool> g_active{false};
static std::atomic<int> g_rawFrames{0};
static EGLBoolean (*orig_swap)(EGLDisplay, EGLSurface);
static EGLBoolean hk_swap(EGLDisplay dpy, EGLSurface surf) {
    EGLContext ctx = eglGetCurrentContext();
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w); eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (ctx == EGL_NO_CONTEXT || w < 200 || h < 200) return orig_swap(dpy, surf);   // не основной сервейс

    if (!g_active.load()) {                          // фаза 1: пассивно, только считаем кадры
        int f = ++g_rawFrames;
        if (f == PHASE2_FRAMES) {
            pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            pthread_create(&th, &at, Phase2, nullptr);
        } else if (f % 120 == 0) LOGD("passive frames=%d", f);
        return orig_swap(dpy, surf);
    }

    if (!g_imInit) {
        IMGUI_CHECKVERSION(); ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigWindowsMoveFromTitleBarOnly = true;
        ImGui::StyleColorsDark(); g_baseStyle = ImGui::GetStyle();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        g_imInit = true; g_ctx = ctx;
        LOGI("imgui init %dx%d ctx=%p", w, h, ctx);
    } else if (ctx != g_ctx) {
        LOGW("gl ctx changed %p -> %p, реинит бэкенда", g_ctx, ctx);
        ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplOpenGL3_Init("#version 300 es"); g_ctx = ctx;
    }
    if (g_surfW != w || g_surfH != h) LOGI("surface %dx%d", w, h);
    g_surfW = w; g_surfH = h;

    ImGuiIO& io = ImGui::GetIO();
    if (g_lastScale != g_cfg.uiScale) {
        ImGui::GetStyle() = g_baseStyle; ImGui::GetStyle().ScaleAllSizes(g_cfg.uiScale);
        io.FontGlobalScale = g_cfg.uiScale; g_lastScale = g_cfg.uiScale;
    }
    static uint64_t last = NowMs(); uint64_t now = NowMs();
    io.DeltaTime = std::max(0.001f, (now - last) / 1000.f); last = now;
    io.DisplaySize = {(float)w, (float)h};

    PollTouch(io, h);
    ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
    DrawEsp(ImGui::GetBackgroundDrawList());
    DrawMenu();
    ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    ST.frames++;
    static uint64_t hbT = now; static uint32_t hbF = 0, hbU = 0;
    if (now - hbT >= 5000) {
        float dt = (now - hbT) / 1000.f;
        LOGI("hb fps=%.1f upd/s=%.0f snaps=%zu drawn=%u exc=%u w2sFail=%u local=%s",
             (ST.frames - hbF) / dt, (ST.upd - hbU) / dt, g_snaps.size(), ST.drawn.load(), ST.exc.load(), ST.w2sFail.load(),
             NowMs() - g_localSeen.load() < 1000 ? "yes" : "NO");
        hbT = now; hbF = ST.frames; hbU = ST.upd;
    }
    return orig_swap(dpy, surf);
}

// ───────────────────────────── bootstrap ─────────────────────────────
static int PhdrCb(dl_phdr_info* i, size_t, void* d) {
    if (i->dlpi_name && strstr(i->dlpi_name, "libil2cpp.so")) { *(const char**)d = i->dlpi_name; return 1; }
    return 0;
}
static bool InGameProcess() {
    char b[256] = {}; FILE* f = fopen("/proc/self/cmdline", "r"); if (!f) return false;
    fread(b, 1, sizeof b - 1, f); fclose(f);
    return strstr(b, PKG) != nullptr;
}

// фаза 2 (отдельный поток, запускается из hk_swap после PHASE2_FRAMES кадров): il2cpp + хук Update, затем g_active
static void* Phase2(void*) {
    LOGI("phase2: старт (%d кадров отрендерено)", PHASE2_FRAMES);
    const char* il = nullptr; int tries = 0;
    while (true) {
        dl_iterate_phdr(PhdrCb, &il);
        if (il) { g_il2cpp = dlopen(il, RTLD_NOW | RTLD_NOLOAD); if (g_il2cpp) break; }
        if (!(tries++ % 20)) LOGI("no libil2cpp (try %d)", tries);
        if (tries > 600) { LOGE("libil2cpp так и не появилась, abort"); return nullptr; }
        usleep(100000);
    }
    LOGI("libil2cpp: %s handle=%p", il, g_il2cpp);
    if (!ResolveApi()) { LOGE("resolve failed, abort"); return nullptr; }

    for (tries = 0; !(g_domain = il2cpp_domain_get()); tries++) {
        if (tries > 60) { LOGE("il2cpp_domain_get == null, abort"); return nullptr; }
        sleep(1);
    }
    il2cpp_thread_attach(g_domain);                 // ДО любых class_from_name / method_from_name
    LOGI("il2cpp attached domain=%p", g_domain);
    size_t last = 0, stable = 0;                    // ждём, пока список assemblies перестанет расти
    for (tries = 0; stable < 3; tries++) {
        size_t n = 0; il2cpp_domain_get_assemblies(g_domain, &n);
        stable = (n == last && n > 0) ? stable + 1 : 0; last = n;
        if (tries > 60) { LOGE("assemblies не стабилизировались (n=%zu), abort", n); return nullptr; }
        sleep(1);
    }
    LOGI("assemblies=%zu", last);
    for (tries = 0; !BindGame(); tries++) {
        if (!(tries % 5)) LOGI("ждём классы игры (try %d)", tries);
        if (tries > 180) { LOGE("PlayerController не найден за 3 мин, abort"); return nullptr; }
        sleep(1);
    }

    Method mu = GetMethod(G.kPlayer, "PlayerController", "Update", 0);
    if (!mu) { LOGE("PlayerController.Update miss — нечего хукать"); return nullptr; }
    if (access("/data/local/tmp/esp.noupdate", F_OK) == 0) LOGW("esp.noupdate: Update хук пропущен");
    else { A64HookFunction(mu.fn, (void*)hk_Update, (void**)&orig_Update); LOGI("Update hooked @%p", mu.fn); }
    g_active = true;                                // с этого момента swap-хук рисует меню/ESP
    LOGI("active: меню включено");
    return nullptr;
}

// фаза 1: только пассивный хук eglSwapBuffers (hk_swap до PHASE2_FRAMES просто пропускает кадр)
static void* Boot(void*) {
    // LD_PRELOAD отрабатывает до setArgV0 — ждём, пока процесс станет игрой
    int t = 0; while (!InGameProcess()) { if (++t > 600) return nullptr; usleep(100000); }   // 60с, иначе это не наш процесс
    g_dataDir = std::string("/data/data/") + PKG + "/files";
    mkdir(g_dataDir.c_str(), 0771);
    const char* dirs[] = {g_dataDir.c_str(), "/data/local/tmp"};
    Log::init(dirs, 2);
    g_cfgPath = g_dataDir + "/esp.json";
    LOGI("libesp boot pid=%d pkg=%s log=%s", getpid(), PKG, Log::path.c_str());
    CfgLoad();

    if (access("/data/local/tmp/esp.noswap", F_OK) == 0) { LOGW("esp.noswap: swap хук пропущен"); return nullptr; }
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    void* sw = egl ? dlsym(egl, "eglSwapBuffers") : nullptr;
    if (!sw) { LOGE("no eglSwapBuffers"); return nullptr; }
    A64HookFunction(sw, (void*)hk_swap, (void**)&orig_swap);
    LOGI("swap hooked @%p (пассивно, phase2 через %d кадров)", sw, PHASE2_FRAMES);
    return nullptr;
}

__attribute__((constructor)) static void Entry() {
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "libesp loaded pid=%d", getpid());
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &at, Boot, nullptr);
}

/* build.sh  (NDK r25+, из каталога с main.cpp, рядом imgui/ и And64InlineHook/)
NDK=$ANDROID_NDK_HOME; CXX=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++
$CXX -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -DIMGUI_IMPL_OPENGL_ES3 -Iimgui -IAnd64InlineHook \
  main.cpp imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp imgui/backends/imgui_impl_opengl3.cpp \
  And64InlineHook/And64InlineHook.cpp -o libesp.so -llog -lEGL -lGLESv3 -ldl -static-libstdc++ -Wl,-z,max-page-size=16384
*/
