CC ?= gcc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local

CPPFLAGS := -D_POSIX_C_SOURCE=200809L -Iinclude \
	$(shell $(PKG_CONFIG) --cflags ncursesw json-c libcurl)
CFLAGS := -std=c23 -O2 -g -Wall -Wextra -Wpedantic -Werror \
	-Wconversion -Wshadow -Wformat=2 -Wstrict-prototypes \
	-Wmissing-prototypes -fstack-protector-strong -D_FORTIFY_SOURCE=3
LDLIBS := $(shell $(PKG_CONFIG) --libs ncursesw json-c libcurl) -lm

ifeq ($(SANITIZE),1)
CFLAGS := $(filter-out -O2,$(CFLAGS)) -O1 -fno-omit-frame-pointer \
	-fsanitize=address,undefined
LDLIBS += -fsanitize=address,undefined
endif

APP := build/hands-on-learning
SOURCES := $(filter-out src/state.c,$(wildcard src/*.c))
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

test: $(TEST_BINS)
	@set -e; for test_bin in $(TEST_BINS); do "$$test_bin"; done

check: all test

sanitize:
	$(MAKE) clean
	$(MAKE) SANITIZE=1 check

install: $(APP)
	install -Dm755 $(APP) $(DESTDIR)$(PREFIX)/bin/hands-on-learning
	install -d $(DESTDIR)$(PREFIX)/share/hands-on-learning/courses
	cp -R courses/demo.holcourse $(DESTDIR)$(PREFIX)/share/hands-on-learning/courses/
	install -Dm644 courses/catalog.json \
		$(DESTDIR)$(PREFIX)/share/hands-on-learning/courses/catalog.json

clean:
	rm -rf build
