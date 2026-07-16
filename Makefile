# jsvm — native test build + wasm32 freestanding build.
#
# make test        build and run unit tests (plain + ASan/UBSan)
# make wasm        build build/jsvm.wasm — needs a clang with a wasm backend
#                  and wasm-ld. Auto-detected: system clang, the LLVM inside
#                  an emsdk install (found via emcc on PATH), or Homebrew
#                  LLVM. Override with WASM_CC=...
#
#                  Note: emcc itself can't drive this build — the core is
#                  freestanding (-nostdlib) and emcc force-exports its
#                  runtime's stack symbols, which don't exist here. The
#                  clang/wasm-ld bundled with emsdk is what gets used.
# make clean

CC ?= cc

WASM_CC ?= $(shell \
  if clang --print-targets 2>/dev/null | grep -q wasm32; then echo clang; \
  elif command -v emcc >/dev/null 2>&1 && [ -x "$$(dirname "$$(command -v emcc)")/../bin/clang" ]; then \
    echo "$$(dirname "$$(command -v emcc)")/../bin/clang"; \
  elif [ -x /opt/homebrew/opt/llvm/bin/clang ]; then echo /opt/homebrew/opt/llvm/bin/clang; \
  else echo clang; fi)

WARNINGS = -std=c11 -Wall -Wextra -Werror -Wshadow -Wvla
CFLAGS  ?= -O2 -g

SRC := src/js_vm.c src/js_gc.c src/js_string.c src/js_map.c src/js_object.c \
       src/js_arena.c src/js_lexer.c src/js_parser.c src/js_number.c \
       src/js_compiler.c src/js_interp.c
HDR := include/jsvm.h src/jsvm_internal.h src/js_syntax.h src/js_bytecode.h
INC := -Iinclude -Isrc

ASAN := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

build/test_runner: $(SRC) test/test_main.c $(HDR)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(SRC) test/test_main.c -o $@

build/test_runner_asan: $(SRC) test/test_main.c $(HDR)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(SRC) test/test_main.c -o $@

build/test_syntax: $(SRC) test/test_syntax.c $(HDR)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(SRC) test/test_syntax.c -o $@

build/test_syntax_asan: $(SRC) test/test_syntax.c $(HDR)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(SRC) test/test_syntax.c -o $@

build/test_exec: $(SRC) test/test_exec.c $(HDR)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(SRC) test/test_exec.c -o $@

build/test_exec_asan: $(SRC) test/test_exec.c $(HDR)
	@mkdir -p build
	$(CC) $(WARNINGS) $(ASAN) $(INC) $(SRC) test/test_exec.c -o $@

# the jsvm CLI: compile + run a .js file
.PHONY: cli
cli: build/jsvm
build/jsvm: $(SRC) tools/jsvm_cli.c $(HDR)
	@mkdir -p build
	$(CC) $(WARNINGS) $(CFLAGS) $(INC) $(SRC) tools/jsvm_cli.c -o $@

.PHONY: test
test: build/test_runner build/test_runner_asan build/test_syntax build/test_syntax_asan \
      build/test_exec build/test_exec_asan
	./build/test_runner
	./build/test_runner_asan
	./build/test_syntax
	./build/test_syntax_asan
	./build/test_exec
	./build/test_exec_asan

# Freestanding wasm32: no libc, no OS. The core never calls libc; the shim
# provides the mem* symbols the compiler itself may emit. -fno-builtin keeps
# the shim's own implementations from being folded into recursive calls.
.PHONY: wasm
wasm: build/jsvm.wasm
build/jsvm.wasm: $(SRC) src/js_wasm_shim.c $(HDR)
	@mkdir -p build
	$(WASM_CC) --target=wasm32-unknown-unknown $(WARNINGS) -O2 \
	  -ffreestanding -fno-builtin -nostdlib -DJSVM_FREESTANDING \
	  $(INC) -Wl,--no-entry -Wl,--export-all -Wl,-z,stack-size=1048576 \
	  $(SRC) src/js_wasm_shim.c -o $@

.PHONY: clean
clean:
	rm -rf build
