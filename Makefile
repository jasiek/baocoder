# baocoder - AMBE+2 (3600x2450) codec: build, tests, fixtures.
#
#   make            build libbaocoder.a and the ambe_decode / ambe_encode CLIs
#   make test       run the unit and known-good-pair tests
#   make fixtures   regenerate tests/fixtures from upstream sources (needs net)
#   make tables     re-extract the quantiser tables from a firmware image
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -g -std=c99 -Wall -Wextra -Wno-unused-parameter
# glibc hides POSIX 2008 (getline, used by the tests) and the BSD math
# constants when -std=c99 is in force; _DEFAULT_SOURCE asks for the usual set
# back.  macOS exposes both regardless and ignores it.
CPPFLAGS += -Iinclude -Isrc -D_DEFAULT_SOURCE
LDLIBS  += -lm

SRC     := src/golay.c src/ambe_basop.c src/ambe_fft.c src/ambe_encode_params.c src/ambe_fec.c src/ambe_params.c src/ambe_synth.c \
           src/ambe_tables_fw.c src/ambe_decoder.c src/ambe_analysis.c src/ambe_encoder.c src/rc4.c src/aes.c
OBJ     := $(SRC:.c=.o)
LIB     := libbaocoder.a

TESTS   := tests/test_golay tests/test_aes tests/test_basop tests/test_fft tests/test_tables tests/test_fec \
           tests/test_params tests/test_synth tests/test_e2e \
           tests/test_encode tests/test_encode_sweep tests/test_encode_pcm \
           tests/test_encode_voicing

THIRD   := third_party
MBELIB  := $(THIRD)/mbelib
SAMPLES := $(THIRD)/known-key-mbe-samples

.PHONY: all test fixtures tables clean distclean check-fixedpoint

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
	$(MAKE) --no-print-directory check-fixedpoint; \
	echo "all tests passed"

# "Fixed point only" is a claim, so it is checked rather than asserted: the
# library must contain no floating-point instruction and must link with no
# libm.  Both are cheap and catch a float creeping back in - a stray double in
# an intermediate is easy to write and impossible to see in a diff.
check-fixedpoint: $(LIB)
	@bad=$$(nm -u $(LIB) 2>/dev/null | awk '{print $$NF}' | \
	    grep -Ex '_?(cos|sin|tan|exp|exp2|log|log2|log10|sqrt|pow|fmod|floor|ceil|ldexp|frexp|fabs)f?' \
	    | sort -u); \
	if [ -n "$$bad" ]; then echo "libm references in $(LIB):"; echo "$$bad"; exit 1; fi
	@$(CC) $(CFLAGS) $(CPPFLAGS) tools/ambe_decode.c $(LIB) -o .nolibm.tmp || \
	    { echo "$(LIB) does not link without -lm"; rm -f .nolibm.tmp; exit 1; }
	@rm -rf .nolibm.tmp .nolibm.tmp.dSYM
	@if command -v otool >/dev/null 2>&1; then \
	  n=$$(otool -tv $(LIB) 2>/dev/null | grep -cE '^[[:space:]]+[0-9a-f]+[[:space:]]+(f(add|sub|mul|div|cvt|mov|cmp|neg|abs|sqrt|madd|msub)|scvtf|ucvtf|fcvtz)') || true; \
	  if [ "$$n" != "0" ]; then echo "$$n floating-point instructions in $(LIB)"; exit 1; fi; \
	fi
	@echo "ok   $(LIB) is integer-only: no libm, no floating-point instructions"

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

# Re-extract the quantiser tables from the radio image.  The firmware is not
# redistributed here, so point FIRMWARE at your own copy:
#
#   make tables FIRMWARE=/path/to/DM32_L01_048_20250821.bin
#
# The expected image is sha256
# fda860febfcf1a234eed7fa73272112891074aac83746e4f8dfe224a2a700f8f; the
# generated file records it so a mismatch is visible in the diff.
FIRMWARE ?= firmware/DM32_L01_048_20250821.bin
tables:
	@test -f "$(FIRMWARE)" || { \
	  echo "no firmware image at $(FIRMWARE)"; \
	  echo "usage: make tables FIRMWARE=/path/to/DM32_L01_048_20250821.bin"; \
	  exit 1; }
	python3 tools/extract_tables.py $(FIRMWARE) > src/ambe_tables_fw.c.new
	@mv src/ambe_tables_fw.c.new src/ambe_tables_fw.c
	@echo "regenerated src/ambe_tables_fw.c from $(FIRMWARE)"

clean:
	rm -rf .nolibm.tmp .nolibm.tmp.dSYM
	rm -f $(OBJ) $(LIB) ambe_decode ambe_encode tools/mbe_ref tools/dmra_decrypt $(TESTS)

distclean: clean
	rm -rf $(THIRD)
