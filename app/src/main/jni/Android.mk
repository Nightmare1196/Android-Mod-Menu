LOCAL_PATH := $(call my-dir)

# Dobby
include $(CLEAR_VARS)
LOCAL_MODULE := Dobby
LOCAL_SRC_FILES := Dobby/${TARGET_ARCH_ABI}/libdobby.a
include $(PREBUILT_STATIC_LIBRARY)

# Mod kitabxanasi (adi: libMyLibName.so, smali-deki loadLibrary("MyLibName") ile eyni qalir)
include $(CLEAR_VARS)
LOCAL_MODULE    := MyLibName

LOCAL_CFLAGS := -w -s -fexceptions
LOCAL_CPPFLAGS := -w -s -std=c++17 -fexceptions
LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all
LOCAL_LDLIBS := -llog
LOCAL_ARM_MODE := arm

LOCAL_C_INCLUDES += $(LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/Dobby/

# Yalniz Main.cpp qurulur. Menu/Jni.cpp, Menu/Menu.cpp, Menu/Setup.cpp (JNI_OnLoad) CIXARILIB.
LOCAL_SRC_FILES := Main.cpp

LOCAL_STATIC_LIBRARIES := Dobby

include $(BUILD_SHARED_LIBRARY)
