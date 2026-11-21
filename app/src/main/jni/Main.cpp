// StandChillow 2.07 (offline) - v8: cheating.txt ile idare olunan sistem
//  * Aimbot (kamera/baxis veziyyetine yazir) - ayarlari cheating.txt-den oxuyur
//  * Admin panelinin hiyleleri (ESP, sonsuz sursat, hereket...) - oyunun oz CheatParameters obyektine yazir
// Hedef: Android arm64-v8a, IL2CPP. Java/JNI hissesi YOXDUR.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdarg>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <android/log.h>

#define BUILD_ID "v11-clone-apply"
#define TAG "PHANTOM_AIMBOT"
#define LOG_DIR  "/storage/emulated/0/Android/data/com.Nightmare.Custom1/files"
#define LOG_FILE LOG_DIR "/mod_log.txt"
#define CFG_FILE LOG_DIR "/cheating.txt"

// Hem logcat-a, hem de faila yazir
static void LogBoth(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", buf);
    static int lines = 0;
    if (lines < 5000) {
        mkdir(LOG_DIR, 0777);
        FILE *f = fopen(LOG_FILE, "a");
        if (f) { fprintf(f, "%s\n", buf); fclose(f); lines++; }
    }
}
#define LOGI(...) LogBoth(__VA_ARGS__)

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
// GameController (singleton) ve "cva" (hiyle idareci sinfi)
static const uintptr_t RVA_GC_GETINSTANCE  = 0x40D26A4; // GameController.ysv() static -> singleton
static const uintptr_t RVA_GC_GETCVA       = 0x40D279C; // GameController.ytb() -> cva (instance)
static const uintptr_t RVA_CVA_LYL         = 0x419A1AC; // cva.lyl(CheatParameters) - deyisikliyi bildirir
static const uintptr_t RVA_CP_CLONE        = 0x4197188; // CheatParameters.Clone()

// ---------------- Sahe ofsetleri (dump.cs) ----------------
static const uintptr_t OFF_CAM_LOCALPLAYER = 0x40; // PlayerMainCamera -> PlayerController
static const uintptr_t OFF_PC_BIPED        = 0x30; // PlayerController -> BipedMap
static const uintptr_t OFF_PC_TEAM         = 0x79; // PlayerController -> Team (byte)
static const uintptr_t OFF_PC_AIM          = 0xD0; // PlayerController -> AimController
static const uintptr_t OFF_BIPED_HEAD      = 0x20; // BipedMap -> Head (Transform)
static const uintptr_t OFF_AIM_CAMTRANS    = 0x78; // AimController -> camTransform
static const uintptr_t OFF_AIM_DATA        = 0x80; // AimController -> AimingData
static const uintptr_t OFF_AIM_MINX        = 0x94; // AimController -> minimumX (pitch hedd)
static const uintptr_t OFF_AIM_MAXX        = 0x98; // AimController -> maximumX
static const uintptr_t OFF_AD_CURAIM       = 0x18; // AimingData -> curAimAngle (pitch,yaw,0)
static const uintptr_t OFF_AD_CUREUL       = 0x24; // AimingData -> curEulerAngles (0,yaw,0)
static const uintptr_t OFF_GC_CVA          = 0x30; // GameController -> cva
// cva -> CheatParameters -> alt obyektler
static const uintptr_t OFF_CVA_PARAMS      = 0x10; // cva -> CheatParameters (hazirki)
static const uintptr_t OFF_CP_PLAYER       = 0x10;
static const uintptr_t OFF_CP_AIMESP       = 0x18;
static const uintptr_t OFF_CP_WEAPON       = 0x30;
static const uintptr_t OFF_CP_OTHER        = 0x38;

// ---------------- Ayarlar (cheating.txt-den yenilenir) ----------------
static bool  g_enabled   = true;
static bool  g_teamCheck = true;
static bool  g_applyAim  = true;
static float g_maxDist   = 200.0f;
static float g_fov       = 60.0f;
static float g_smooth    = 1.0f;
static bool  g_notify    = true;

