// StandChillow 2.07 (offline) - 1-ci addim: yukleme + oxuma testi (nisan almir, yalniz log yazir)
// Hedef: Android arm64-v8a, IL2CPP. Java/JNI hissesi YOXDUR (JNI_OnLoad yoxdur).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <pthread.h>
#include <vector>
#include <algorithm>
#include <android/log.h>

#include <cstdarg>
#include <sys/stat.h>

#define TAG "PHANTOM_AIMBOT"
#define LOG_DIR  "/storage/emulated/0/Android/data/com.Nightmare.Custom1/files"
#define LOG_FILE LOG_DIR "/mod_log.txt"

// Hem logcat-a, hem de faila yazir (logcat gorunmese fayla baxmaq ucun)
static void LogBoth(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", buf);
    static int lines = 0;
    if (lines < 3000) {
        mkdir(LOG_DIR, 0777);
        FILE *f = fopen(LOG_FILE, "a");
        if (f) { fprintf(f, "%s\n", buf); fclose(f); lines++; }
    }
}
#define LOGI(...) LogBoth(__VA_ARGS__)
#define LOGE(...) LogBoth(__VA_ARGS__)

extern "C" int DobbyHook(void *address, void *replace_call, void **origin_call);

struct Vector3 { float x, y, z; };

// ---------------- RVA-lar (dump.cs, StandChillow 2.07) ----------------
static const uintptr_t RVA_CAM_GETINSTANCE = 0x42BD014; // PlayerMainCamera.oec() static
static const uintptr_t RVA_PC_ONENABLE     = 0x42B9630; // PlayerController.OnEnable
static const uintptr_t RVA_PC_ONDISABLE    = 0x42B96B8; // PlayerController.OnDisable
static const uintptr_t RVA_PC_LATEUPDATE   = 0x42B990C; // PlayerController.LateUpdate
static const uintptr_t RVA_TR_GETPOS       = 0x77D3690; // Transform.get_position
static const uintptr_t RVA_TR_GETFWD       = 0x77D3BE0; // Transform.get_forward
static const uintptr_t RVA_TR_GETEULER     = 0x77D3880; // Transform.get_eulerAngles
static const uintptr_t RVA_TR_SETEULER     = 0x77D3918; // Transform.set_eulerAngles
static const uintptr_t RVA_OFFLINE_START   = 0x416AFD0; // OfflineModeController.mco
static const uintptr_t RVA_OFFLINE_STOP    = 0x416B010; // OfflineModeController.mcp
static const uintptr_t RVA_AIM_PGL         = 0x42EC7E4; // AimController.pgl(Vector3) - isleyisi yoxlanmayib

// ---------------- Sahe ofsetleri (dump.cs) ----------------
static const uintptr_t OFF_CAM_LOCALPLAYER = 0x40; // PlayerMainCamera -> PlayerController
static const uintptr_t OFF_PC_BIPED        = 0x30; // PlayerController -> BipedMap
static const uintptr_t OFF_PC_TEAM         = 0x79; // PlayerController -> Team (byte)
static const uintptr_t OFF_PC_AIM          = 0xD0; // PlayerController -> AimController
static const uintptr_t OFF_BIPED_HEAD      = 0x20; // BipedMap -> Head (Transform)
static const uintptr_t OFF_AIM_CAMTRANS    = 0x78; // AimController -> camTransform
static const uintptr_t OFF_AIM_HEADANGLES  = 0xB8; // AimController -> _headDirectiveAngles (Vector3, teqribi: baxis bucaqlari?)

// ---------------- Ayarlar ----------------
static bool  g_enabled       = true;
static bool  g_teamCheck     = true;   // eyni komandani kecir
static bool  g_requireOffline= false;  // DIAQNOSTIKA: offline bayragi tetiklenmirse de oxuma isleyir (2.07 yalniz offline-dir)
static bool  g_applyAim      = true;   // nisan aciq: oyunun oz baxis veziyyetine (AimingData) yazir
static float g_maxDist       = 200.0f;
static float g_fov           = 60.0f;  // derece: yalniz bu konusun icindeki dusmenlere (nisangahdan ucaqliq)
static float g_smooth        = 1.0f;   // 0..1, boyuk = tez ceikir

