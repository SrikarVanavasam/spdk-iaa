SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = snic

C_SRCS := snic.c

SPDK_LIB_LIST = $(ALL_MODULES_LIST) event event_bdev nvme

CFLAGS += -I$(SPDK_ROOT_DIR)/include
LIBS += -lrdmacm -libverbs -lpthread

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
