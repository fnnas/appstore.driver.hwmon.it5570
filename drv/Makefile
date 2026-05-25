# For building for the current running version of Linux
ifndef TARGET
TARGET = $(shell uname -r)
endif

KERNEL_MODULES = /lib/modules/$(TARGET)

ifneq ("","$(wildcard /usr/src/linux-headers-$(TARGET)/*)")
# Ubuntu
KERNEL_BUILD = /usr/src/linux-headers-$(TARGET)
else
ifneq ("","$(wildcard /usr/src/kernels/$(TARGET)/*)")
# Fedora
KERNEL_BUILD = /usr/src/kernels/$(TARGET)
else
KERNEL_BUILD = $(KERNEL_MODULES)/build
endif
endif

DRIVER := it5570_hwmon
DRIVER_DIR := $(if $(M),$(M),$(CURDIR))
REPO_DIR := $(abspath $(DRIVER_DIR)/..)
GIT_COMMIT := $(shell git -C $(REPO_DIR) rev-parse --short HEAD 2>/dev/null || echo none)

obj-m := $(DRIVER).o
$(DRIVER)-y := it5570_main.o it5570_core.o it5570_sensors.o
ccflags-y += -DIT5570_DRIVER_REVISION=\"$(GIT_COMMIT)\"

MAKEFLAGS += --no-print-directory

.PHONY: all modules clean

all: modules

modules:
	@$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR) $@

clean:
	@$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR) $@
