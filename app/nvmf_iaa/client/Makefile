SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk

APP = client

C_SRCS := snic_client_lib.c client_demo.c

# Link NVMe and Environment libraries
SPDK_LIB_LIST = $(ALL_MODULES_LIST) nvme

CFLAGS += -I$(SPDK_ROOT_DIR)/include
LIBS += -lrdmacm -libverbs -lpthread

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
