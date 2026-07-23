# lamassu — native test build + WebAssembly package build.
#
# make test        build and run unit tests (plain + ASan/UBSan)
# make bench       build the CLI and run bench/*.js, reporting ops/sec
# make pkg         build the npm package's wasm artifact (needs emcc)
# make clean

CC ?= cc

WARNINGS = -std=c11 -Wall -Wextra -Werror -Wshadow -Wvla
CFLAGS  ?= -O2 -g

SRC := src/js_vm.c src/js_gc.c src/js_string.c src/js_map.c src/js_object.c \
       src/js_arena.c src/js_lexer.c src/js_parser.c src/js_number.c \
       src/js_compiler.c src/js_interp.c src/js_mathkernel.c src/js_builtins.c \
       src/js_promise.c src/js_module.c src/js_regexp.c src/js_serialize.c \
       src/js_date.c src/js_mapobj.c src/js_setobj.c
HDR := include/lamassu.h src/lamassu_internal.h src/js_syntax.h src/js_bytecode.h \
       src/js_regexp.h src/js_date.h src/js_mapobj.h src/js_setobj.h
INC := -Iinclude -Isrc

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
RE_OBJ      := build/re_lexer.o build/re_parser.o build/re_compiler.o build/re_vm.o
RE_OBJ_ASAN := build/re_lexer_asan.o build/re_parser_asan.o build/re_compiler_asan.o \
               build/re_vm_asan.o
REGEX_FLAGS := -DLAMASSU_HAS_REGEX $(RE_INC)

build/re_%_asan.o: third_party/baru-re/src/re_%.c $(RE_HDR)
	@mkdir -p build
	$(CC) $(RE_WARN) $(ASAN) $(RE_INC) -c $< -o $@

build/re_%.o: third_party/baru-re/src/re_%.c $(RE_HDR)
	@mkdir -p build
	$(CC) $(RE_WARN) $(CFLAGS) $(RE_INC) -c $< -o $@

build/test_runner: $(SRC) test/test_main.c $(HDR) $(RE_OBJ)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ) test/test_main.c -o $@

build/test_runner_asan: $(SRC) test/test_main.c $(HDR) $(RE_OBJ_ASAN)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ_ASAN) test/test_main.c -o $@

build/test_syntax: $(SRC) test/test_syntax.c $(HDR) $(RE_OBJ)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ) test/test_syntax.c -o $@

build/test_syntax_asan: $(SRC) test/test_syntax.c $(HDR) $(RE_OBJ_ASAN)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ_ASAN) test/test_syntax.c -o $@

build/test_exec: $(SRC) test/test_exec.c $(HDR) $(RE_OBJ)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ) test/test_exec.c -o $@

build/test_exec_asan: $(SRC) test/test_exec.c $(HDR) $(RE_OBJ_ASAN)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ_ASAN) test/test_exec.c -o $@

build/test_builtins: $(SRC) test/test_builtins.c $(HDR) $(RE_OBJ)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ) test/test_builtins.c -o $@

build/test_builtins_asan: $(SRC) test/test_builtins.c $(HDR) $(RE_OBJ_ASAN)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ_ASAN) test/test_builtins.c -o $@

build/test_async: $(SRC) test/test_async.c $(HDR) $(RE_OBJ)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ) test/test_async.c -o $@

build/test_async_asan: $(SRC) test/test_async.c $(HDR) $(RE_OBJ_ASAN)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ_ASAN) test/test_async.c -o $@

build/test_modules: $(SRC) test/test_modules.c $(HDR) $(RE_OBJ)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ) test/test_modules.c -o $@

build/test_modules_asan: $(SRC) test/test_modules.c $(HDR) $(RE_OBJ_ASAN)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ_ASAN) test/test_modules.c -o $@

build/test_repl: $(SRC) test/test_repl.c $(HDR) $(RE_OBJ)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ) test/test_repl.c -o $@

build/test_repl_asan: $(SRC) test/test_repl.c $(HDR) $(RE_OBJ_ASAN)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ_ASAN) test/test_repl.c -o $@

build/test_regex: $(SRC) test/test_regex.c $(HDR) $(RE_OBJ)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ) test/test_regex.c -o $@

build/test_regex_asan: $(SRC) test/test_regex.c $(HDR) $(RE_OBJ_ASAN)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ_ASAN) test/test_regex.c -o $@

build/test_bytecode: $(SRC) test/test_bytecode.c $(HDR) $(RE_OBJ)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ) test/test_bytecode.c -o $@

build/test_bytecode_asan: $(SRC) test/test_bytecode.c $(HDR) $(RE_OBJ_ASAN)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ_ASAN) test/test_bytecode.c -o $@