// ---------------- Veziyyet (hamisi Unity esas thread-indedir) ----------------
static uintptr_t g_base = 0;
static bool g_offline = false;
static std::vector<void*> g_players;

typedef Vector3 (*Vec3Fn)(void*, void*);
static Vec3Fn TrGetPos = nullptr;
static Vec3Fn TrGetFwd = nullptr;
static Vec3Fn TrGetEuler = nullptr;
typedef void (*SetVec3Fn)(void *, Vector3, void *);
static SetVec3Fn TrSetEuler = nullptr;
typedef void* (*GetInstFn)(void*);
static GetInstFn CamGetInstance = nullptr;
typedef void (*PglFn)(void*, Vector3, void*);
static PglFn AimPgl = nullptr;

static uintptr_t get_module_base(const char *name) {
    uintptr_t base = 0;
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, name)) { base = strtoul(line, nullptr, 16); break; }
    }
    fclose(fp);
    return base;
}

static inline bool PtrOk(void *p) {
    uintptr_t v = (uintptr_t)p;
    return v > 0x10000 && v < 0x0000800000000000ULL && (v & 7) == 0;
}

// Unity obyektinin native hissesi (m_CachedPtr, +0x10) bos deyilse obyekt sagdir
static inline bool UnityAlive(void *obj) {
    if (!PtrOk(obj)) return false;
    return *(void **)((uintptr_t)obj + 0x10) != nullptr;
}

static void *GetLocalPlayer() {
    if (!CamGetInstance) return nullptr;
    void *cam = CamGetInstance(nullptr);
    if (!PtrOk(cam)) return nullptr;
    void *lp = *(void **)((uintptr_t)cam + OFF_CAM_LOCALPLAYER);
    return UnityAlive(lp) ? lp : nullptr;
}

static void *GetHead(void *player) {
    void *biped = *(void **)((uintptr_t)player + OFF_PC_BIPED);
    if (!PtrOk(biped)) return nullptr;
    void *head = *(void **)((uintptr_t)biped + OFF_BIPED_HEAD);
    return UnityAlive(head) ? head : nullptr;
}

// ---------------- Hook-lar ----------------
static void (*old_OnEnable)(void *, void *);
static void hk_OnEnable(void *self, void *mi) {
    if (self && std::find(g_players.begin(), g_players.end(), self) == g_players.end()) {
        g_players.push_back(self);
        static int n = 0;
        if (n++ < 20) LOGI("OnEnable #%d obyekt=%p oyuncular=%zu", n, self, g_players.size());
    }
    old_OnEnable(self, mi);
}

static void (*old_OnDisable)(void *, void *);
static void hk_OnDisable(void *self, void *mi) {
    g_players.erase(std::remove(g_players.begin(), g_players.end(), self), g_players.end());
    old_OnDisable(self, mi);
}

static void (*old_OffStart)(void *, void *);
static void hk_OffStart(void *self, void *mi) {
    g_offline = true;
    LOGI("Offline rejim BASLADI");
    old_OffStart(self, mi);
}

static void (*old_OffStop)(void *, void *);
static void hk_OffStop(void *self, void *mi) {
    g_offline = false;
    LOGI("Offline rejim BITDI");
    old_OffStop(self, mi);
}

static float NormDeg(float a) { while (a > 180.0f) a -= 360.0f; while (a < -180.0f) a += 360.0f; return a; }

