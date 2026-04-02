CC      := clang
CFLAGS  := -std=c17 -Wall -Wextra -Wpedantic -Werror -Isrc -D_CRT_SECURE_NO_WARNINGS
LDFLAGS :=

BUILD   := build
SRC     := src
RUNTIME := runtime

# Compiler sources
SRCS := $(wildcard $(SRC)/*.c)
HDRS := $(wildcard $(SRC)/*.h)
OBJS := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(SRCS))

# Runtime sources (linked into compiled programs, not the compiler)
RT_SRCS := $(wildcard $(RUNTIME)/*.c)
RT_OBJS := $(patsubst $(RUNTIME)/%.c,$(BUILD)/rt_%.o,$(RT_SRCS))

TARGET := $(BUILD)/resurg

.PHONY: all clean runtime test format tidy setup

all: $(TARGET) runtime

$(TARGET): $(OBJS) | $(BUILD)
	$(CC) $(LDFLAGS) -o $@ $^

$(BUILD)/%.o: $(SRC)/%.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

runtime: $(RT_OBJS)

$(BUILD)/rt_%.o: $(RUNTIME)/%.c | $(BUILD)
	$(CC) -std=c17 -Wall -Wextra -Wpedantic -Werror -I$(RUNTIME) -c -o $@ $<

$(BUILD):
ifeq ($(OS),Windows_NT)
	@if not exist $(BUILD) mkdir $(BUILD)
else
	@mkdir -p $(BUILD)
endif

clean:
ifeq ($(OS),Windows_NT)
	@if exist $(BUILD) rmdir /s /q $(BUILD)
else
	@rm -rf $(BUILD)
endif

# Set up git hooks for development
setup:
	git config core.hooksPath .githooks
	@echo 'Git hooks configured.'

# Run a .rsg file: make run FILE=tests/v0.1.0/primitives.rsg
run: $(TARGET) runtime
	$(TARGET) $(FILE) -o $(BUILD)/out.c
	$(CC) -std=c17 -I$(RUNTIME) -o $(BUILD)/out $(BUILD)/out.c $(RT_OBJS)
	$(BUILD)/out

# Run all test cases
TESTS := $(wildcard tests/**/*.rsg)
TEST_TARGETS := $(patsubst %.rsg,%.test,$(TESTS))

test: $(TARGET) runtime $(TEST_TARGETS)
	@echo $(words $(TESTS)) tests passed.

$(TEST_TARGETS): %.test: %.rsg $(TARGET) runtime
	@bash tests/run_test.sh $< $(TARGET) $(CC) "$(RT_OBJS)" $(BUILD) $(RUNTIME)
	@echo   PASS  $<

# Format all C sources with clang-format
ALL_C := $(wildcard $(SRC)/*.c $(SRC)/*.h $(RUNTIME)/*.c $(RUNTIME)/*.h)
format:
	clang-format -i --style=file $(ALL_C)
	@echo 'Formatted $(words $(ALL_C)) file(s).'

# Run clang-tidy on all C sources
TIDY_SRCS := $(wildcard $(SRC)/*.c $(RUNTIME)/*.c)
tidy:
	$(foreach f,$(TIDY_SRCS),clang-tidy --quiet $(f) -- $(CFLAGS) &&) echo clang-tidy passed.
