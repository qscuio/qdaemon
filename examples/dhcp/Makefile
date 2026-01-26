#
# QDaemon DHCP Server Makefile
#

# QDaemon root directory
QDAEMON_ROOT ?= ../..

# Compiler settings
CC ?= gcc
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE
CFLAGS += -I$(QDAEMON_ROOT)/include
CFLAGS += -Iinclude

# Debug/Release
ifdef DEBUG
    CFLAGS += -g -O0 -DDEBUG
else
    CFLAGS += -O2 -DNDEBUG
endif

# Libraries
LDFLAGS = -L$(QDAEMON_ROOT)/lib
LIBS = -lqdaemon -lpthread -lrt -lreadline

# Source files
COMMON_SRCS = \
	src/dhcp_option.c \
	src/dhcp_pool.c \
	src/dhcp_lease.c \
	src/dhcp_server.c \
	src/dhcp_relay.c \
	src/dhcp_rest.c

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

# Build all
all: $(DAEMON) $(CLI)

# Daemon
$(DAEMON): $(DAEMON_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

# CLI client
$(CLI): $(CLI_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)

# Compile rule
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Clean
clean:
	rm -f $(DAEMON) $(CLI)
	rm -f src/*.o cli/*.o

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

# Dependencies
src/dhcp_option.o: include/dhcp_option.h include/dhcp_protocol.h
src/dhcp_pool.o: include/dhcp_pool.h include/dhcp_protocol.h
src/dhcp_lease.o: include/dhcp_lease.h include/dhcp_protocol.h
src/dhcp_server.o: include/dhcp_server.h include/dhcp_pool.h include/dhcp_lease.h include/dhcp_option.h
src/dhcp_relay.o: include/dhcp_relay.h include/dhcp_option.h include/dhcp_protocol.h
src/dhcp_rest.o: include/dhcp_server.h include/dhcp_relay.h include/dhcp_ipc.h
src/qd_dhcpd.o: include/dhcp_protocol.h include/dhcp_server.h include/dhcp_relay.h include/dhcp_ipc.h
cli/qd_dhcp_cli.o: include/dhcp_ipc.h include/dhcp_protocol.h

.PHONY: all clean kmods kmods-clean install
