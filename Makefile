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

.PHONY: all clean configure run test format tidy setup

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
TESTS := $(wildcard tests/integration/**/*.rsg)
TEST_TARGETS := $(patsubst %.rsg,%.test,$(TESTS))

test: all
ifeq ($(OS),Windows_NT)
	@powershell -NoProfile -ExecutionPolicy Bypass -File tests/run_all_tests.ps1 $(TARGET) $(CC) "$(RT_LIB)" $(BUILD) $(RUNTIME) $(TESTS)
else
	@$(MAKE) --no-print-directory -j$(JOBS) $(TEST_TARGETS)
	@echo '$(words $(TESTS)) tests passed.'
endif

# Run a single test: make tests/integration/v0.1.0/primitives.test
$(TEST_TARGETS): %.test: %.rsg all
ifeq ($(OS),Windows_NT)
	@powershell -NoProfile -ExecutionPolicy Bypass -File tests/run_test.ps1 $< $(TARGET) $(CC) "$(RT_LIB)" $(BUILD) $(RUNTIME)
else
	@bash tests/run_test.sh $< $(TARGET) $(CC) "$(RT_LIB)" $(BUILD) $(RUNTIME)
endif
	@echo   PASS  $<

# Format all C sources with clang-format
ALL_C := $(wildcard src/*.c src/*.h src/*/*.c src/*/*.h src/*/*/*.c src/*/*/*.h \
                    include/*.h include/*/*.h include/*/*/*.h \
                    $(RUNTIME)/*.c $(RUNTIME)/*.h)
format:
	@clang-format -i --style=file $(ALL_C)
	@echo 'Formatted $(words $(ALL_C)) file(s).'

# Run clang-tidy on all C sources (uses compile_commands.json)
TIDY_SRCS := $(filter %.c,$(ALL_C))
TIDY_TARGETS := $(patsubst %.c,%.tidy,$(TIDY_SRCS))

$(TIDY_TARGETS): %.tidy: %.c
	@clang-tidy --quiet -p $(BUILD) $<

tidy: $(BUILD)/Makefile
	@"$(MAKE)" --no-print-directory -j$(JOBS) $(TIDY_TARGETS)
	@echo clang-tidy passed.