// ---------------- Veziyyet (hamisi Unity esas thread-indedir) ----------------
static unsigned g_cntLate = 0, g_cntTick = 0, g_cntApplied = 0;
static uintptr_t g_base = 0;
static std::vector<void *> g_players;
static void *g_cheats = nullptr; // cva obyekti

typedef Vector3 (*Vec3Fn)(void *, void *);
static Vec3Fn TrGetPos = nullptr, TrGetFwd = nullptr, TrGetEuler = nullptr;
typedef void (*SetVec3Fn)(void *, Vector3, void *);
static SetVec3Fn TrSetEuler = nullptr;
typedef void *(*GetInstFn)(void *);
static GetInstFn CamGetInstance = nullptr;

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

// ================= cheating.txt =================
static std::map<std::string, std::string> g_cfg;
static time_t g_cfgMtime = 0;

static const char *TEMPLATE =
"# StandChillow mod ayarlari. 1 = aciq, 0 = qapali.\n"
"# Setrin evvelinde # varsa, o ayar tetbiq olunmur (oyunun ozunun deyeri qalir).\n"
"# Fayl deyisdikde 1-2 saniye icinde tetbiq olunur.\n"
"\n"
"# --- Aimbot ---\n"
"aimbot=1\n"
"aim_fov=60\n"
"aim_smooth=1.0\n"
"aim_maxdist=200\n"
"team_check=1\n"
"\n"
"# --- ESP ---\n"
"#esp=1\n"
"#esp_boxes=1\n"
"#esp_skeleton=1\n"
"#esp_snapline=0\n"
"\n"
"# --- Silah ---\n"
"#infinity_ammo=1\n"
"#infinity_grenades=1\n"
"#grenade_trajectory=1\n"
"#anti_recoil=1\n"
"#wall_shot=1\n"
"\n"
"# --- Oyuncu ---\n"
"#move_speed=5.3\n"
"#gravity=16\n"
"#jump_height=6.2\n"
"#bhop=1\n"
"#invisibility=0\n"
"#untouchable=0\n"
"#third_person=0\n"
"#max_hp=100\n"
"\n"
"# --- Diger ---\n"
"#radar=1\n"
"#reverse_map=0\n"
"#time_scale=1.0\n"
"\n"
"# Texniki: 1 = deyisiklik oyuna bildirilir; oyun cokurse 0 yaz\n"
"notify=1\n";

static void EnsureTemplate() {
    struct stat st;
    if (stat(CFG_FILE, &st) == 0) return;
    mkdir(LOG_DIR, 0777);
    FILE *f = fopen(CFG_FILE, "w");
    if (f) { fputs(TEMPLATE, f); fclose(f); LOGI("cheating.txt yaradildi"); }
}

static std::string Trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static bool CfgGet(const char *key, std::string &out) {
    auto it = g_cfg.find(key);
    if (it == g_cfg.end()) return false;
    out = it->second;
    return true;
}
static bool CfgBool(const char *key, bool &v) {
    std::string s;
    if (!CfgGet(key, s)) return false;
    v = (s == "1" || s == "true" || s == "on" || s == "aciq");
    return true;
}
static bool CfgFloat(const char *key, float &v) {
    std::string s;
    if (!CfgGet(key, s)) return false;
    v = strtof(s.c_str(), nullptr);
    return true;
}

static bool LoadConfig() { // dayisibse true
    struct stat st;
    if (stat(CFG_FILE, &st) != 0) { EnsureTemplate(); if (stat(CFG_FILE, &st) != 0) return false; }
    if (st.st_mtime == g_cfgMtime) return false;
    g_cfgMtime = st.st_mtime;
    FILE *f = fopen(CFG_FILE, "r");
    if (!f) return false;
    g_cfg.clear();
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string l = Trim(line);
        if (l.empty() || l[0] == '#') continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        g_cfg[Trim(l.substr(0, eq))] = Trim(l.substr(eq + 1));
    }
    fclose(f);
    LOGI("cheating.txt oxundu: %zu ayar", g_cfg.size());
    return true;
}

