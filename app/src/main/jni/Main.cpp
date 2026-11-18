// language: C++, file: Main.cpp, target: Android arm64-v8a (IL2CPP)
#include <jni.h>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>
#include <mutex>
#include <algorithm>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <android/log.h>

#define LOG_TAG "PHANTOM_AIMBOT"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Dobby hook üçün xarici funksiya
extern "C" int DobbyHook(void *address, void *replace_call, void **origin_call);

// --- Riyaziyyat Strukturları ---
struct Vector3 {
    float x, y, z;
    Vector3 operator-(const Vector3& o) const { return {x - o.x, y - o.y, z - o.z}; }
};

struct Matrix4x4 {
    float m00, m10, m20, m30;
    float m01, m11, m21, m31;
    float m02, m12, m22, m32;
    float m03, m13, m23, m33;
};

// --- Offsetlər (StandChillow 2.07 F1) ---
#define OFFSET_PlayerMainCamera_Instance 0x42BD014
#define OFFSET_PlayerController_OnEnable 0x42B9630
#define OFFSET_PlayerController_OnDisable 0x42B96B8
#define OFFSET_AimController_SetTarget   0x42EC7E4
#define OFFSET_Transform_GetPosition     0x77CF6F4

// --- Menyu Dəyişənləri (Test üçün default aktivdir) ---
bool aimbot_enabled = true; 
bool team_check = true;
float aimbot_fov = 15.0f;
float aimbot_max_dist = 150.0f;

// --- Oyun Daxili Dəyişənlər ---
std::vector<void*> playerList;
std::mutex playerMutex;
uintptr_t il2cpp_base = 0;

// --- Funksiya Pointerləri ---
typedef void (*GetPositionFn)(void*, Vector3*);
GetPositionFn GetTransformPosition = nullptr;

// --- Köməkçi Funksiyalar ---
uintptr_t get_module_base(const char* module_name) {
    uintptr_t base = 0;
    FILE *fp = fopen("/proc/self/maps", "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, module_name)) {
                base = strtoul(line, NULL, 16);
                break;
            }
        }
        fclose(fp);
    }
    return base;
}

bool IsValidPtr(void* obj) {
    if (!obj) return false;
    return *(void**)((uintptr_t)obj + 0x10) != nullptr;
}

Vector3 GetPosition(void* transform) {
    Vector3 pos = {0, 0, 0};
    if (transform && GetTransformPosition) {
        GetTransformPosition(transform, &pos);
    }
    return pos;
}

// --- Hook Funksiyaları ---
void (*old_PlayerController_OnEnable)(void*);
void PlayerController_OnEnable(void* instance) {
    if (instance) {
        std::lock_guard<std::mutex> lock(playerMutex);
        if (std::find(playerList.begin(), playerList.end(), instance) == playerList.end()) {
            playerList.push_back(instance);
        }
    }
    old_PlayerController_OnEnable(instance);
}

void (*old_PlayerController_OnDisable)(void*);
void PlayerController_OnDisable(void* instance) {
    if (instance) {
        std::lock_guard<std::mutex> lock(playerMutex);
        playerList.erase(std::remove(playerList.begin(), playerList.end(), instance), playerList.end());
    }
    old_PlayerController_OnDisable(instance);
}

// --- Aimbot Dövrü ---
void AimbotThread() {
    while (true) {
        if (aimbot_enabled && il2cpp_base) {
            auto mainCamGetInstance = (void*(*)())(il2cpp_base + OFFSET_PlayerMainCamera_Instance);
            void* mainCam = mainCamGetInstance();
            
            if (mainCam) {
                auto localPlayer = *(void**)((uintptr_t)mainCam + 0x40); 
                
                if (localPlayer && IsValidPtr(localPlayer)) {
                    uint8_t localTeam = *(uint8_t*)((uintptr_t)localPlayer + 0x79); 
                    auto localAim = *(void**)((uintptr_t)localPlayer + 0xD0);       
                    
                    if (localAim) {
                        auto camTransform = *(void**)((uintptr_t)localAim + 0x78);  
                        
                        if (camTransform) {
                            Vector3 camPos = GetPosition(camTransform);
                            Matrix4x4 camMat = *(Matrix4x4*)((uintptr_t)camTransform + 0x80);
                            Vector3 camForward = { camMat.m02, camMat.m12, camMat.m22 };
                            
                            void* bestTarget = nullptr;
                            float bestDist = 999999.0f;
                            Vector3 bestHeadPos = {0, 0, 0};
                            
                            std::lock_guard<std::mutex> lock(playerMutex);
                            for (auto player : playerList) {
                                if (player == localPlayer) continue;
                                if (!IsValidPtr(player)) continue;
                                if (team_check && *(uint8_t*)((uintptr_t)player + 0x79) == localTeam) continue;
                                
                                auto biped = *(void**)((uintptr_t)player + 0x30); 
                                if (!biped) continue;
                                
                                auto headTrans = *(void**)((uintptr_t)biped + 0x20); 
                                if (!headTrans) continue;
                                
                                Vector3 headPos = GetPosition(headTrans);
                                Vector3 dir = headPos - camPos;
                                float dist = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
                                if (dist > aimbot_max_dist) continue;
                                
                                float dot = (dir.x*camForward.x + dir.y*camForward.y + dir.z*camForward.z) / dist;
                                float cosFov = std::cos(aimbot_fov * 0.5f * 3.14159265f / 180.0f);
                                if (dot < cosFov) continue;
                                
                                if (dist < bestDist) {
                                    bestDist = dist;
                                    bestTarget = player;
                                    bestHeadPos = headPos;
                                }
                            }
                            
                            if (bestTarget) {
                                typedef void (*AimSetTarget)(void*, Vector3);
                                AimSetTarget setTarget = (AimSetTarget)(il2cpp_base + OFFSET_AimController_SetTarget);
                                setTarget(localAim, bestHeadPos);
                            }
                        }
                    }
                }
            }
        }
        usleep(5000);
    }
}

// --- XÜSUSİ FUNKSIYA: Kitabxana yüklənəndə avtomatik işə düşür (JNI_OnLoad yerinə) ---
__attribute__((constructor))
void InitAimbot() {
    std::thread([]() {
        // libil2cpp.so yüklənənə qədər gözləyirik
        while (!il2cpp_base) {
            il2cpp_base = get_module_base("libil2cpp.so");
            if (!il2cpp_base) {
                usleep(100000); // 100ms gözlə
            }
        }
        
        GetTransformPosition = (GetPositionFn)(il2cpp_base + OFFSET_Transform_GetPosition);
        
        void* targetOnEnable = (void*)(il2cpp_base + OFFSET_PlayerController_OnEnable);
        DobbyHook(targetOnEnable, (void*)PlayerController_OnEnable, (void**)&old_PlayerController_OnEnable);
        
        void* targetOnDisable = (void*)(il2cpp_base + OFFSET_PlayerController_OnDisable);
        DobbyHook(targetOnDisable, (void*)PlayerController_OnDisable, (void**)&old_PlayerController_OnDisable);
        
        LOGI("PHANTOM: Aimbot hazırdır. Baza: %p", (void*)il2cpp_base);
        
        std::thread(AimbotThread).detach();
    }).detach();
}