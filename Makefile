# QDaemon Makefile
# Alternative to CMake build

CC ?= gcc
AR ?= ar
CFLAGS = -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE
CFLAGS += -Iinclude -Isrc/util -Isrc/cint
LDFLAGS = -lpthread -lrt -lreadline -lm

DEBUG ?= 0
ifeq ($(DEBUG), 1)
    CFLAGS += -g -O0 -DDEBUG
else
    CFLAGS += -O2 -DNDEBUG
endif

# Directories
BUILDDIR = build
SRCDIR = src
CLIENTDIR = client
EXAMPLEDIR = examples
TESTDIR = tests

# Core sources
CORE_SRCS = $(wildcard $(SRCDIR)/core/*.c)
IPC_SRCS = $(wildcard $(SRCDIR)/ipc/*.c)
DAEMON_SRCS = $(wildcard $(SRCDIR)/daemon/*.c)
CLI_SRCS = $(wildcard $(SRCDIR)/cli/*.c)
CINT_SRCS = $(wildcard $(SRCDIR)/cint/*.c)
CLIENT_SRCS = $(wildcard $(CLIENTDIR)/src/*.c)

# Objects
CORE_OBJS = $(CORE_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
IPC_OBJS = $(IPC_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
DAEMON_OBJS = $(DAEMON_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
CLI_OBJS = $(CLI_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
CINT_OBJS = $(CINT_SRCS:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)
CLIENT_OBJS = $(CLIENT_SRCS:$(CLIENTDIR)/%.c=$(BUILDDIR)/client/%.o)

ALL_OBJS = $(CORE_OBJS) $(IPC_OBJS) $(DAEMON_OBJS) $(CLI_OBJS) $(CINT_OBJS)

# Targets
LIB_STATIC = $(BUILDDIR)/libqdaemon.a
LIB_SHARED = $(BUILDDIR)/libqdaemon.so
CLIENT_STATIC = $(BUILDDIR)/libqdclient.a
CLIENT_SHARED = $(BUILDDIR)/libqdclient.so

EXAMPLES = $(BUILDDIR)/simple_daemon $(BUILDDIR)/multi_instance $(BUILDDIR)/kernel_comm $(BUILDDIR)/meta_cli
TESTS = $(BUILDDIR)/test_memory $(BUILDDIR)/test_threadpool $(BUILDDIR)/test_event $(BUILDDIR)/test_ipc

.PHONY: all clean examples tests install kmod

all: $(LIB_STATIC) $(LIB_SHARED) $(CLIENT_STATIC) $(CLIENT_SHARED) examples tests

examples: $(EXAMPLES)

tests: $(TESTS)

# Create directories
$(BUILDDIR)/core $(BUILDDIR)/ipc $(BUILDDIR)/daemon $(BUILDDIR)/cli $(BUILDDIR)/client/src:
	mkdir -p $@

# Object compilation
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)/core $(BUILDDIR)/ipc $(BUILDDIR)/daemon $(BUILDDIR)/cli
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILDDIR)/client/%.o: $(CLIENTDIR)/%.c | $(BUILDDIR)/client/src
	$(CC) $(CFLAGS) -I$(CLIENTDIR)/include -fPIC -c $< -o $@

$(BUILDDIR)/cint/%.o: $(SRCDIR)/cint/%.c
	mkdir -p $(BUILDDIR)/cint
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
	$(CC) $(CFLAGS) -I$(EXAMPLEDIR)/af_meta/include $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

# Tests
$(BUILDDIR)/test_memory: $(TESTDIR)/test_memory.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/test_threadpool: $(TESTDIR)/test_threadpool.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/test_event: $(TESTDIR)/test_event.c $(LIB_STATIC)
	$(CC) $(CFLAGS) $< -o $@ -L$(BUILDDIR) -lqdaemon $(LDFLAGS)

$(BUILDDIR)/test_ipc: $(TESTDIR)/test_ipc.c $(LIB_STATIC) $(CLIENT_STATIC)
	$(CC) $(CFLAGS) -I$(CLIENTDIR)/include $< -o $@ -L$(BUILDDIR) -lqdaemon -lqdclient $(LDFLAGS)

# Run tests
test: tests
	@for t in $(TESTS); do echo "Running $$t..."; $$t || exit 1; done
	@echo "All tests passed!"

# Kernel module
kmod:
	$(MAKE) -C kernel

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