static void ApplyAimConfig() {
    bool b; float v;
    if (CfgBool("aimbot", b)) g_applyAim = b;
    if (CfgBool("team_check", b)) g_teamCheck = b;
    if (CfgBool("notify", b)) g_notify = b;
    if (CfgFloat("aim_fov", v)) g_fov = std::max(1.0f, std::min(360.0f, v));
    if (CfgFloat("aim_smooth", v)) g_smooth = std::max(0.01f, std::min(1.0f, v));
    if (CfgFloat("aim_maxdist", v)) g_maxDist = std::max(1.0f, v);
}

// ================= Hiyle (cheat) parametrlerinin yazilmasi =================
typedef void (*LylFn)(void *, void *, void *);
static LylFn CvaLyl = nullptr;
typedef void *(*GetGCFn)(void *);
static GetGCFn GetGC = nullptr;
typedef void *(*CloneFn)(void *, void *);
static CloneFn CpClone = nullptr;
typedef void *(*GcGetCvaFn)(void *, void *);
static GcGetCvaFn GcYtb = nullptr;

static void FindCva() {
    if (!GetGC) return;
    void *gc = GetGC(nullptr);
    void *raw = PtrOk(gc) ? *(void **)((uintptr_t)gc + OFF_GC_CVA) : nullptr;
    void *viaCall = (PtrOk(gc) && GcYtb) ? GcYtb(gc, nullptr) : nullptr;
    void *def = PtrOk(gc) ? *(void **)((uintptr_t)gc + 0x1B0) : nullptr; // _defaultCheatParameters
    static time_t lastDbg = 0;
    time_t now = time(nullptr);
    if (!g_cheats && now - lastDbg >= 5) {
        lastDbg = now;
        LOGI("FindCva: gc=%p raw=%p ytb=%p default=%p", gc, raw, viaCall, def);
    }
    void *cv = PtrOk(raw) ? raw : viaCall;
    if (!PtrOk(cv)) { g_cheats = nullptr; return; }
    if (cv != g_cheats) {
        g_cheats = cv;
        void *cpp = *(void **)((uintptr_t)cv + OFF_CVA_PARAMS);
        LOGI("cva tapildi: gc=%p cva=%p params=%p flags=%d,%d", gc, cv, cpp,
             (int)*(uint8_t *)((uintptr_t)cv + 0x40), (int)*(uint8_t *)((uintptr_t)cv + 0x41));
    }
}

// Istenilen deyerleri yazir (dry=true: yalniz fergi yoxlayir, yazmir). Fergli bir sey varsa true qaytarir.
static bool WriteCheats(void *cp, bool dry) {
    void *player = *(void **)((uintptr_t)cp + OFF_CP_PLAYER);
    void *aimesp = *(void **)((uintptr_t)cp + OFF_CP_AIMESP);
    void *weapon = *(void **)((uintptr_t)cp + OFF_CP_WEAPON);
    void *other  = *(void **)((uintptr_t)cp + OFF_CP_OTHER);

    bool changed = false;
    auto setB = [&](void *obj, uintptr_t off, const char *key) {
        bool v;
        if (PtrOk(obj) && CfgBool(key, v)) {
            uint8_t *p = (uint8_t *)((uintptr_t)obj + off);
            if (*p != (uint8_t)v) { if (!dry) *p = (uint8_t)v; changed = true; }
        }
    };
    auto setF = [&](void *obj, uintptr_t off, const char *key) {
        float v;
        if (PtrOk(obj) && CfgFloat(key, v)) {
            float *p = (float *)((uintptr_t)obj + off);
            if (*p != v) { if (!dry) *p = v; changed = true; }
        }
    };
    auto setI = [&](void *obj, uintptr_t off, const char *key) {
        float v;
        if (PtrOk(obj) && CfgFloat(key, v)) {
            int *p = (int *)((uintptr_t)obj + off);
            if (*p != (int)v) { if (!dry) *p = (int)v; changed = true; }
        }
    };

    // ESP (AimEspCheatParameters)
    setB(aimesp, 0x10, "esp");
    setB(aimesp, 0x18, "esp_boxes");
    setB(aimesp, 0x19, "esp_skeleton");
    setB(aimesp, 0x1A, "esp_snapline");
    // Silah (WeaponCheatParameters)
    setB(weapon, 0x10, "infinity_ammo");
    setB(weapon, 0x11, "infinity_grenades");
    setB(weapon, 0x12, "grenade_trajectory");
    setB(weapon, 0x13, "anti_recoil");
    setB(weapon, 0x14, "wall_shot");
    // Oyuncu (PlayerCheatParameters)
    setF(player, 0x10, "move_speed");
    setF(player, 0x14, "gravity");
    setF(player, 0x18, "jump_height");
    setB(player, 0x20, "bhop");
    setB(player, 0x21, "invisibility");
    setB(player, 0x22, "untouchable");
    setI(player, 0x24, "max_hp");
    setB(player, 0x28, "third_person");
    // Diger (OtherCheatParameters)
    setF(other, 0x10, "time_scale");
    setB(other, 0x14, "reverse_map");
    setB(other, 0x15, "radar");
    return changed;
}

