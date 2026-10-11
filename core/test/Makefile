# core/test/Makefile — host build of the pure policy tests (no board, no IDF).
#
# The review's order-8 developer gate: run these in seconds every change.
# -Wall -Wextra + ASan/UBSan per 07-TESTING-STRATEGY (ASan is a HOST technique
# here, never promised on firmware).
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g \
           -fsanitize=address,undefined -fno-omit-frame-pointer
TARGET  := test_core
SRC     := test_core.c

.PHONY: test clean
test: $(TARGET)
	./$(TARGET)

$(TARGET): $(SRC) ../sample_model.h ../readiness.h ../ota_identity.h ../audio_cmd.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -rf $(TARGET) $(TARGET).dSYM
