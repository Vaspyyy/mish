CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
BIN_OUTPUT ?= bin
BIN_NAME ?= mish
SOURCES := $(wildcard src/*.c)
OBJECTS := $(patsubst src/%.c,build/%.o,$(SOURCES))
DEPENDENCIES := $(OBJECTS:.o=.d)

.PHONY: all compile run sequence clean test check sanitize loc
all: compile
compile: $(BIN_OUTPUT)/$(BIN_NAME)

$(BIN_OUTPUT)/$(BIN_NAME): $(OBJECTS)
	mkdir -p "$(BIN_OUTPUT)"
	$(CC) $(OBJECTS) $(LDFLAGS) -o "$@"

build/%.o: src/%.c src/mish.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c "$<" -o "$@"

run: compile
	"$(BIN_OUTPUT)/$(BIN_NAME)" $(INVOC_ARGS)
sequence: run

check:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O2 -g -std=c11 -Wall -Wextra -Wpedantic -Werror'
	python3 tests/run.py "$(BIN_OUTPUT)/$(BIN_NAME)"

test: compile
	python3 tests/run.py "$(BIN_OUTPUT)/$(BIN_NAME)"

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'
	CC='$(CC)' MISH_TEST_SANITIZE=1 ASAN_OPTIONS=detect_leaks=1 python3 tests/run.py "$(BIN_OUTPUT)/$(BIN_NAME)"

loc:
	wc -l src/*.c src/*.h

clean:
	rm -rf build "$(BIN_OUTPUT)/$(BIN_NAME)"

-include $(DEPENDENCIES)
