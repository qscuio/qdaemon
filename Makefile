# QDaemon Makefile

# Directories
BUILDDIR = build
SRCDIR = src
CLIENTDIR = client
EXAMPLEDIR = examples
TESTDIR = tests


CC ?= gcc
AR ?= ar
CFLAGS = -Wall -Wextra -Werror -std=gnu11 -D_GNU_SOURCE
CFLAGS += -Iinclude -Isrc/util -Isrc/cint
LDFLAGS = -lpthread -lrt -lreadline -lm 
LDFLAGS += -L$(BUILDDIR) -Wl,-rpath,'$$ORIGIN'

# Backend selection: epoll (default) or uring
BACKEND ?= epoll

ifeq ($(BACKEND), uring)
    CFLAGS += -DQD_BACKEND_URING
    LDFLAGS += -luring
else
    CFLAGS += -DQD_BACKEND_EPOLL
endif

DEBUG ?= 0
ifeq ($(DEBUG), 1)
    CFLAGS += -g -O0 -DDEBUG
else
    CFLAGS += -O2 -DNDEBUG
endif

# Sources
CORE_SRCS = $(wildcard $(SRCDIR)/core/*.c)

# Backend sources (only compile selected backend)
ifeq ($(BACKEND), uring)
    BACKEND_SRCS = $(SRCDIR)/core/backend/qd_backend_uring.c
else
    BACKEND_SRCS = $(SRCDIR)/core/backend/qd_backend_epoll.c
endif

IPC_SRCS = $(wildcard $(SRCDIR)/ipc/*.c)
DAEMON_SRCS = $(wildcard $(SRCDIR)/daemon/*.c)
CLI_SRCS = $(wildcard $(SRCDIR)/cli/*.c)
CLIENT_SRCS = $(wildcard $(CLIENTDIR)/src/*.c)

# CINT Sources
CINT_SRCS = $(wildcard $(SRCDIR)/cint/*.c)

# Objects
CORE_OBJS = $(CORE_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
BACKEND_OBJS = $(BACKEND_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
IPC_OBJS = $(IPC_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
DAEMON_OBJS = $(DAEMON_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
CLI_OBJS = $(CLI_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
CLIENT_OBJS = $(CLIENT_SRCS:$(CLIENTDIR)/%.c=$(BUILDDIR)/client/%.o)

CINT_OBJS = $(CINT_SRCS:$(SRCDIR)/cint/%.c=$(BUILDDIR)/cint/%.o)

ALL_OBJS = $(CORE_OBJS) $(BACKEND_OBJS) $(IPC_OBJS) $(DAEMON_OBJS) $(CLI_OBJS) $(CINT_OBJS)

# Targets
LIB_STATIC = $(BUILDDIR)/libqdaemon.a
LIB_SHARED = $(BUILDDIR)/libqdaemon.so
CLIENT_STATIC = $(BUILDDIR)/libqdclient.a
CLIENT_SHARED = $(BUILDDIR)/libqdclient.so

EXAMPLES = $(BUILDDIR)/simple_daemon $(BUILDDIR)/multi_instance $(BUILDDIR)/kernel_comm \
           $(BUILDDIR)/meta_cli $(BUILDDIR)/meta_server $(BUILDDIR)/meta_client \
           $(BUILDDIR)/qd_dhcpd $(BUILDDIR)/qd_dhcp_cli
TESTS = $(BUILDDIR)/test_memory $(BUILDDIR)/test_threadpool $(BUILDDIR)/test_event $(BUILDDIR)/test_ipc $(BUILDDIR)/test_aio $(BUILDDIR)/test_channel $(BUILDDIR)/test_channel_watch $(BUILDDIR)/test_workqueue

.PHONY: all clean examples tests install kmod kmods

all: $(LIB_STATIC) $(LIB_SHARED) $(CLIENT_STATIC) $(CLIENT_SHARED) examples tests kmods

examples: $(EXAMPLES)

tests: $(TESTS)

# Create directories
$(BUILDDIR)/core $(BUILDDIR)/core/backend $(BUILDDIR)/ipc $(BUILDDIR)/daemon $(BUILDDIR)/cli $(BUILDDIR)/cint $(BUILDDIR)/client/src:
	mkdir -p $@

# Object compilation
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)/core $(BUILDDIR)/core/backend $(BUILDDIR)/ipc $(BUILDDIR)/daemon $(BUILDDIR)/cli
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILDDIR)/client/%.o: $(CLIENTDIR)/%.c | $(BUILDDIR)/client/src
	$(CC) $(CFLAGS) -I$(CLIENTDIR)/include -fPIC -c $< -o $@

# CINT Compilation
$(BUILDDIR)/cint/%.o: $(SRCDIR)/cint/%.c | $(BUILDDIR)/cint
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

# Libraries
$(LIB_STATIC): $(ALL_OBJS)
	$(AR) rcs $@ $^

$(LIB_SHARED): $(ALL_OBJS)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

$(CLIENT_STATIC): $(CLIENT_OBJS)
	$(AR) rcs $@ $^

$(CLIENT_SHARED): $(CLIENT_OBJS)
	$(CC) -shared -o $@ $^ $(LDFLAGS)

# Examples
$(BUILDDIR)/simple_daemon: $(EXAMPLEDIR)/simple_daemon.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/multi_instance: $(EXAMPLEDIR)/multi_instance.c $(LIB_STATIC) $(CLIENT_STATIC)
	$(CC) $(CFLAGS) -I$(CLIENTDIR)/include $< -o $@ -L$(BUILDDIR) -lqdaemon -lqdclient $(LDFLAGS)

$(BUILDDIR)/kernel_comm: $(EXAMPLEDIR)/kernel_comm.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/meta_cli: $(EXAMPLEDIR)/af_meta/src/meta_cli.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -I$(EXAMPLEDIR)/af_meta/include -I$(SRCDIR)/cint $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/meta_server: $(EXAMPLEDIR)/af_meta/src/meta_server.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -I$(EXAMPLEDIR)/af_meta/include $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/meta_client: $(EXAMPLEDIR)/af_meta/src/meta_client.c $(LIB_STATIC) $(CLIENT_STATIC)
	$(CC) $(CFLAGS) -I$(EXAMPLEDIR)/af_meta/include -I$(CLIENTDIR)/include $< -o $@ -L$(BUILDDIR) -lqdaemon -lqdclient $(LDFLAGS)

# DHCP daemon sources
DHCP_SRCS = $(EXAMPLEDIR)/dhcp/src/qd_dhcpd.c \
            $(EXAMPLEDIR)/dhcp/src/dhcp_option.c \
            $(EXAMPLEDIR)/dhcp/src/dhcp_pool.c \
            $(EXAMPLEDIR)/dhcp/src/dhcp_lease.c \
            $(EXAMPLEDIR)/dhcp/src/dhcp_server.c \
            $(EXAMPLEDIR)/dhcp/src/dhcp_relay.c \
            $(EXAMPLEDIR)/dhcp/src/dhcp_rest.c \
            $(EXAMPLEDIR)/dhcp/src/dhcp_core.c \
            $(EXAMPLEDIR)/dhcp/src/dhcp_storage_file.c \
            $(EXAMPLEDIR)/dhcp/src/qd_dhcp_msg.c

$(BUILDDIR)/qd_dhcpd: $(DHCP_SRCS) $(LIB_STATIC)
	$(CC) $(CFLAGS) -I$(EXAMPLEDIR)/dhcp/include $(DHCP_SRCS) -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

# DHCP CLI
$(BUILDDIR)/qd_dhcp_cli: $(EXAMPLEDIR)/dhcp/cli/qd_dhcp_cli.c $(LIB_STATIC)
	$(CC) $(CFLAGS) -I$(EXAMPLEDIR)/dhcp/include $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

# Tests
$(BUILDDIR)/test_memory: $(TESTDIR)/test_memory.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/test_threadpool: $(TESTDIR)/test_threadpool.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/test_event: $(TESTDIR)/test_event.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/test_ipc: $(TESTDIR)/test_ipc.c $(LIB_STATIC) $(CLIENT_STATIC)
	$(CC) $(CFLAGS) -I$(CLIENTDIR)/include $< -o $@ -L$(BUILDDIR) -lqdaemon -lqdclient $(LDFLAGS)

$(BUILDDIR)/test_aio: $(TESTDIR)/test_aio.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/test_channel: $(TESTDIR)/test_channel.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/test_channel_watch: $(TESTDIR)/test_channel_watch.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/test_workqueue: $(TESTDIR)/test_workqueue.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/bench_event: $(TESTDIR)/bench_event.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/bench_aio: $(TESTDIR)/bench_aio.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

# Run tests
test: tests
	@for t in $(TESTS); do echo "Running $$t..."; $$t || exit 1; done
	@echo "All tests passed!"

# Kernel module
kmod:
	$(MAKE) -C kernel

# Kernel modules (core + examples)
kmods: kmod
	$(MAKE) -C examples/af_meta/kernel
	$(MAKE) -C examples/dhcp/kernel

# Installation
PREFIX ?= /usr/local
install: all
	install -d $(PREFIX)/lib $(PREFIX)/include/qdaemon
	install -m 644 $(LIB_STATIC) $(LIB_SHARED) $(PREFIX)/lib/
	install -m 644 $(CLIENT_STATIC) $(CLIENT_SHARED) $(PREFIX)/lib/
	install -m 644 include/qdaemon/*.h $(PREFIX)/include/qdaemon/
	install -m 644 $(CLIENTDIR)/include/qdclient.h $(PREFIX)/include/

clean:
	rm -rf $(BUILDDIR)

# Dependencies
-include $(ALL_OBJS:.o=.d) $(CLIENT_OBJS:.o=.d)
