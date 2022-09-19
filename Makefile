# SPDX-License-Identifier: GPL-2.0-only
ccflags-y+=-Werror

obj-m += optee.o
obj-y += optee/

optee-objs := tee_core.o \
	      tee_shm.o \
	      tee_shm_pool.o \
	      tee_data_pipe.o

LOCAL_INCLUDES += -I$(M)/include \
                -I$(M)/include/linux \
                -I$(KERNEL_SRC)/$(M)/include \
                -I$(KERNEL_SRC)/$(M)/include/linux

ccflags-y+=$(LOCAL_INCLUDES)
EXTRA_CFLAGS += $(LOCAL_INCLUDES)

all:
	@$(MAKE) -C $(KERNEL_SRC) M=$(M)  modules
	#@$(MAKE) -C $(KERNEL_SRC) M=$(M)/optee --trace  modules

modules_install:
	@echo "$(MAKE) INSTALL_MOD_STRIP=1 M=$(M) -C $(KERNEL_SRC) modules_install"
	@$(MAKE) INSTALL_MOD_STRIP=1 M=$(M) -C $(KERNEL_SRC) modules_install
	mkdir -p ${OUT_DIR}/../vendor_lib
	if [ $(TARGET_BUILD_KERNEL_VERSION) = 5.15 ]; then \
		find $(INSTALL_MOD_PATH)/lib/modules/*/$(INSTALL_MOD_DIR) -name "*.ko" -exec cp {} ${OUT_DIR}/../vendor_lib/ \;	; \
	else \
		find $(INSTALL_MOD_PATH)/lib/modules/*/extra -name "*.ko" -exec cp {} ${OUT_DIR}/../vendor_lib/ \; ; \
	fi

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) clean
