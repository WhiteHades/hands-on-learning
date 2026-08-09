CC ?= gcc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local

CPPFLAGS := -D_POSIX_C_SOURCE=200809L -Iinclude \
	$(shell $(PKG_CONFIG) --cflags ncursesw json-c libcurl libxml-2.0 libarchive)
CFLAGS := -std=c23 -O2 -g -Wall -Wextra -Wpedantic -Werror \
	-Wconversion -Wshadow -Wformat=2 -Wstrict-prototypes \
	-Wmissing-prototypes -fstack-protector-strong -D_FORTIFY_SOURCE=3
LDLIBS := $(shell $(PKG_CONFIG) --libs ncursesw json-c libcurl libxml-2.0 libarchive) -lm

ifeq ($(SANITIZE),1)
CFLAGS := $(filter-out -O2,$(CFLAGS)) -O1 -fno-omit-frame-pointer \
	-fsanitize=address,undefined
LDLIBS += -fsanitize=address,undefined
endif

APP := build/hands-on-learning
TEST_CARTRIDGE := build/test-course.imscc
TEST_STAGE := build/test-cartridge
TEST_CARTRIDGE_SOURCES := $(wildcard tests/fixtures/cartridge/* \
	tests/fixtures/cartridge/web/* tests/fixtures/cartridge/assessments/*)
SOURCES := $(wildcard src/*.c)
CORE_SOURCES := $(filter-out src/main.c,$(SOURCES))
OBJECTS := $(patsubst src/%.c,build/%.o,$(SOURCES))
TEST_SOURCES := $(wildcard tests/test_*.c)
TEST_BINS := $(patsubst tests/%.c,build/%,$(TEST_SOURCES))

.PHONY: all clean test check sanitize install

all: $(APP)

$(APP): $(OBJECTS)
	$(CC) $(CFLAGS) $^ $(LDLIBS) -o $@

build/%.o: src/%.c include/hol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/test_%: tests/test_%.c $(CORE_SOURCES) include/hol.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(CORE_SOURCES) $(LDLIBS) -o $@

build:
	mkdir -p $@

$(TEST_CARTRIDGE): $(TEST_CARTRIDGE_SOURCES) | build
	rm -rf $(TEST_STAGE) $@
	mkdir -p $(TEST_STAGE)
	cp -R tests/fixtures/cartridge/. $(TEST_STAGE)/
	find $(TEST_STAGE) -type f -exec touch -t 202608090000 {} +
	cd $(TEST_STAGE) && find imsmanifest.xml LICENSE web assessments -type f \
		-print | LC_ALL=C sort | zip -X -q $(abspath $@) -@
	rm -rf $(TEST_STAGE)

test: $(TEST_BINS) $(APP) $(TEST_CARTRIDGE)
	@set -e; for test_bin in $(TEST_BINS); do "$$test_bin"; done

check: all test

sanitize:
	$(MAKE) clean
	$(MAKE) SANITIZE=1 check

install: $(APP)
	install -Dm755 $(APP) $(DESTDIR)$(PREFIX)/bin/hands-on-learning
	install -d $(DESTDIR)$(PREFIX)/share/hands-on-learning/courses
	install -Dm644 courses/catalog.json \
		$(DESTDIR)$(PREFIX)/share/hands-on-learning/courses/catalog.json

clean:
	rm -rf build
