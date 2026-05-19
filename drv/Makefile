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

obj-m := $(DRIVER).o

MAKEFLAGS += --no-print-directory

.PHONY: all modules clean

all: modules

modules:
	@$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR) $@

clean:
	@$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR) $@
