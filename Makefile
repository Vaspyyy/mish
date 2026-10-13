CARGO ?= cargo
BIN_OUTPUT ?= bin
INVOC_ARGS ?=

.PHONY: compile run clean sequence test check

compile:
	$(CARGO) build --release
	mkdir -p "$(BIN_OUTPUT)"
	cp target/release/mish "$(BIN_OUTPUT)/mish"

run: compile
	"$(BIN_OUTPUT)/mish" $(INVOC_ARGS)

test:
	$(CARGO) test

check:
	$(CARGO) fmt --check
	$(CARGO) clippy --all-targets -- -D warnings

clean:
	$(CARGO) clean
	rm -f "$(BIN_OUTPUT)/mish"

sequence: run
