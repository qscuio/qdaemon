# Kernel module Makefile

obj-m := qd_kmod.o

KDIR ?= /lib/modules/$(shell uname -r)/build
ROOTDIR ?= $(abspath $(CURDIR)/..)
BUILDDIR ?= $(ROOTDIR)/build
KMOD_OUTPUT ?= $(BUILDDIR)/kmods/kernel

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(KMOD_OUTPUT) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(KMOD_OUTPUT) clean

install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) MO=$(KMOD_OUTPUT) modules_install
	depmod -a

.PHONY: all clean install
