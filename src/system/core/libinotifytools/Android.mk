LOCAL_PATH:= $(call my-dir)

# build host static library
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	redblack.c \
	inotifytools.c

LOCAL_STATIC_LIBRARIES := \
	libinofitytools

LOCAL_MODULE:= libinotifytools

LOCAL_C_INCLUDES := $(KERNEL_HEADERS)
include $(BUILD_HOST_STATIC_LIBRARY)