static void LogCvaFields() {
    if (!g_cheats) return;
    uintptr_t c = (uintptr_t)g_cheats;
    static void *l18 = (void *)1, *l20 = (void *)1, *l28 = (void *)1, *l48 = (void *)1, *l10 = (void *)1;
    static int lf = -1;
    void *p10 = *(void **)(c + 0x10), *p18 = *(void **)(c + 0x18), *p20 = *(void **)(c + 0x20),
         *p28 = *(void **)(c + 0x28), *p48 = *(void **)(c + 0x48);
    int fl = (*(uint8_t *)(c + 0x40) << 8) | *(uint8_t *)(c + 0x41);
    if (p10 != l10 || p18 != l18 || p20 != l20 || p28 != l28 || p48 != l48 || fl != lf) {
        l10 = p10; l18 = p18; l20 = p20; l28 = p28; l48 = p48; lf = fl;
        LOGI("cva alanlari: params=%p panel=%p material=%p event=%p applier=%p flags=%04x", p10, p18, p20, p28, p48, fl);
    }
}

static void ApplyCheats() {
    FindCva();
    if (!g_cheats) return;
    LogCvaFields();
    void *cp = *(void **)((uintptr_t)g_cheats + OFF_CVA_PARAMS);
    if (!PtrOk(cp)) return;

    if (!WriteCheats(cp, true)) return; // fergli bir sey yoxdur

    // Admin paneli "Tetbiq et" basanda etdiyi kimi: parametrlerin KOPYASINI yarat, deyis, lyl() ile bildir
    void *ncp = (g_notify && CpClone) ? CpClone(cp, nullptr) : nullptr;
    if (PtrOk(ncp)) {
        WriteCheats(ncp, false);
        LOGI("hiyle: kopya yaradildi %p (kohne %p), lyl cagirilir", ncp, cp);
        if (CvaLyl) CvaLyl(g_cheats, ncp, nullptr);
    } else {
        WriteCheats(cp, false);
        LOGI("hiyle parametrleri birbasa yazildi (notify=%d)", (int)g_notify);
        if (g_notify && CvaLyl) CvaLyl(g_cheats, cp, nullptr);
    }
}

// ---------------- Hook-lar ----------------
static void (*old_OnEnable)(void *, void *);
static void hk_OnEnable(void *self, void *mi) {
    if (self && std::find(g_players.begin(), g_players.end(), self) == g_players.end())
        g_players.push_back(self);
    old_OnEnable(self, mi);
}
static void (*old_OnDisable)(void *, void *);
static void hk_OnDisable(void *self, void *mi) {
    g_players.erase(std::remove(g_players.begin(), g_players.end(), self), g_players.end());
    old_OnDisable(self, mi);
}

static float NormDeg(float a) { while (a > 180.0f) a -= 360.0f; while (a < -180.0f) a += 360.0f; return a; }

