.PHONY: test

test:
	@TEST_BIN="$$(mktemp -t paw3222-delta-test.XXXXXX)"; \
	trap 'rm -f "$${TEST_BIN}"' EXIT; \
	cc -std=c11 -Wall -Wextra -Werror tests/paw3222_delta_test.c -o "$${TEST_BIN}"; \
	"$${TEST_BIN}"
