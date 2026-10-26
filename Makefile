# Local LLM Gateway - build and verification targets (GNU make, Linux/glibc).
#
#   make            build ./llm-client and ./llm-server
#   make test       unit tests + offline integration tests (mock backend)
#   make sanitize   ASan + UBSan build, unit + integration tests
#   make tsan       ThreadSanitizer build, concurrency-focused tests
#   make valgrind   Memcheck leak checks for client and server sessions
#   make clean      remove known build outputs only

ifeq ($(origin CC),default)
CC := gcc
endif
PYTHON ?= python3

STD := -std=gnu99
WARN := -Wall -Wextra -pedantic
OPT ?= -O2 -g
SAN ?=
BUILD ?= build/release
BINDIR ?= .

CPPFLAGS := -Iinclude -D_GNU_SOURCE -MMD -MP
ALL_CFLAGS = $(STD) $(WARN) $(OPT) $(SAN) $(EXTRA_CFLAGS) -pthread
ALL_LDFLAGS = $(SAN) -pthread $(LDFLAGS)

COMMON := buf netio http numparse
SERVER_ONLY := server config backend json history stats workers
CLIENT_OBJS := $(addprefix $(BUILD)/,client_main.o client.o $(COMMON:=.o))
SERVER_OBJS := $(addprefix $(BUILD)/,server_main.o $(SERVER_ONLY:=.o) $(COMMON:=.o))
LIB_OBJS := $(addprefix $(BUILD)/,client.o $(SERVER_ONLY:=.o) $(COMMON:=.o))
UNIT_SRCS := $(wildcard tests/unit/*.c)
UNIT_OBJS := $(patsubst tests/unit/%.c,$(BUILD)/unit/%.o,$(UNIT_SRCS))
UNIT_BIN := $(BUILD)/unit_tests
FAULT_LIB := $(BUILD)/libfaultinject.so
WRAP := -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc

ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
TSAN_FLAGS := -fsanitize=thread

.PHONY: all bins unit test unit-test integration-test sanitize tsan valgrind clean
.DEFAULT_GOAL := all

all: $(BINDIR)/llm-client $(BINDIR)/llm-server

$(BINDIR)/llm-client: $(CLIENT_OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(ALL_LDFLAGS)

$(BINDIR)/llm-server: $(SERVER_OBJS)
	@mkdir -p $(BINDIR)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(ALL_LDFLAGS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -c $< -o $@

$(BUILD)/unit/%.o: tests/unit/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -Itests/unit $(ALL_CFLAGS) -c $< -o $@

$(UNIT_BIN): $(UNIT_OBJS) $(LIB_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(ALL_LDFLAGS) $(WRAP)

$(FAULT_LIB): tests/faultinject.c
	@mkdir -p $(dir $@)
	$(CC) $(STD) $(WARN) -O2 -g -D_GNU_SOURCE -fPIC -shared -o $@ $< -ldl -pthread

unit: $(UNIT_BIN)

unit-test: $(UNIT_BIN)
	$(UNIT_BIN)

integration-test: all $(FAULT_LIB)
	$(PYTHON) tests/run_tests.py --bin-dir $(BINDIR) --fault-lib $(FAULT_LIB) $(TEST_ARGS)

test: all unit-test integration-test

sanitize:
	$(MAKE) BUILD=build/asan BINDIR=build/asan OPT="-O1 -g" SAN="$(ASAN_FLAGS)" all unit
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1 \
		build/asan/unit_tests
	$(MAKE) $(FAULT_LIB)
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
		$(PYTHON) tests/run_tests.py --bin-dir build/asan --sanitizer asan $(TEST_ARGS)

tsan:
	$(MAKE) BUILD=build/tsan BINDIR=build/tsan OPT="-O1 -g" SAN="$(TSAN_FLAGS)" all
	TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" \
		$(PYTHON) tests/run_tests.py --bin-dir build/tsan --sanitizer tsan $(TEST_ARGS)

valgrind: all
	$(PYTHON) tests/run_tests.py --bin-dir $(BINDIR) --valgrind $(TEST_ARGS)

clean:
	rm -rf build/release build/asan build/tsan
	rm -f llm-client llm-server
	rm -rf tests/integration/__pycache__
	-rmdir build 2>/dev/null || true

-include $(CLIENT_OBJS:.o=.d) $(SERVER_OBJS:.o=.d) $(UNIT_OBJS:.o=.d)