static void Tick(void *local) {
    g_cntTick++;
    void *aim = *(void **)((uintptr_t)local + OFF_PC_AIM);
    if (!PtrOk(aim)) return;
    void *camT = *(void **)((uintptr_t)aim + OFF_AIM_CAMTRANS);
    if (!UnityAlive(camT)) return;

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
        float minX = *(float *)((uintptr_t)aim + OFF_AIM_MINX), maxX = *(float *)((uintptr_t)aim + OFF_AIM_MAXX);
        if (minX < maxX && minX > -91.0f && maxX < 91.0f) np = std::max(minX, std::min(maxX, np));
        void *ad = *(void **)((uintptr_t)aim + OFF_AIM_DATA);
        if (PtrOk(ad)) {
            Vector3 *caa = (Vector3 *)((uintptr_t)ad + OFF_AD_CURAIM);
            Vector3 *ce  = (Vector3 *)((uintptr_t)ad + OFF_AD_CUREUL);
            caa->x = np; caa->y = ny;
            ce->y = ny;
        }
        TrSetEuler(camT, Vector3{np, ny, 0.0f}, nullptr);
        applied = true;
        g_cntApplied++;
    }

    static time_t last = 0;
    time_t now = time(nullptr);
    if (now - last >= 5) {
        last = now;
        if (best) LOGI("oyuncu=%zu hedef: mesafe=%.1f bucaq=%.1f aim=%d", g_players.size(), bestDist, bestAng, (int)applied);
        else      LOGI("oyuncu=%zu dusmen tapilmadi", g_players.size());
    }
}

static void (*old_LateUpdate)(void *, void *);
static void hk_LateUpdate(void *self, void *mi) {
    g_cntLate++;
    old_LateUpdate(self, mi);
    if (!g_enabled) return;

    // Her saniye: cheating.txt-ni yoxla ve hiyleleri tetbiq et
    static time_t lastCfg = 0;
    time_t now = time(nullptr);
    if (now != lastCfg) {
        lastCfg = now;
        LoadConfig();
        ApplyAimConfig();
        ApplyCheats();
    }

    void *local = GetLocalPlayer();
    if (local && self == local) Tick(local);
}

// ---------------- Baslatma ----------------
static void Hook(const char *name, uintptr_t rva, void *rep, void **orig) {
    int r = DobbyHook((void *)(g_base + rva), rep, orig);
    LOGI("hook %s @%p -> %d", name, (void *)(g_base + rva), r);
}

static void *InitThread(void *) {
    EnsureTemplate();
    while (!(g_base = get_module_base("libil2cpp.so"))) usleep(100000);
    LOGI("libil2cpp baza: %p", (void *)g_base);
    sleep(8); // oyunun yuklenmesini gozle (tez hook cokme verir)

    TrGetPos = (Vec3Fn)(g_base + RVA_TR_GETPOS);
    TrGetFwd = (Vec3Fn)(g_base + RVA_TR_GETFWD);
    TrGetEuler = (Vec3Fn)(g_base + RVA_TR_GETEULER);
    TrSetEuler = (SetVec3Fn)(g_base + RVA_TR_SETEULER);
    CamGetInstance = (GetInstFn)(g_base + RVA_CAM_GETINSTANCE);
    GetGC = (GetGCFn)(g_base + RVA_GC_GETINSTANCE);
    GcYtb = (GcGetCvaFn)(g_base + RVA_GC_GETCVA);
    CvaLyl = (LylFn)(g_base + RVA_CVA_LYL);
    CpClone = (CloneFn)(g_base + RVA_CP_CLONE);

    Hook("OnEnable",   RVA_PC_ONENABLE,   (void *)hk_OnEnable,   (void **)&old_OnEnable);
    Hook("OnDisable",  RVA_PC_ONDISABLE,  (void *)hk_OnDisable,  (void **)&old_OnDisable);
    Hook("LateUpdate", RVA_PC_LATEUPDATE, (void *)hk_LateUpdate, (void **)&old_LateUpdate);
    LOGI("hazirdir");

    // Ureyin vurusu: her 10 san (maksimum ~10 deqiqe)
    for (int i = 0; i < 60; i++) {
        sleep(10);
        LOGI("heartbeat: late=%u tick=%u aim=%u oyuncular=%zu cva=%p", g_cntLate, g_cntTick, g_cntApplied, g_players.size(), g_cheats);
    }
    return nullptr;
}

__attribute__((constructor))
static void ModInit() {
    LOGI("MyLibName YUKLENDI (constructor) build=%s", BUILD_ID);
    pthread_t t;
    if (pthread_create(&t, nullptr, InitThread, nullptr) == 0) pthread_detach(t);
}