static void Tick(void *local) {
    void *aim = *(void **)((uintptr_t)local + OFF_PC_AIM);
    if (!PtrOk(aim)) return;
    void *camT = *(void **)((uintptr_t)aim + OFF_AIM_CAMTRANS);
    if (!UnityAlive(camT)) return;

    // ---- DIAQNOSTIKA: baxis bucaqlari hansi sahede saxlanir? (yalniz oxuyuruq, ~4 defe/san) ----
    {
        static long lastMs = 0;
        timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        long ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
        if (ms - lastMs >= 250) {
            lastMs = ms;
            Vector3 ec = TrGetEuler(camT, nullptr);
            void *ad = *(void **)((uintptr_t)aim + 0x80);              // AimingData
            Vector3 caa{0, 0, 0}, ce{0, 0, 0};
            if (PtrOk(ad)) { caa = *(Vector3 *)((uintptr_t)ad + 0x18); ce = *(Vector3 *)((uintptr_t)ad + 0x24); }
            void *sn = *(void **)((uintptr_t)aim + 0x170);             // AimSnapshot
            float ax = 0, ay = 0;
            if (PtrOk(sn)) { ax = *(float *)((uintptr_t)sn + 0x54); ay = *(float *)((uintptr_t)sn + 0x58); }
            void *spine = *(void **)((uintptr_t)aim + 0x60);           // spineDirector
            void *fpsc  = *(void **)((uintptr_t)aim + 0x68);           // FPSCamera
            Vector3 es{0, 0, 0}, ef{0, 0, 0};
            if (UnityAlive(spine)) es = TrGetEuler(spine, nullptr);
            if (UnityAlive(fpsc))  ef = TrGetEuler(fpsc, nullptr);
            LOGI("A cam=(%.1f,%.1f) curAim=(%.1f,%.1f,%.1f) curEul=(%.1f,%.1f,%.1f) snapXY=(%.1f,%.1f) spine=(%.1f,%.1f) fps=(%.1f,%.1f)",
                 ec.x, ec.y, caa.x, caa.y, caa.z, ce.x, ce.y, ce.z, ax, ay, es.x, es.y, ef.x, ef.y);
        }
    }

    Vector3 cp = TrGetPos(camT, nullptr);
    Vector3 fw = TrGetFwd(camT, nullptr);
    uint8_t myTeam = *(uint8_t *)((uintptr_t)local + OFF_PC_TEAM);

    // Hedef: nisangaha (kamera istiqametine) en yaxin bucaqli dusmen
    void *best = nullptr; float bestDist = 0; float bestAng = 1e9f; Vector3 bestHead{0, 0, 0};
    for (void *p : g_players) {
        if (p == local || !UnityAlive(p)) continue;
        if (g_teamCheck && *(uint8_t *)((uintptr_t)p + OFF_PC_TEAM) == myTeam) continue;
        void *h = GetHead(p);
        if (!h) continue;
        Vector3 hp = TrGetPos(h, nullptr);
        Vector3 d{hp.x - cp.x, hp.y - cp.y, hp.z - cp.z};
        float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (dist < 0.1f || dist > g_maxDist) continue;
        float c = (d.x * fw.x + d.y * fw.y + d.z * fw.z) / dist;
        c = std::max(-1.0f, std::min(1.0f, c));
        float ang = std::acos(c) * 57.29578f;
        if (ang < bestAng) { bestAng = ang; bestDist = dist; best = p; bestHead = hp; }
    }

    bool applied = false;
    if (best && g_applyAim && TrSetEuler && bestAng <= g_fov) {
        Vector3 dd{bestHead.x - cp.x, bestHead.y - cp.y, bestHead.z - cp.z};
        Vector3 eu = TrGetEuler(camT, nullptr);
        float curYaw = eu.y;
        float curPitch = eu.x > 180.0f ? eu.x - 360.0f : eu.x;
        float tyaw = std::atan2(dd.x, dd.z) * 57.29578f; if (tyaw < 0) tyaw += 360.0f;
        float tpitch = -std::asin(std::max(-1.0f, std::min(1.0f, dd.y / bestDist))) * 57.29578f;
        float ny = curYaw + NormDeg(tyaw - curYaw) * g_smooth;
        float np = curPitch + (tpitch - curPitch) * g_smooth;
        ny = std::fmod(ny + 360.0f, 360.0f);
        // Pitch hududu (oyunun oz minimumX/maximumX deyerleri, munasibdirse)
        float minX = *(float *)((uintptr_t)aim + 0x94), maxX = *(float *)((uintptr_t)aim + 0x98);
        if (minX < maxX && minX > -91.0f && maxX < 91.0f) np = std::max(minX, std::min(maxX, np));
        // Oyunun ozunun baxis veziyyetini yaziriq: curAimAngle=(pitch,yaw,0), curEulerAngles=(0,yaw,0) (beden)
        void *ad = *(void **)((uintptr_t)aim + 0x80);
        if (PtrOk(ad)) {
            Vector3 *caa = (Vector3 *)((uintptr_t)ad + 0x18);
            Vector3 *ce  = (Vector3 *)((uintptr_t)ad + 0x24);
            caa->x = np; caa->y = ny;
            ce->y = ny;
        }
        // Bu kadrin gorunusu da uygun olsun
        TrSetEuler(camT, Vector3{np, ny, 0.0f}, nullptr);
        applied = true;
    }

    static time_t last = 0;
    time_t now = time(nullptr);
    if (now != last) {
        last = now;
        if (best) LOGI("oyuncu=%zu hedef: mesafe=%.1f bucaq=%.1f aim=%d", g_players.size(), bestDist, bestAng, (int)applied);
        else      LOGI("oyuncu=%zu dusmen tapilmadi", g_players.size());
    }
}

