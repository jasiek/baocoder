# AMBE+2 (3600x2450) decoder - build, tests, fixtures.
#
#   make            build libambe.a and the ambe_decode CLI
#   make test       run the unit and known-good-pair tests
#   make fixtures   regenerate tests/fixtures from upstream sources (needs net)
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -g -std=c99 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -Iinclude -Isrc
LDLIBS  += -lm

SRC     := src/golay.c src/ambe_encode_params.c src/ambe_fec.c src/ambe_params.c src/ambe_synth.c \
           src/ambe_tables_fw.c src/ambe_decoder.c src/ambe_analysis.c src/ambe_encoder.c src/rc4.c src/aes.c
OBJ     := $(SRC:.c=.o)
LIB     := libambe.a

TESTS   := tests/test_golay tests/test_aes tests/test_tables tests/test_fec \
           tests/test_params tests/test_synth tests/test_e2e \
           tests/test_encode tests/test_encode_sweep tests/test_encode_pcm

THIRD   := third_party
MBELIB  := $(THIRD)/mbelib
SAMPLES := $(THIRD)/known-key-mbe-samples

.PHONY: all test fixtures tables clean distclean

all: $(LIB) ambe_decode ambe_encode

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

ambe_decode: tools/ambe_decode.c $(LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) $(LDLIBS) -o $@

ambe_encode: tools/ambe_encode.c $(LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) $(LDLIBS) -o $@

tools/dmra_decrypt: tools/dmra_decrypt.c $(LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) $(LDLIBS) -o $@

tests/%: tests/%.c $(LIB) tests/testutil.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -Itests $< $(LIB) $(LDLIBS) -o $@

test: $(TESTS)
	@fail=0; for t in $(TESTS); do \
	  printf '%-24s ' "$$(basename $$t)"; \
	  if ./$$t; then :; else fail=1; fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo "FAILURES"; exit 1; fi; \
	echo "all tests passed"

# ---------------------------------------------------------------- fixtures
#
# Regenerating the committed fixtures needs two upstream checkouts and a build
# of mbelib, which acts as the independent reference decoder.  Neither is
# vendored: both are fetched into third_party/ (gitignored).

$(MBELIB)/build/libmbe.a:
	mkdir -p $(THIRD)
	test -d $(MBELIB) || git clone --depth 1 https://github.com/szechyjs/mbelib.git $(MBELIB)
	cmake -S $(MBELIB) -B $(MBELIB)/build -DCMAKE_BUILD_TYPE=Release >/dev/null
	cmake --build $(MBELIB)/build -j4 >/dev/null

$(SAMPLES)/README.md:
	mkdir -p $(THIRD)
	test -d $(SAMPLES) || git clone --depth 1 \
	    https://github.com/tylerwatt12/known-key-mbe-samples.git $(SAMPLES)

tools/mbe_ref: tools/mbe_ref.c $(MBELIB)/build/libmbe.a $(LIB)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I$(MBELIB) $< $(LIB) $(MBELIB)/build/libmbe.a $(LDLIBS) -o $@

fixtures: tools/mbe_ref tools/dmra_decrypt $(SAMPLES)/README.md
	python3 tools/make_fixtures.py $(SAMPLES) tests/fixtures

# Re-extract the quantiser tables from the radio image.  Needs the firmware,
# which is not tracked in this repository.
FIRMWARE ?= ../firmware/DM32_L01_048_20250821.bin
tables:
	python3 tools/extract_tables.py $(FIRMWARE) > src/ambe_tables_fw.c

clean:
	rm -f $(OBJ) $(LIB) ambe_decode ambe_encode tools/mbe_ref tools/dmra_decrypt $(TESTS)

distclean: clean
	rm -rf $(THIRD)
