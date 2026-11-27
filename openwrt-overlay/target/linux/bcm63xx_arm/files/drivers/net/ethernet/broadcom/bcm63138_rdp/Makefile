# SPDX-License-Identifier: GPL-2.0
ccflags-y += \
	-include $(src)/include/rdp_kbuild_compat.h \
	-I$(src)/include \
	-I$(src)/include/rdp \
	-I$(src)/include/rdpa_gpl \
	-I$(src)/rdp_subsystem \
	-I$(src)/rdd \
	-DDSL_63138 \
	-DCONFIG_BCM963138 \
	-DBCM_DSL_RDP \
	-DLEGACY_RDP \
	-D_BYTE_ORDER_LITTLE_ENDIAN_ \
	-DUSE_SOC_BASE_ADDR \
	-Wno-unused-variable \
	-Wno-unused-function \
	-Wno-unused-but-set-variable \
	-Wno-missing-prototypes \
	-Wno-missing-declarations \
	-Wno-strict-prototypes \
	-Wno-old-style-definition \
	-Wno-pointer-sign \
	-Wno-int-conversion \
	-Wno-enum-conversion \
	-Wno-implicit-fallthrough

obj-y += \
	bcm63138_cmdlist.o \
	rdp_compat.o \
	rdp_subsystem/data_path_init.o \
	rdp_subsystem/rdp_drv_bbh.o \
	rdp_subsystem/rdp_drv_bpm.o \
	rdp_subsystem/rdp_drv_ih.o \
	rdp_subsystem/rdp_drv_sbpm.o \
	rdp_subsystem/rdp_bbh_arrays.o \
	rdp_subsystem/rdp_dma_arrays.o \
	rdp_subsystem/rdp_runner_arrays.o \
	rdp_subsystem/runner_fw_a.o \
	rdp_subsystem/runner_fw_b.o \
	rdp_subsystem/runner_fw_c.o \
	rdp_subsystem/runner_fw_d.o \
	rdp_subsystem/predict_runner_fw_a.o \
	rdp_subsystem/predict_runner_fw_b.o \
	rdp_subsystem/predict_runner_fw_c.o \
	rdp_subsystem/predict_runner_fw_d.o \
	rdd/rdd_init.o \
	rdd/rdd_cpu.o \
	rdd/rdd_common.o \
	rdd/rdd_crc.o \
	rdd/rdd_tm.o \
	rdd/rdd_lookup_engine.o \
	rdd/rdd_utils.o \
	rdd/rdd_router.o \
	rdd/rdd_bridge.o \
	rdd/rdd_interworking.o \
	rdd/rdd_l2_ucast.o \
	rdd/rdd_service_queues.o
