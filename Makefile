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
CMAKE_CMD := cmake -S . -B $(BUILD) -DCMAKE_C_COMPILER=$(CC) -G "Unix Makefiles"

ifeq ($(OS),Windows_NT)
  JOBS ?= $(if $(NUMBER_OF_PROCESSORS),$(NUMBER_OF_PROCESSORS),4)
else
  JOBS ?= $(shell nproc 2>/dev/null || echo 4)
endif

.PHONY: all clean configure run test test-one clean-tests format tidy setup

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
	@python3 tests/run_tests.py --resurg=$(TARGET) --cc=$(CC) --rt-objs="$(RT_LIB)" \
		--build=$(BUILD) --runtime=$(RUNTIME) -j$(JOBS)

# Run a single test: make test-one FILE=tests/integration/v0.1.0/primitives.rsg
test-one: all
	@python3 tests/run_tests.py --resurg=$(TARGET) --cc=$(CC) --rt-objs="$(RT_LIB)" \
		--build=$(BUILD) --runtime=$(RUNTIME) -j1 $(FILE)

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
                    $(RUNTIME)/*.c $(RUNTIME)/*.h)
format:
	@clang-format -i --style=file $(ALL_C)
	@echo 'Formatted $(words $(ALL_C)) file(s).'

# Run clang-tidy on all C sources (uses compile_commands.json)
TIDY_SRCS := $(filter %.c,$(ALL_C))

tidy: $(BUILD)/Makefile
	@printf '%s\n' $(TIDY_SRCS) | xargs -P$(JOBS) -n1 clang-tidy --quiet -p $(BUILD)
	@echo clang-tidy passed.
