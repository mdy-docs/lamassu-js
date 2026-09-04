# lamassu — frontend/runtime split build: native, WASI, and the npm package.
#
# make test               build and run unit tests (plain + ASan/UBSan)
# make bench              build the CLI and run bench/*.js, reporting ops/sec
# make pkg                build the npm package's wasm artifact (needs emcc);
#                         this is what the docs site and its playground import
# make wasi               build the CLI as a wasm32-wasip2 component (needs wasi-sdk)
# make test-wasi          run the unit tests under wasmtime
# make bench-wasi         run bench/*.js under wasmtime
# make check-runtime-only prove the runtime links with no frontend
# make reactor           build the fleet reactor (runtime only, wasip1 core module)
# make reactor-check     prove no parser/compiler symbol reached the reactor
# make server            build the HTTP server for wasip2 (sockets + --stdio)
# make server-native     the same sources as a native binary
# make server-wasip1     wasip1 build, --stdio only (preview1 has no sockets)
# make server-check      prove no parser/compiler symbol reached the server
# make clean
#
# ---------------------------------------------------------------------------
# TWO HALVES.
#
# FRONTEND is the lexer, parser, compiler, and the bytecode WRITER: everything
# that turns text into code. RUNTIME is everything else — the interpreter, GC,
# builtins, promises, the module pipeline, and the bytecode READER/VERIFIER.
#
# The dependency runs one way, frontend -> runtime, and nothing points back.
# That is enforced, not documented: `make check-runtime-only` links the runtime
# archive alone against a bytecode-only main, so any new call from a runtime
# file into the frontend fails the build with an undefined symbol.
#
# The payoff is a process that runs precompiled bytecode and cannot compile
# source, because the code that would do it is not in the binary. See the
# header comment in include/lamassu_compile.h.
# ---------------------------------------------------------------------------

CC ?= cc
AR ?= ar

# -D_POSIX_C_SOURCE: -std=c11 makes glibc hide POSIX declarations (strdup
# in the tests, gettimeofday in js_date.c); macOS exposes them regardless.
WARNINGS = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wshadow -Wvla
CFLAGS  ?= -O2 -g
# The math kernel is freestanding, but a few builtins (fmod, sqrt/hypot)
# use compiler-native ops that gcc lowers to libm calls on Linux; macOS
# bundles libm into libSystem so this is a no-op there.
LIBS = -lm

RUNTIME_SRC := src/runtime/js_vm.c src/runtime/js_gc.c src/runtime/js_string.c \
               src/runtime/js_map.c src/runtime/js_object.c src/runtime/js_number.c \
               src/runtime/js_interp.c src/runtime/js_mathkernel.c \
               src/runtime/js_builtins.c src/runtime/js_promise.c \
               src/runtime/js_case.c \
               src/runtime/js_module.c src/runtime/js_regexp.c \
               src/runtime/js_bytecode_read.c src/runtime/js_date.c \
               src/runtime/js_mapobj.c src/runtime/js_setobj.c \
               src/runtime/js_error.c

# js_arena.c is here and not in the runtime because the arena's only users are
# the parser and the two compile entry points — the runtime never allocates one.
FRONTEND_SRC := src/frontend/js_lexer.c src/frontend/js_parser.c \
                src/frontend/js_compiler.c src/frontend/js_arena.c \
                src/frontend/js_bytecode_write.c src/frontend/js_compile_api.c \
                src/frontend/js_module_compile.c

HDR := include/lamassu.h include/lamassu_compile.h src/lamassu_internal.h \
       src/js_bytecode.h src/js_bcformat.h src/js_valindex.h \
       src/frontend/js_syntax.h src/runtime/js_regexp.h src/runtime/js_date.h \
       src/runtime/js_mapobj.h src/runtime/js_setobj.h src/runtime/js_error.h
INC := -Iinclude -Isrc -Isrc/runtime -Isrc/frontend

ASAN := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

# Regex engine (third_party/baru-re), enabled via -DLAMASSU_HAS_REGEX in every
# build. Compiled as separate objects because re_vm.c contains three VLAs the
# engine's own build permits — the engine keeps every other strict flag,
# lamassu code keeps -Wvla.
RE_INC  := -Ithird_party/baru-re/include
RE_WARN := -std=c11 -Wall -Wextra -Werror -Wshadow
RE_SRC  := third_party/baru-re/src/re_lexer.c third_party/baru-re/src/re_parser.c \
           third_party/baru-re/src/re_compiler.c third_party/baru-re/src/re_vm.c