static void (*old_LateUpdate)(void *, void *);
static void hk_LateUpdate(void *self, void *mi) {
    old_LateUpdate(self, mi);
    if (!g_enabled) return;
    void *local = GetLocalPlayer();
    static time_t lastDiag = 0;
    time_t now = time(nullptr);
    if (now != lastDiag && (!local || self == local)) {
        lastDiag = now;
        LOGI("diag: offline=%d oyuncular=%zu local=%p", (int)g_offline, g_players.size(), local);
    }
    if (g_requireOffline && !g_offline) return;
    if (local && self == local) Tick(local);
}

// ---------------- Baslatma ----------------
static void Hook(const char *name, uintptr_t rva, void *rep, void **orig) {
    int r = DobbyHook((void *)(g_base + rva), rep, orig);
    LOGI("hook %s @%p -> %d", name, (void *)(g_base + rva), r);
}

static void *InitThread(void *) {
    while (!(g_base = get_module_base("libil2cpp.so"))) usleep(100000);
    LOGI("libil2cpp baza: %p", (void *)g_base);
    sleep(8); // oyunun yuklenmesini gozle (tez hook cokme verir)

    TrGetPos = (Vec3Fn)(g_base + RVA_TR_GETPOS);
    TrGetFwd = (Vec3Fn)(g_base + RVA_TR_GETFWD);
    TrGetEuler = (Vec3Fn)(g_base + RVA_TR_GETEULER);
    TrSetEuler = (SetVec3Fn)(g_base + RVA_TR_SETEULER);
    CamGetInstance = (GetInstFn)(g_base + RVA_CAM_GETINSTANCE);
    AimPgl = (PglFn)(g_base + RVA_AIM_PGL);

    Hook("OnEnable",   RVA_PC_ONENABLE,   (void *)hk_OnEnable,   (void **)&old_OnEnable);
    Hook("OnDisable",  RVA_PC_ONDISABLE,  (void *)hk_OnDisable,  (void **)&old_OnDisable);
    Hook("LateUpdate", RVA_PC_LATEUPDATE, (void *)hk_LateUpdate, (void **)&old_LateUpdate);
    Hook("OffStart",   RVA_OFFLINE_START, (void *)hk_OffStart,   (void **)&old_OffStart);
    Hook("OffStop",    RVA_OFFLINE_STOP,  (void *)hk_OffStop,    (void **)&old_OffStop);
    LOGI("hazirdir");
    return nullptr;
}

__attribute__((constructor))
static void ModInit() {
    LOGI("MyLibName YUKLENDI (constructor)");
    pthread_t t;
    if (pthread_create(&t, nullptr, InitThread, nullptr) == 0) pthread_detach(t);
}
