# Originally sourced from https://github.com/a1wong/it87/blob/master/Makefile
# For building for the current running version of Linux
ifndef TARGET
TARGET		:= $(shell uname -r)
endif
# Or specific version
#TARGET		:= 2.6.33.5

KERNEL_MODULES	:= /lib/modules/$(TARGET)

ifneq ("","$(wildcard /usr/src/linux-headers-$(TARGET)/*)")
# Ubuntu
KERNEL_BUILD	:= /usr/src/linux-headers-$(TARGET)
else
ifneq ("","$(wildcard /usr/src/kernels/$(TARGET)/*)")
# Fedora
KERNEL_BUILD	:= /usr/src/kernels/$(TARGET)
else
KERNEL_BUILD	:= $(KERNEL_MODULES)/build
endif
endif

#SYSTEM_MAP	:= $(KERNEL_BUILD)/System.map
ifneq ("","$(wildcard /boot/System.map-$(TARGET))")
SYSTEM_MAP	:= /boot/System.map-$(TARGET)
else
# Arch
SYSTEM_MAP	:= /proc/kallsyms
endif

DRIVER := asus-wmi-sensors
ifneq ("","$(wildcard .git/*)")
DRIVER_VERSION := $(shell git describe --long --always)
else
ifneq ("", "$(wildcard VERSION)")
DRIVER_VERSION := $(shell cat VERSION)
else
DRIVER_VERSION := unknown
endif
endif

# DKMS
DKMS_ROOT_PATH=/usr/src/$(DRIVER)-$(DRIVER_VERSION)
MODPROBE_OUTPUT=$(shell lsmod | grep asus-wmi-sensors)

# Directory below /lib/modules/$(TARGET)/kernel into which to install
# the module:
MOD_SUBDIR = drivers/hwmon
MODDESTDIR=$(KERNEL_MODULES)/kernel/$(MOD_SUBDIR)

obj-m	:= $(patsubst %,%.o,$(DRIVER))
obj-ko  := $(patsubst %,%.ko,$(DRIVER))

MAKEFLAGS += --no-print-directory

ifneq ("","$(wildcard $(MODDESTDIR)/*.ko.gz)")
COMPRESS_GZIP := y
endif
ifneq ("","$(wildcard $(MODDESTDIR)/*.ko.xz)")
COMPRESS_XZ := y
endif

.PHONY: all install modules modules_install clean dkms dkms_clean

all: modules

# Targets for running make directly in the external module directory:
# Add -g -DDEBUG to build a debug module
ASUS_WMI_SENSORS_CFLAGS=-DASUS_WMI_SENSORS_DRIVER_VERSION='\"$(DRIVER_VERSION)\"'

modules:
	@$(MAKE) EXTRA_CFLAGS="$(ASUS_WMI_SENSORS_CFLAGS)" -C $(KERNEL_BUILD) M=$(CURDIR) $@

clean:
	@$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR) $@

install: modules_install

modules_install:
	mkdir -p $(MODDESTDIR)
	cp $(DRIVER).ko $(MODDESTDIR)/
ifeq ($(COMPRESS_GZIP), y)
	@gzip -f $(MODDESTDIR)/$(DRIVER).ko
endif
ifeq ($(COMPRESS_XZ), y)
	@xz -f $(MODDESTDIR)/$(DRIVER).ko
endif
	depmod -a -F $(SYSTEM_MAP) $(TARGET)

dkms:
	@cp --preserve dkms.conf.am dkms.conf
	@sed -i -e '/^PACKAGE_VERSION=/ s/=.*/=\"$(DRIVER_VERSION)\"/' dkms.conf
	@echo "$(DRIVER_VERSION)" >VERSION
	@mkdir $(DKMS_ROOT_PATH)
	@cp dkms.conf $(DKMS_ROOT_PATH)
	@cp VERSION $(DKMS_ROOT_PATH)
	@cp Makefile $(DKMS_ROOT_PATH)
	@cp asus-wmi-sensors.c $(DKMS_ROOT_PATH)
	@dkms add -m $(DRIVER) -v $(DRIVER_VERSION)
	@dkms build -m $(DRIVER) -v $(DRIVER_VERSION)
	@dkms install --force -m $(DRIVER) -v $(DRIVER_VERSION)
	@modprobe $(DRIVER)

dkms_clean:
	@if [ ! -z "$(MODPROBE_OUTPUT)" ]; then \
		rmmod $(DRIVER);\
	fi
	@dkms remove -m $(DRIVER) -v $(DRIVER_VERSION) --all
	@rm -rf $(DKMS_ROOT_PATH)
	@rm -- VERSION dkms.conf