RE_HDR  := third_party/baru-re/include/regexp.h third_party/baru-re/include/ucd.h
REGEX_FLAGS := -DLAMASSU_HAS_REGEX $(RE_INC)

# ---- per-flavour object trees -------------------------------------------
#
# Three flavours (native / ASan / WASI) share one set of pattern rules by
# keeping objects in build/<flavour>/, mirroring the source layout. The regex
# engine's objects live under the same tree with their own warning set.

RT_OBJ    := $(patsubst src/%.c,build/native/%.o,$(RUNTIME_SRC))
FE_OBJ    := $(patsubst src/%.c,build/native/%.o,$(FRONTEND_SRC))
RE_OBJ    := $(patsubst third_party/baru-re/src/%.c,build/native/re/%.o,$(RE_SRC))
RT_OBJ_A  := $(patsubst src/%.c,build/asan/%.o,$(RUNTIME_SRC))
FE_OBJ_A  := $(patsubst src/%.c,build/asan/%.o,$(FRONTEND_SRC))
RE_OBJ_A  := $(patsubst third_party/baru-re/src/%.c,build/asan/re/%.o,$(RE_SRC))

build/native/re/%.o: third_party/baru-re/src/%.c $(RE_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(RE_WARN) $(CFLAGS) $(RE_INC) -c $< -o $@
build/asan/re/%.o: third_party/baru-re/src/%.c $(RE_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(RE_WARN) $(ASAN) $(RE_INC) -c $< -o $@
build/native/%.o: src/%.c $(HDR)
	@mkdir -p $(dir $@)
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) -c $< -o $@
build/asan/%.o: src/%.c $(HDR)
	@mkdir -p $(dir $@)
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(REGEX_FLAGS) -c $< -o $@

# ---- the two archives ----------------------------------------------------
#
# Link order is always frontend-then-runtime: the frontend calls into the
# runtime to build function cells, intern strings and compile regex literals,
# and nothing goes the other way.

RUNTIME_LIB := build/liblamassu_runtime.a
FRONTEND_LIB := build/liblamassu_frontend.a
RUNTIME_LIB_A := build/liblamassu_runtime_asan.a
FRONTEND_LIB_A := build/liblamassu_frontend_asan.a

$(RUNTIME_LIB): $(RT_OBJ) $(RE_OBJ)
	@mkdir -p build
	@rm -f $@
	$(AR) rcs $@ $^
$(FRONTEND_LIB): $(FE_OBJ)
	@mkdir -p build
	@rm -f $@
	$(AR) rcs $@ $^
$(RUNTIME_LIB_A): $(RT_OBJ_A) $(RE_OBJ_A)
	@mkdir -p build
	@rm -f $@
	$(AR) rcs $@ $^
$(FRONTEND_LIB_A): $(FE_OBJ_A)
	@mkdir -p build
	@rm -f $@
	$(AR) rcs $@ $^

.PHONY: libs
libs: $(RUNTIME_LIB) $(FRONTEND_LIB)

# ---- tests ---------------------------------------------------------------
#
# Every test/*.c is a standalone main. They exercise the compiler, so they link
# both halves; check-runtime-only below is what covers the other direction.

TESTS := $(patsubst test/%.c,build/%,$(wildcard test/*.c))
TESTS_ASAN := $(patsubst test/%.c,build/%_asan,$(wildcard test/*.c))

build/%: test/%.c $(FRONTEND_LIB) $(RUNTIME_LIB)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) $< \
	  $(FRONTEND_LIB) $(RUNTIME_LIB) -o $@ $(LIBS)
build/%_asan: test/%.c $(FRONTEND_LIB_A) $(RUNTIME_LIB_A)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(REGEX_FLAGS) $< \
	  $(FRONTEND_LIB_A) $(RUNTIME_LIB_A) -o $@ $(LIBS)

.PHONY: test
test: $(TESTS) $(TESTS_ASAN)
	@for t in $(TESTS) $(TESTS_ASAN); do echo "-- $$t"; ./$$t || exit 1; done

# ---- the lamassu CLI: compile + run a .js file ---------------------------
.PHONY: cli
cli: build/lamassu
build/lamassu: tools/lamassu.c $(FRONTEND_LIB) $(RUNTIME_LIB)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) tools/lamassu.c \
	  $(FRONTEND_LIB) $(RUNTIME_LIB) -o $@ $(LIBS)

# ---- the guard -----------------------------------------------------------
#
# tools/run_bc.c includes <lamassu.h> only and runs a .jsbc file. Linking it
# against the runtime archive ALONE is the proof that the split holds: if a
# runtime source ever calls the parser or compiler again, this link fails with
# an undefined symbol and CI goes red. It is also the skeleton of a fleet
# binary — this is what one looks like.
.PHONY: check-runtime-only
check-runtime-only: build/lamassu-runtime-only
	@echo "ok: the runtime links with no frontend"
build/lamassu-runtime-only: tools/run_bc.c $(RUNTIME_LIB)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) -Iinclude $< $(RUNTIME_LIB) -o $@ $(LIBS)

# Benchmarks (bench/*.js): -O2 release build, no ASan overhead, so numbers
# reflect real interpreter performance. Each script self-times with Date.now()
# and prints one "name: N iters in Mms (ops/sec)" line; see bench/_util.js.
.PHONY: bench
bench: build/lamassu
	@for f in bench/*.js; do \
	  case "$$f" in */_util.js) continue ;; esac; \
	  ./build/lamassu "$$f" || exit 1; \
	done

# ---- WASI: the same C, built for wasm32-wasip2, run under wasmtime --------
#
# Not a port. The core's entire OS surface is gettimeofday (js_date.c), which
# wasi-libc maps to clock_time_get; everything else is stdio and malloc from
# tools/lamassu.c. No #ifdef, no shim, no source shared with wasm_api.c — that
# file is the *emscripten* embedding, and its Asyncify __hostcall has no place
# here (a wasmtime host call is a plain synchronous import).
#
# wasip2, not preview1: wasi-sdk emits a component for it, which is the format
# wasmtime runs natively. A preview1 core module would buy back Node's WASI
# host, which this CLI has no use for.
#
# The 8MB stack matches the emscripten build's -sSTACK_SIZE below. wasi-sdk
# defaults to 64KB and js_parser.c permits 256 levels of recursive descent, so
# the default is not close to enough. A reactor build that only ever RUNS
# bytecode never enters the parser and can take far less.
#
# The toolchain lives BESIDE the checkout (../wasi-sdk-33.0), the convention
# the sibling nisaba-db repo's build scripts use: one download serves every
# worktree, and a `git clean -xdf` cannot cost anyone a 600MB fetch.
# The pins live in tools/toolchain.sh, which the fetch scripts source. Read
# rather than repeated: a Makefile and a fetch script that disagree about which
# toolchain they mean is exactly the failure pinning exists to prevent. The
# search paths below must stay in step with that file's *_candidates.
WASI_SDK_VERSION := $(shell sed -n 's/^WASI_SDK_VERSION=//p' tools/toolchain.sh)
WASMTIME_VERSION := $(shell sed -n 's/^WASMTIME_VERSION=//p' tools/toolchain.sh)
WASI_SDK ?= $(firstword $(wildcard $(dir $(CURDIR))wasi-sdk-$(WASI_SDK_VERSION) /opt/wasi-sdk))
WASI_CC   := $(WASI_SDK)/bin/wasm32-wasip2-clang
# Falls through to whatever is on PATH when no pinned copy is installed.
WASMTIME  ?= $(firstword $(wildcard $(dir $(CURDIR))wasmtime-$(WASMTIME_VERSION)/wasmtime \
                                    /opt/wasmtime/wasmtime) wasmtime)
# -O2 without -g: DWARF in a wasm component multiplies the artifact size and
# wasmtime does nothing with it here.
WASI_CFLAGS  := -O2
WASI_LDFLAGS := -Wl,-z,stack-size=8388608

# Fail with the reason rather than "no such file or directory" from a $(WASI_CC)
# that expanded to /bin/wasm32-wasip2-clang.
.PHONY: wasi-sdk-check
wasi-sdk-check:
	@if [ -z "$(WASI_SDK)" ]; then \
	  echo "error: no wasi-sdk $(WASI_SDK_VERSION) found; looked in:" >&2; \
	  echo "    $(dir $(CURDIR))wasi-sdk-$(WASI_SDK_VERSION)" >&2; \
	  echo "    /opt/wasi-sdk" >&2; \
	  echo "  fetch it: ./tools/get-wasi-sdk.sh" >&2; \
	  echo "  or:       make wasi WASI_SDK=/path/to/wasi-sdk" >&2; \
	  exit 1; \
	elif [ ! -x "$(WASI_CC)" ]; then \
	  echo "error: $(WASI_SDK) is not a wasi-sdk ($(WASI_CC) is missing)" >&2; \
	  exit 1; \
	fi

RT_OBJ_W := $(patsubst src/%.c,build/wasi/%.o,$(RUNTIME_SRC))
FE_OBJ_W := $(patsubst src/%.c,build/wasi/%.o,$(FRONTEND_SRC))
RE_OBJ_W := $(patsubst third_party/baru-re/src/%.c,build/wasi/re/%.o,$(RE_SRC))
RUNTIME_LIB_W  := build/liblamassu_runtime_wasi.a
FRONTEND_LIB_W := build/liblamassu_frontend_wasi.a

build/wasi/re/%.o: third_party/baru-re/src/%.c $(RE_HDR)
	@mkdir -p $(dir $@)
	$(WASI_CC) $(RE_WARN) $(WASI_CFLAGS) $(RE_INC) -c $< -o $@
build/wasi/%.o: src/%.c $(HDR)
	@mkdir -p $(dir $@)
	$(WASI_CC) $(WARNINGS) $(WASI_CFLAGS) $(INC) $(REGEX_FLAGS) -c $< -o $@

$(RUNTIME_LIB_W): $(RT_OBJ_W) $(RE_OBJ_W)
	@mkdir -p build
	@rm -f $@
	$(WASI_SDK)/bin/llvm-ar rcs $@ $^
$(FRONTEND_LIB_W): $(FE_OBJ_W)
	@mkdir -p build
	@rm -f $@
	$(WASI_SDK)/bin/llvm-ar rcs $@ $^

.PHONY: wasi
wasi: wasi-sdk-check build/lamassu-wasip2.wasm
build/lamassu-wasip2.wasm: tools/lamassu.c $(FRONTEND_LIB_W) $(RUNTIME_LIB_W)
	@mkdir -p build
	$(WASI_CC) $(WARNINGS) $(WASI_CFLAGS) $(INC) $(REGEX_FLAGS) $(WASI_LDFLAGS) \
	  tools/lamassu.c $(FRONTEND_LIB_W) $(RUNTIME_LIB_W) -o $@ $(LIBS)

WASI_TESTS := $(patsubst test/%.c,build/wasi_%.wasm,$(wildcard test/*.c))

build/wasi_%.wasm: test/%.c $(FRONTEND_LIB_W) $(RUNTIME_LIB_W)
	@mkdir -p build
	$(WASI_CC) $(WARNINGS) $(WASI_CFLAGS) $(INC) $(REGEX_FLAGS) $(WASI_LDFLAGS) \
	  $< $(FRONTEND_LIB_W) $(RUNTIME_LIB_W) -o $@ $(LIBS)

# WASI has no ambient filesystem: without --dir every fopen fails, so the tests
# and the CLI both get the tree they actually read as their one preopen.
.PHONY: test-wasi
test-wasi: wasi-sdk-check $(WASI_TESTS)
	@for t in $(WASI_TESTS); do \
	  echo "-- $$t"; \
	  $(WASMTIME) run --dir . $$t || exit 1; \
	done

.PHONY: bench-wasi
bench-wasi: wasi-sdk-check build/lamassu-wasip2.wasm
	@for f in bench/*.js; do \
	  case "$$f" in */_util.js) continue ;; esac; \
	  $(WASMTIME) run --dir . build/lamassu-wasip2.wasm "$$f" || exit 1; \
	done

# ---- the fleet reactor: precompiled bytecode in, output out ---------------
#
# src/reactor.c against liblamassu_runtime ALONE — no lexer, no parser, no
# compiler in the binary. See that file's header for the ABI and the intended
# Wizer cut point; `make reactor-check` proves the parser is really absent.
#
# WASIP1, AND DELIBERATELY NOT WASIP2. wasi-sdk emits a *component* for wasip2,
# and a component does not expose plain C exports — lifting them needs a WIT
# world and wit-bindgen, which buys typed interfaces at the cost of a heavier
# instantiation path and tooling this repo does not have. A core module exports
# lam_* directly, instantiates leaner, and is what wasmtime's pooling allocator
# and copy-on-write memory are built around. The CLI stays on wasip2 (it wants
# WASI's filesystem, not a custom ABI); revisit this one only if you want typed
# interfaces more than you want instantiation cost.
#
# The reactor never parses, so it takes a 1MB stack rather than the CLI's 8MB —
# the 8MB exists for js_parser.c's 256 levels of recursive descent, which is
# code this binary does not contain. In a pooling allocator, per-slot memory is
# what caps density, so this is not a cosmetic difference.
REACTOR_CFLAGS  := -O2
# -mexec-model=reactor is a LINK option (no main, exports _initialize instead
# of _start); the driver rejects it on a -c compile line.
REACTOR_LDFLAGS := -mexec-model=reactor -Wl,-z,stack-size=1048576
WASI_P1_CC := $(WASI_SDK)/bin/wasm32-wasip1-clang

RT_OBJ_W1 := $(patsubst src/%.c,build/wasip1/%.o,$(RUNTIME_SRC))
RE_OBJ_W1 := $(patsubst third_party/baru-re/src/%.c,build/wasip1/re/%.o,$(RE_SRC))
RUNTIME_LIB_W1 := build/liblamassu_runtime_wasip1.a

build/wasip1/re/%.o: third_party/baru-re/src/%.c $(RE_HDR)
	@mkdir -p $(dir $@)
	$(WASI_P1_CC) $(RE_WARN) $(REACTOR_CFLAGS) $(RE_INC) -c $< -o $@
build/wasip1/%.o: src/%.c $(HDR)
	@mkdir -p $(dir $@)
	$(WASI_P1_CC) $(WARNINGS) $(REACTOR_CFLAGS) $(INC) $(REGEX_FLAGS) -c $< -o $@

$(RUNTIME_LIB_W1): $(RT_OBJ_W1) $(RE_OBJ_W1)
	@mkdir -p build
	@rm -f $@
	$(WASI_SDK)/bin/llvm-ar rcs $@ $^

.PHONY: reactor
reactor: wasi-sdk-check build/lamassu-reactor.wasm
build/lamassu-reactor.wasm: src/reactor.c src/utf8.h include/lamassu.h $(RUNTIME_LIB_W1)
	@mkdir -p build
	$(WASI_P1_CC) $(WARNINGS) $(REACTOR_CFLAGS) -Iinclude -Isrc $(REACTOR_LDFLAGS) \
	  src/reactor.c $(RUNTIME_LIB_W1) -o $@ $(LIBS)

# The claim, checked rather than asserted: no frontend symbol survives into the
# reactor, and its WASI import surface is small enough to read in one screen.
.PHONY: reactor-check
reactor-check: build/lamassu-reactor.wasm
	@echo "-- exports:"; $(WASI_SDK)/bin/llvm-nm --defined-only $< 2>/dev/null | grep -o 'lam_[a-z_]*' | sort -u | sed 's/^/     /'
	@echo "-- WASI imports:"; $(WASI_SDK)/bin/llvm-nm --undefined-only $< 2>/dev/null | awk '{print $$NF}' | sort -u | sed 's/^/     /'
	@if $(WASI_SDK)/bin/llvm-nm $< 2>/dev/null | grep -qE 'js_parse_module|js_compile_ast|js_compile_module_body|js_lex_'; then \
	  echo "FAIL: a frontend symbol reached the reactor" >&2; exit 1; \
	fi
	@echo "ok: no parser/compiler symbol in the reactor"

# ---- the server: HTTP in, rendered page out ------------------------------
#
# tools/server.c against liblamassu_runtime ALONE, built for three targets from
# one main() — the claim this rests on being that the engine is C and the host
# is a detail. wasip2 is the deployment target: it is the only wasm target with
# sockets (preview1 has no socket() at all, not a missing right), so the wasip1
# build serves --stdio and says so if asked for a port.
#
# Directly after the sibling nisaba-db repo's wasm/build-server.sh, including
# the reason for building all three: a target that stops compiling is a boundary
# that has moved.
SERVER_CFLAGS := -O2

.PHONY: server server-wasip1 server-native
server: wasi-sdk-check build/lamassu-server-wasip2.wasm
server-wasip1: wasi-sdk-check build/lamassu-server-wasip1.wasm
server-native: build/lamassu-server

# wasip2 — sockets + --stdio. Runs under `wasmtime run -S inherit-network`.
build/lamassu-server-wasip2.wasm: tools/server.c src/utf8.h include/lamassu.h $(RUNTIME_LIB_W)
	@mkdir -p build
	$(WASI_CC) $(WARNINGS) $(SERVER_CFLAGS) -DLAMASSU_SOCKETS=1 -Iinclude -Isrc \
	  $(WASI_LDFLAGS) tools/server.c $(RUNTIME_LIB_W) -o $@ $(LIBS)

# wasip1 — --stdio only. Exists to prove the transport does not depend on
# sockets, and because Node's WASI host can drive it in a test.
build/lamassu-server-wasip1.wasm: tools/server.c src/utf8.h include/lamassu.h $(RUNTIME_LIB_W1)
	@mkdir -p build
	$(WASI_P1_CC) $(WARNINGS) $(SERVER_CFLAGS) -Iinclude -Isrc \
	  $(WASI_LDFLAGS) tools/server.c $(RUNTIME_LIB_W1) -o $@ $(LIBS)

# native — same sources, wherever a cc runs.
build/lamassu-server: tools/server.c src/utf8.h include/lamassu.h $(RUNTIME_LIB)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) -DLAMASSU_SOCKETS=1 -Iinclude -Isrc \
	  tools/server.c $(RUNTIME_LIB) -o $@ $(LIBS)

# Same claim as reactor-check, against the server: no parser reached it.
.PHONY: server-check
server-check: build/lamassu-server-wasip2.wasm
	@if $(WASI_SDK)/bin/llvm-nm $< 2>/dev/null | grep -qE 'js_parse_module|js_compile_ast|js_compile_module_body'; then \
	  echo "FAIL: a frontend symbol reached the server" >&2; exit 1; \
	fi
	@echo "ok: no parser/compiler symbol in the server"

# npm package artifact: the engine's REPL surface compiled to an ES module
# (packages/lamassu-js/dist/lamassu.mjs + lamassu.wasm), the wasm+shim the npm
# package publishes and the Vite playground imports. Needs emcc on PATH
# (Emscripten SDK); emscripten supplies libc (malloc/free). EXPORT_ES6 + the
# .mjs extension emit a real ES module whose default export is the
# `createLamassuModule` factory; it locates the sibling .wasm via
# import.meta.url (a bundler like Vite can also be handed an explicit wasm
# URL — see the package wrapper).
#
# This one links BOTH halves: it evaluates source the browser hands it, which
# is exactly the capability a fleet runtime drops.
EMCC ?= emcc
PKG_DIR := packages/lamassu-js
PKG_DIST := $(PKG_DIR)/dist

.PHONY: pkg
pkg: $(PKG_DIST)/lamassu.mjs
$(PKG_DIST)/lamassu.mjs: $(RUNTIME_SRC) $(FRONTEND_SRC) src/wasm_api.c $(HDR) $(RE_SRC) $(RE_HDR)
	@mkdir -p $(PKG_DIST)
	$(EMCC) -O2 $(INC) $(REGEX_FLAGS) $(RUNTIME_SRC) $(FRONTEND_SRC) $(RE_SRC) src/wasm_api.c \
	  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createLamassuModule \
	  -sENVIRONMENT=web,node \
	  -sSTACK_SIZE=8388608 -sSTACK_OVERFLOW_CHECK=2 \
	  -sEXPORTED_FUNCTIONS=_lamassu_eval,_lamassu_eval_module,_lamassu_reset,_lamassu_settle_deferred,_lamassu_set_limits,_malloc,_free \
	  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8 \
	  -sASYNCIFY -sASYNCIFY_STACK_SIZE=262144 \
	  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=16777216 \
	  -o $(PKG_DIST)/lamassu.mjs

.PHONY: clean
clean:
	rm -rf build $(PKG_DIST)
