# SPDX-License-Identifier: MIT
#
# Include after defining TARGET (.elf), OBJECTS and the module's build flags,
# but before rules using OBJECTS. The module links TARGET with LDFLAGS and LIBS.
ifndef LIBOUNCE_MODULE_INCLUDED
LIBOUNCE_MODULE_INCLUDED := 1

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO must point to the Switch 2 SDK)
endif
ifneq ($(suffix $(TARGET)),.elf)
$(error Set TARGET to the module's .elf filename before including libounce-module.mk)
endif

LIBOUNCE_MODULE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
LIBOUNCE_MODULE_NRO := $(TARGET:.elf=.nro)
LIBOUNCE_MODULE_OBJECTS := $(LIBOUNCE_MODULE_DIR)/lib/module-start.o

include $(DEVKITPRO)/libounce/switch2_cpu.mk
CC := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc
CXX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-g++
LIBOUNCE_MODULE_FLAGS := $(SWITCH2_CPU) -mtp=soft -fPIC -D__SWITCH__ -D__SWITCH2__
LIBOUNCE_MODULE_FLAGS += -mtls-dialect=trad -ftls-model=global-dynamic -DLIBOUNCE_MODULE_TLS
LIBOUNCE_MODULE_FLAGS += -I$(DEVKITPRO)/libounce/include -I$(DEVKITPRO)/portlibs/switch/include
CFLAGS += $(LIBOUNCE_MODULE_FLAGS)
CXXFLAGS += $(LIBOUNCE_MODULE_FLAGS) -fno-exceptions -fno-rtti -fuse-cxa-atexit
LDFLAGS += $(SWITCH2_CPU) -mtp=soft -nostdlib -shared -Wl,-Bsymbolic,--hash-style=sysv,-z,text
LDFLAGS += -Wl,-T,$(DEVKITPRO)/libounce/switch.ld
LDFLAGS += -Wl,-T,$(LIBOUNCE_MODULE_DIR)/module/module-tls.ld
LIBS += $(LIBOUNCE_MODULE_OBJECTS) -Wl,--whole-archive $(LIBOUNCE_MODULE_DIR)/lib/libounce_module.a -Wl,--no-whole-archive -lgcc

ifeq ($(strip $(.DEFAULT_GOAL)),)
.DEFAULT_GOAL := all
endif

all: $(LIBOUNCE_MODULE_NRO)

$(LIBOUNCE_MODULE_NRO): $(TARGET)
	$(DEVKITPRO)/tools/bin/elf2nro $< $@

$(TARGET): $(LIBOUNCE_MODULE_OBJECTS) $(LIBOUNCE_MODULE_DIR)/lib/libounce_module.a $(LIBOUNCE_MODULE_DIR)/libounce-module.mk $(LIBOUNCE_MODULE_DIR)/module/module-tls.ld $(DEVKITPRO)/libounce/switch.ld

.PHONY: libounce-module-clean

clean: libounce-module-clean

libounce-module-clean:
	$(RM) $(LIBOUNCE_MODULE_NRO)
endif
