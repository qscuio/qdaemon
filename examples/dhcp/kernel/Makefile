# DHCP Kernel Module Makefile
#
# Builds qd_dhcp_kmod.ko with:
# - Virtual network device (qdhcp0) for userspace communication
# - DHCP packet interception via dev_add_pack() and TC ingress hook
# - DHCP snooping with binding table
# - Netlink for control messages
#

obj-m := qd_dhcp_kmod.o

KDIR ?= /lib/modules/$(shell uname -r)/build
ROOTDIR ?= $(abspath $(CURDIR)/../../..)
BUILDDIR ?= $(ROOTDIR)/build
KMOD_OUTPUT ?= $(BUILDDIR)/kmods/dhcp

ccflags-y := -DDEBUG

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(KMOD_OUTPUT) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(KMOD_OUTPUT) clean

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(KMOD_OUTPUT) modules_install
	depmod -a

# Load with both interception methods (default)
load:
	insmod qd_dhcp_kmod.ko intercept_method=3

# Load with dev_add_pack only
load-pack:
	insmod qd_dhcp_kmod.ko intercept_method=1

# Load with TC ingress only
load-tc:
	insmod qd_dhcp_kmod.ko intercept_method=2

unload:
	rmmod qd_dhcp_kmod

.PHONY: all clean install load load-pack load-tc unload
