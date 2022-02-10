#Build marlin2
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    wcnd_marlin2.c \
    wcnd_atcmd.c \
    wcnd_cmd.c \
    wcnd_sm.c \
    wcnd_util.c

ifeq ($(USE_SPRD_ENG), true)
LOCAL_CFLAGS += -DCONFIG_ENG_MODE
LOCAL_SRC_FILES += \
    wcnd_eng_cmd_executer.c \
    wcnd_eng_wifi_priv.c
endif

LOCAL_C_INCLUDES := $(LOCAL_PATH)/../../connmgr

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libnetutils \
    liblog

LOCAL_MODULE := libmarlin2
LOCAL_PROPRIETARY_MODULE := true

include $(BUILD_STATIC_LIBRARY)