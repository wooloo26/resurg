MAKEFLAGS += --no-print-directory

CC        := clang
BUILD     := build
RUNTIME   := runtime

ifeq ($(OS),Windows_NT)
  EXE    := .exe
  RT_LIB := $(BUILD)/resurg_runtime.lib
else
  EXE    :=
  RT_LIB := $(BUILD)/libresurg_runtime.a
endif

TARGET    := $(BUILD)/resurg$(EXE)
VSCODE_EXT:= tools/rsg-lsp/editors/vscode
CMAKE_CMD := cmake -S . -B $(BUILD) -DCMAKE_C_COMPILER=$(CC) -G "Unix Makefiles"

ifeq ($(OS),Windows_NT)
  PYTHON := python
else
  PYTHON := python3
endif

ifeq ($(OS),Windows_NT)
  JOBS ?= $(if $(NUMBER_OF_PROCESSORS),$(NUMBER_OF_PROCESSORS),4)
else
  JOBS ?= $(shell nproc 2>/dev/null || echo 4)
endif

.PHONY: all clean configure run test test-one test-std test-lsp vscode-lsp clean-tests format tidy lint setup

# Build everything (auto-configures on first run)
all: $(BUILD)/Makefile
	@cmake --build $(BUILD) --parallel $(JOBS)

# CMake configure (re-run when CMakeLists.txt changes)
$(BUILD)/Makefile: CMakeLists.txt
	@$(CMAKE_CMD)

configure:
	@$(CMAKE_CMD)

clean:
ifeq ($(OS),Windows_NT)
	@if exist $(BUILD) rmdir /s /q $(BUILD)
else
	@rm -rf $(BUILD)
endif

# Set up git hooks for development
setup:
ifeq ($(OS),Windows_NT)
	@if not exist .githooks mkdir .githooks
	@copy pre-commit .githooks\pre-commit >nul
	@git config core.hooksPath .githooks
	@echo Git hooks configured.
else
	@mkdir -p .githooks
	@cp pre-commit .githooks/pre-commit
	@chmod +x .githooks/pre-commit
	@git config core.hooksPath .githooks
	@echo 'Git hooks configured.'
endif

# Run a .rsg file: make run FILE=tests/integration/v0.1.0/primitives.rsg
run: all
	@$(TARGET) $(FILE) -o $(BUILD)/out.c
	@$(CC) -std=c17 -I$(RUNTIME) -o $(BUILD)/out$(EXE) $(BUILD)/out.c $(RT_LIB)
	@$(BUILD)/out$(EXE)

# Run all test cases
test: all
	@$(PYTHON) tests/run_tests.py --resurg=$(TARGET) --cc=$(CC) --rt-objs="$(RT_LIB)" \
		--build=$(BUILD) --runtime=$(RUNTIME) -j$(JOBS)

# Run a single test: make test-one FILE=tests/integration/v0.1.0/primitives.rsg
test-one: all
	@$(PYTHON) tests/run_tests.py --resurg=$(TARGET) --cc=$(CC) --rt-objs="$(RT_LIB)" \
		--build=$(BUILD) --runtime=$(RUNTIME) -j1 $(FILE)

# Run std tests only
test-std: all
	@$(PYTHON) tests/run_tests.py --resurg=$(TARGET) --cc=$(CC) --rt-objs="$(RT_LIB)" \
		--build=$(BUILD) --runtime=$(RUNTIME) -j$(JOBS) \
		tests/integration/stdlib/*.rsg

test-lsp: all
	@$(PYTHON) tests/lsp/test_lsp.py --lsp=$(BUILD)/rsg-lsp$(EXE)

# Build rsg-lsp, compile VSCode extension, and run LSP tests
vscode-lsp: all
	@cd $(VSCODE_EXT) && (pnpm install --frozen-lockfile 2>/dev/null || pnpm install)
	@cd $(VSCODE_EXT) && pnpm run compile
	@$(PYTHON) tests/lsp/test_lsp.py --lsp=$(BUILD)/rsg-lsp$(EXE)
	@echo 'VSCode LSP extension: built and tested.'

# Install the VSCode extension via symlink (dev mode)
install-ext: vscode-lsp
ifeq ($(OS),Windows_NT)
	@if not exist "$(APPDATA)\Code\extensions\resurg.resurg-lsp-1.0.0" ( \
		mklink /D "$(APPDATA)\Code\extensions\resurg.resurg-lsp-1.0.0" "$(CURDIR)\$(VSCODE_EXT)" \
	)
else
	@EXT_DIR="$$HOME/.vscode-server/extensions"; \
	 if [ ! -d "$$EXT_DIR" ]; then EXT_DIR="$$HOME/.vscode/extensions"; fi; \
	 mkdir -p "$$EXT_DIR"; \
	 ln -sfn "$(CURDIR)/$(VSCODE_EXT)" "$$EXT_DIR/resurg.resurg-lsp-1.0.0"
endif
	@echo 'VSCode extension installed (symlink).'

# Clean only test build artifacts (preserves the compiler)
clean-tests:
ifeq ($(OS),Windows_NT)
	@if exist $(BUILD)\tests rmdir /s /q $(BUILD)\tests
else
	@rm -rf $(BUILD)/tests
endif

# Format all C sources with clang-format
ALL_C := $(wildcard src/*.c src/*.h src/*/*.c src/*/*.h src/*/*/*.c src/*/*/*.h \
                    src/*/*/*/*.c src/*/*/*/*.h \
                    include/*.h include/*/*.h include/*/*/*.h \
                    $(RUNTIME)/*.c $(RUNTIME)/*.h \
                    tools/rsg-lsp/*.c tools/rsg-lsp/*.h)
format:
	@clang-format -i --style=file $(ALL_C)
	@echo 'Formatted $(words $(ALL_C)) file(s).'

# Run clang-tidy on all C sources (uses compile_commands.json)
TIDY_SRCS := $(filter %.c,$(ALL_C))

tidy: $(BUILD)/Makefile
ifeq ($(OS),Windows_NT)
	@$(PYTHON) -c "import subprocess,sys,concurrent.futures as F;fs='$(TIDY_SRCS)'.split();run=lambda f:subprocess.call(['clang-tidy','--quiet','-p','$(BUILD)',f]);x=F.ThreadPoolExecutor($(JOBS));errs=sum(1 for r in x.map(run,fs) if r);sys.exit(errs)"
else
	@printf '%s\n' $(TIDY_SRCS) | xargs -P$(JOBS) -n1 clang-tidy --quiet -p $(BUILD)
endif
	@echo clang-tidy passed.

# Unified lint: format + tidy
lint: format tidy
