#
# QDaemon DHCP Server Makefile
#

# Build mode: set STANDALONE=1 for building without libqdaemon
STANDALONE ?= 0

# QDaemon root directory (only used when STANDALONE=0)
QDAEMON_ROOT ?= ../..

# Compiler settings
CC ?= gcc
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE
CFLAGS += -Iinclude
CFLAGS += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)

ifeq ($(STANDALONE),1)
    # Standalone mode - use stub library
    CFLAGS += -DSTANDALONE
    LDFLAGS =
    LIBS = -lpthread -lrt -lreadline
    STUB_SRC = src/qdaemon_stub.c
    STUB_OBJ = src/qdaemon_stub.o
else
    # Normal mode - use libqdaemon
    CFLAGS += -I$(QDAEMON_ROOT)/include
    LDFLAGS = -L$(QDAEMON_ROOT)/lib
    LIBS = -lqdaemon -lpthread -lrt -lreadline
    STUB_SRC =
    STUB_OBJ =
endif

# Debug/Release
ifdef DEBUG
    CFLAGS += -g -O0 -DDEBUG
else
    CFLAGS += -O2 -DNDEBUG
endif

# Source files
COMMON_SRCS = \
	src/dhcp_option.c \
	src/dhcp_pool.c \
	src/dhcp_lease.c \
	src/dhcp_server.c \
	src/dhcp_relay.c \
	src/dhcp_rest.c \
	src/qd_dhcp_msg.c

DAEMON_SRCS = \
	src/qd_dhcpd.c \
	$(COMMON_SRCS)

CLI_SRCS = cli/qd_dhcp_cli.c

# Object files
DAEMON_OBJS = $(DAEMON_SRCS:.c=.o)
CLI_OBJS = $(CLI_SRCS:.c=.o)

# Targets
DAEMON = qd_dhcpd
CLI = qd_dhcp_cli

# Test programs
TEST_RAWSOCK = test_rawsock

# Build all
all: $(DAEMON) $(CLI)

# Build test programs
test: $(TEST_RAWSOCK)

$(TEST_RAWSOCK): test_rawsock.c include/dhcp_kmod.h include/dhcp_protocol.h
	$(CC) -Wall -Wextra -std=c11 -D_GNU_SOURCE -Iinclude -o $@ $<

# Daemon
$(DAEMON): $(DAEMON_OBJS) $(STUB_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

# CLI client
$(CLI): $(CLI_OBJS) $(STUB_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

# Compile rule
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Clean
clean:
	rm -f $(DAEMON) $(CLI) $(TEST_RAWSOCK)
	rm -f src/*.o cli/*.o
	rm -f $(STUB_OBJ)

# Kernel module (separate build)
kmods:
	$(MAKE) -C kernel

kmods-clean:
	$(MAKE) -C kernel clean

# Install
install: $(DAEMON) $(CLI)
	install -d $(DESTDIR)/usr/local/bin
	install -m 755 $(DAEMON) $(DESTDIR)/usr/local/bin/
	install -m 755 $(CLI) $(DESTDIR)/usr/local/bin/

# System install (for packaging)
install-system: $(DAEMON) $(CLI) $(TEST_RAWSOCK)
	install -d $(DESTDIR)/usr/sbin
	install -m 755 $(DAEMON) $(DESTDIR)/usr/sbin/
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(CLI) $(DESTDIR)/usr/bin/
	install -m 755 $(TEST_RAWSOCK) $(DESTDIR)/usr/bin/qd_dhcp_test
	install -d $(DESTDIR)/etc/qd-dhcpd
	install -m 644 etc/qd-dhcpd/qd-dhcpd.conf $(DESTDIR)/etc/qd-dhcpd/
	install -d $(DESTDIR)/etc/default
	install -m 644 etc/default/qd-dhcpd $(DESTDIR)/etc/default/
	install -d $(DESTDIR)/var/lib/qd-dhcpd
	install -d $(DESTDIR)/usr/include/qd-dhcpd
	install -m 644 include/dhcp_kmod.h $(DESTDIR)/usr/include/qd-dhcpd/

# Debian package build
deb:
	dpkg-buildpackage -us -uc -b

deb-clean:
	debian/rules clean
	rm -f ../qd-dhcpd_*.deb ../qd-dhcpd_*.changes ../qd-dhcpd_*.buildinfo

# Dependencies
src/dhcp_option.o: include/dhcp_option.h include/dhcp_protocol.h
src/dhcp_pool.o: include/dhcp_pool.h include/dhcp_protocol.h
src/dhcp_lease.o: include/dhcp_lease.h include/dhcp_protocol.h
src/dhcp_server.o: include/dhcp_server.h include/dhcp_pool.h include/dhcp_lease.h include/dhcp_option.h
src/dhcp_relay.o: include/dhcp_relay.h include/dhcp_option.h include/dhcp_protocol.h
src/dhcp_rest.o: include/dhcp_server.h include/dhcp_relay.h include/dhcp_ipc.h
src/qd_dhcp_msg.o: include/qd_dhcp_msg.h include/dhcp_kmod.h src/qd_dhcp_msg_internal.h
src/qd_dhcpd.o: include/dhcp_protocol.h include/dhcp_server.h include/dhcp_relay.h include/dhcp_ipc.h include/qd_dhcp_msg.h
cli/qd_dhcp_cli.o: include/dhcp_ipc.h include/dhcp_protocol.h

.PHONY: all clean test kmods kmods-clean install install-system deb deb-clean