build/test_dynamic_import: $(SRC) test/test_dynamic_import.c $(HDR) $(RE_OBJ)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ) test/test_dynamic_import.c -o $@

build/test_dynamic_import_asan: $(SRC) test/test_dynamic_import.c $(HDR) $(RE_OBJ_ASAN)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ_ASAN) test/test_dynamic_import.c -o $@

build/test_module_bc: $(SRC) test/test_module_bc.c $(HDR) $(RE_OBJ)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ) test/test_module_bc.c -o $@

build/test_module_bc_asan: $(SRC) test/test_module_bc.c $(HDR) $(RE_OBJ_ASAN)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ_ASAN) test/test_module_bc.c -o $@

# the lamassu CLI: compile + run a .js file
.PHONY: cli
cli: build/lamassu
build/lamassu: $(SRC) tools/lamassu.c $(HDR) $(RE_OBJ)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(REGEX_FLAGS) $(SRC) $(RE_OBJ) tools/lamassu.c -o $@

.PHONY: test
test: build/test_runner build/test_runner_asan build/test_syntax build/test_syntax_asan \
      build/test_exec build/test_exec_asan build/test_builtins build/test_builtins_asan \
      build/test_async build/test_async_asan build/test_modules build/test_modules_asan \
      build/test_repl build/test_repl_asan build/test_regex build/test_regex_asan \
      build/test_bytecode build/test_bytecode_asan \
      build/test_module_bc build/test_module_bc_asan \
      build/test_dynamic_import build/test_dynamic_import_asan
	./build/test_runner
	./build/test_runner_asan
	./build/test_syntax
	./build/test_syntax_asan
	./build/test_exec
	./build/test_exec_asan
	./build/test_builtins
	./build/test_builtins_asan
	./build/test_async
	./build/test_async_asan
	./build/test_modules
	./build/test_modules_asan
	./build/test_repl
	./build/test_repl_asan
	./build/test_regex
	./build/test_regex_asan
	./build/test_bytecode
	./build/test_bytecode_asan
	./build/test_module_bc
	./build/test_module_bc_asan
	./build/test_dynamic_import
	./build/test_dynamic_import_asan

# Benchmarks (bench/*.js): -O2 release build, no ASan overhead, so numbers
# reflect real interpreter performance. Each script self-times with Date.now()
# and prints one "name: N iters in Mms (ops/sec)" line; see bench/_util.js.
# Gates the "opt" roadmap phase in docs/plan.md (shapes + inline caches for
# property access) — that work is deferred until these numbers say it matters.
.PHONY: bench
bench: build/lamassu
	@for f in bench/*.js; do \
	  case "$$f" in */_util.js) continue ;; esac; \
	  ./build/lamassu "$$f" || exit 1; \
	done

# npm package artifact: the engine's REPL surface compiled to an ES module
# (packages/lamassu-js/dist/lamassu.mjs + lamassu.wasm), the wasm+shim the npm
# package publishes and the Vite playground imports. Needs emcc on PATH
# (Emscripten SDK); emscripten supplies libc (malloc/free). EXPORT_ES6 + the
# .mjs extension emit a real ES module whose default export is the
# `createLamassuModule` factory; it locates the sibling .wasm via
# import.meta.url (a bundler like Vite can also be handed an explicit wasm
# URL — see the package wrapper).
EMCC ?= emcc
PKG_DIR := packages/lamassu-js
PKG_DIST := $(PKG_DIR)/dist

.PHONY: pkg
pkg: $(PKG_DIST)/lamassu.mjs
$(PKG_DIST)/lamassu.mjs: $(SRC) src/wasm_api.c $(HDR) $(RE_SRC) $(RE_HDR)
	@mkdir -p $(PKG_DIST)
	$(EMCC) -O2 $(INC) $(REGEX_FLAGS) $(SRC) $(RE_SRC) src/wasm_api.c \
	  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createLamassuModule \
	  -sENVIRONMENT=web,node \
	  -sSTACK_SIZE=8388608 -sSTACK_OVERFLOW_CHECK=2 \
	  -sEXPORTED_FUNCTIONS=_lamassu_eval,_lamassu_eval_module,_lamassu_reset,_lamassu_settle_deferred,_malloc,_free \
	  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8 \
	  -sASYNCIFY -sASYNCIFY_STACK_SIZE=262144 \
	  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=16777216 \
	  -o $(PKG_DIST)/lamassu.mjs

# Back-compat alias: `make web` now means "build the package artifact".
.PHONY: web
web: pkg

.PHONY: clean
clean:
	rm -rf build $(PKG_DIST)
