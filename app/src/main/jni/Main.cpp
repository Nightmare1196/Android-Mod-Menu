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

// ---------------- Ayarlar ----------------
static bool  g_enabled       = true;
static bool  g_teamCheck     = true;   // eyni komandani kecir
static bool  g_requireOffline= false;  // DIAQNOSTIKA: offline bayragi tetiklenmirse de oxuma isleyir (2.07 yalniz offline-dir)
static bool  g_applyAim      = false;  // 1-ci addimda SONDURULUB (yalniz log)
static float g_maxDist       = 200.0f;

// ---------------- Veziyyet (hamisi Unity esas thread-indedir) ----------------
static uintptr_t g_base = 0;
static bool g_offline = false;
static std::vector<void*> g_players;

typedef Vector3 (*Vec3Fn)(void*, void*);
static Vec3Fn TrGetPos = nullptr;
static Vec3Fn TrGetFwd = nullptr;
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

static void Tick(void *local) {
    void *aim = *(void **)((uintptr_t)local + OFF_PC_AIM);
    if (!PtrOk(aim)) return;
    void *camT = *(void **)((uintptr_t)aim + OFF_AIM_CAMTRANS);
    if (!UnityAlive(camT)) return;

    Vector3 cp = TrGetPos(camT, nullptr);
    Vector3 fw = TrGetFwd(camT, nullptr);
    uint8_t myTeam = *(uint8_t *)((uintptr_t)local + OFF_PC_TEAM);

    void *best = nullptr; float bestDist = 1e9f; float bestAng = 0; Vector3 bestHead{0, 0, 0};
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
        if (dist < bestDist) { bestDist = dist; best = p; bestHead = hp; bestAng = std::acos(c) * 57.29578f; }
    }

    static time_t last = 0;
    time_t now = time(nullptr);
    if (now != last) {
        last = now;
        if (best) LOGI("oyuncu=%zu en yaxin dusmen: mesafe=%.1f bucaq=%.1f", g_players.size(), bestDist, bestAng);
        else      LOGI("oyuncu=%zu dusmen tapilmadi", g_players.size());
    }

    if (g_applyAim && best && AimPgl) AimPgl(aim, bestHead, nullptr);
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
