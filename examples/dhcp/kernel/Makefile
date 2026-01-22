# DHCP Kernel Module Makefile

obj-m := qd_dhcp_kmod.o

KDIR ?= /lib/modules/$(shell uname -r)/build
ROOTDIR ?= $(abspath $(CURDIR)/../../..)
BUILDDIR ?= $(ROOTDIR)/build
KMOD_OUTPUT ?= $(BUILDDIR)/kmods/dhcp

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(KMOD_OUTPUT) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(KMOD_OUTPUT) clean

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(KMOD_OUTPUT) modules_install
	depmod -a

load:
	insmod $(KMOD_OUTPUT)/qd_dhcp_kmod.ko

unload:
	rmmod qd_dhcp_kmod

.PHONY: all clean install load unload
