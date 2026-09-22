.PHONY: test

test:
	@set -eu; \
	DELTA_BIN="$$(mktemp -t paw3222-delta-test.XXXXXX)"; \
	ACCEL_BIN="$$(mktemp -t paw3222-accel-test.XXXXXX)"; \
	trap 'rm -f "$${DELTA_BIN}" "$${ACCEL_BIN}"' EXIT; \
	cc -std=c11 -Wall -Wextra -Werror tests/paw3222_delta_test.c -o "$${DELTA_BIN}"; \
	"$${DELTA_BIN}"; \
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
		-Iinclude tests/paw3222_pointer_acceleration_test.c \
		src/paw3222_pointer_acceleration.c -o "$${ACCEL_BIN}"; \
	UBSAN_OPTIONS=halt_on_error=1 "$${ACCEL_BIN}"
