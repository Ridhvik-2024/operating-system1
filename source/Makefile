# AI-OS v1.0 — Makefile (host runtime)
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wshadow -Wformat -Wformat-security \
	   -fstack-protector-strong -D_FORTIFY_SOURCE=2 -D_GNU_SOURCE
LDFLAGS  = -pthread -lm

AR      ?= ar

LIB_SRCS  := src/json.c src/types.c src/aiproc.c src/sembus.c \
	     src/action.c src/policy.c src/mem.c src/tools.c src/sandbox.c \
	     src/stage.c src/snapshot.c src/supervisor.c src/audit.c src/effects.c \
	     src/agent.c \
	     src/cortex.c src/shell.c src/shroute.c src/ksched.c src/ebpf.c src/hardening.c \
	     src/microlm.c src/autopilot.c src/durable.c \
	     src/fb.c src/win.c src/input.c
LIB_OBJS  := $(LIB_SRCS:.c=.o)
LIB       := libaios.a

BIN       := tool-exec aios-demo aios-memctl aiosh aios-governor aios-desktop
TEST_BINS := $(patsubst tests/%.c,build/tests/%,$(wildcard tests/test_*.c))

# M9: install/upgrade path. Works with DESTDIR for staged packaging.
PREFIX    ?= /usr/local
INCDIR    := $(PREFIX)/include/aios
BINDIR    := $(PREFIX)/bin
HDRS      := $(wildcard include/aios/*.h)

.PHONY: all test clean demo install uninstall

all: $(LIB) $(BIN)

install: all
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(INCDIR) $(DESTDIR)$(PREFIX)/lib
	install -m 0755 aiosh aios-demo aios-memctl aios-governor tool-exec \
	        $(DESTDIR)$(BINDIR)/
	install -m 0644 $(HDRS) $(DESTDIR)$(INCDIR)/
	install -m 0644 libaios.a $(DESTDIR)$(PREFIX)/lib/
	@echo "== AI-OS v1.1 installed to $(DESTDIR)$(PREFIX) =="
	@echo "   aiosh           $(BINDIR)/aiosh"
	@echo "   (tool-exec sits next to aiosh; the sandbox finds it via /proc/self/exe)"
	@echo "   headers/lib     $(INCDIR)/, $(PREFIX)/lib/libaios.a"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/aiosh $(DESTDIR)$(BINDIR)/aios-demo \
	      $(DESTDIR)$(BINDIR)/aios-memctl $(DESTDIR)$(BINDIR)/aios-governor \
	      $(DESTDIR)$(BINDIR)/tool-exec
	rm -rf $(DESTDIR)$(INCDIR)
	rm -f $(DESTDIR)$(PREFIX)/lib/libaios.a

-include $(LIB_SRCS:.c=.d) $(wildcard tests/*.d)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

# M10: the 8x16 font table lives in src/font8x16.h and is COMMITTED, like
# src/microlm_weights.h. The generator (tools/gen_font.py) needs Pillow and
# DejaVu fonts; the build must need neither. Regenerate explicitly with:
#     python3 tools/gen_font.py src/font8x16.h

src/%.o: src/%.c
	$(CC) $(CFLAGS) -Iinclude -MMD -c -o $@ $<

%: apps/%.c $(LIB)
	$(CC) $(CFLAGS) -Iinclude -MMD -o $@ $< $(LIB) $(LDFLAGS)

tool-exec: src/tool-exec.c $(LIB)
	$(CC) $(CFLAGS) -Iinclude -MMD -o $@ src/tool-exec.c $(LIB) $(LDFLAGS)

# The adversary worker is a normal build target, not a string a test compiles at
# runtime: the booted image has no compiler, and a fixture that cannot build
# there is a fixture that silently stops testing the target that matters most.
build/liar-worker: tests/fixtures/liar-worker.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $<

build/deaf-worker: tests/fixtures/deaf-worker.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $<

# These two suites drive the real worker and its adversaries. Built here rather
# than only by `make test`, so `make build/tests/test_effects` on its own does not
# produce a suite that fails for a reason it cannot explain.
build/tests/test_effects: build/liar-worker build/deaf-worker tool-exec
build/tests/test_shell:   build/liar-worker tool-exec

build/tests/%: tests/%.c $(LIB) tests/t.h
	@mkdir -p build/tests
	$(CC) $(CFLAGS) -Iinclude -MMD -o $@ $< $(LIB) $(LDFLAGS)

test: $(LIB) $(TEST_BINS) tool-exec build/liar-worker build/deaf-worker
	@pass=0; fail=0; \
	for t in $(TEST_BINS); do \
	  if ./$$t >build/last_test.log 2>&1; then pass=$$((pass+1)); \
	       echo "PASS $$t"; \
	  else fail=$$((fail+1)); echo "FAIL $$t"; cat build/last_test.log; fi; \
	done; \
	echo "== $$pass passed, $$fail failed =="; \
	test $$fail -eq 0

demo: aios-demo tool-exec
	./aios-demo --scripted

clean:
	rm -f $(LIB_OBJS) $(LIB) $(BIN) $(TEST_BINS) *.d src/*.d tests/*.d
	rm -rf build
