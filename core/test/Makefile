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
CJSON := ../../managed_components/espressif__cjson/cJSON
REVIEW_DEPS := ../wav_stream.h ../chart_history.h ../input_gate.h ../ha_parse.h ../ha_model.h ../reading_units.h ../energy_model.h ../time_utils.h

test: $(TARGET) test_review
	./$(TARGET)
	./test_review

cjson_test.o: $(CJSON)/cJSON.c
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -I$(CJSON) -c $< -o $@

test_review: test_review.c ../ha_parse.c cjson_test.o $(REVIEW_DEPS) fixtures/nordpool.json fixtures/energy_statistics.json
	$(CC) $(CFLAGS) -I$(CJSON) -o $@ test_review.c ../ha_parse.c cjson_test.o -lm

$(TARGET): $(SRC) ../sample_model.h ../readiness.h ../ota_identity.h ../audio_cmd.h ../comfort_rules.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -rf $(TARGET) $(TARGET).dSYM test_review test_review.dSYM test_history_cache test_history_cache.dSYM cjson_test.o test_dashboard test_dashboard.dSYM

test_history_cache: test_history_cache.c ../history_cache.h ../chart_history.h
	$(CC) $(CFLAGS) -o $@ test_history_cache.c -lm

test: history-cache
history-cache: test_history_cache
	./test_history_cache
.PHONY: history-cache

# Same bounded dashboard projection is built for both boards.
test_dashboard: test_dashboard.c ../dashboard_model.c ../dashboard_model.h ../cost_model.h ../ha_parse.c cjson_test.o
	$(CC) $(CFLAGS) -I.. -I$(CJSON) -o $@ test_dashboard.c ../dashboard_model.c ../ha_parse.c cjson_test.o -lm
test: dashboard
dashboard: test_dashboard
	./test_dashboard
.PHONY: dashboard
