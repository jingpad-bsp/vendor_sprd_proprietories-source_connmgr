LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_32_BIT_ONLY := true

LOCAL_SRC_FILES:= \
    wcnd.c \
    wcnd_worker.c \

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libnetutils \
    liblog

LIB_BOARD_WCN_COMBO := libmarlin2
ifeq ($(strip $(BOARD_HAVE_SPRD_WCN_COMBO)), sharkle)
LIB_BOARD_WCN_COMBO := libmarlin2
else ifeq ($(strip $(BOARD_HAVE_SPRD_WCN_COMBO)), sharkl3)
LIB_BOARD_WCN_COMBO := libmarlin2
else ifeq ($(strip $(BOARD_HAVE_SPRD_WCN_COMBO)), pike2)
LIB_BOARD_WCN_COMBO := libmarlin2
else ifeq ($(strip $(BOARD_HAVE_SPRD_WCN_COMBO)), marlin3)
LIB_BOARD_WCN_COMBO := libmarlin2
else ifeq ($(strip $(BOARD_HAVE_SPRD_WCN_COMBO)), marlin3_lite)
LIB_BOARD_WCN_COMBO := libmarlin2
endif

LOCAL_STATIC_LIBRARIES := $(LIB_BOARD_WCN_COMBO)

ifeq ($(USE_SPRD_ENG), true)
LOCAL_CFLAGS += -DCONFIG_ENG_MODE
LOCAL_SHARED_LIBRARIES += \
    libiwnpi \
    libengbt
endif

ifeq ($(BOARD_WLAN_DEVICE), bcmdhd)
LOCAL_CFLAGS += -DHAVE_SLEEPMODE_CONFIG
endif

ifneq (,$(filter $(MALI_PLATFORM_NAME),pike2 sharkl3 sharkle))
LOCAL_CFLAGS += -DCP2_GE2_COEXT
endif

LOCAL_MODULE := connmgr
LOCAL_INIT_RC := wcnd.rc
LOCAL_MODULE_TAGS := optional

LOCAL_PROPRIETARY_MODULE:= true
LOCAL_REQUIRED_MODULES := connmgr_cli

include $(BUILD_EXECUTABLE)


#build connmgr_cli
include $(CLEAR_VARS)
LOCAL_C_INCLUDES := ./include
LOCAL_SRC_FILES:= \
    wcnd_cli.c


LOCAL_SHARED_LIBRARIES := \
    libcutils \
	liblog

LOCAL_MODULE := connmgr_cli
LOCAL_PROPRIETARY_MODULE:= true
LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)
include $(call all-makefiles-under,$(LOCAL_PATH))
