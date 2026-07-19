CC ?= gcc
CFLAGS += -Wall -Wextra -O2 -DNDEBUG

SQLITE_VERSION ?= 3450300
SQLITE_YEAR ?= 2024
SQLITE_SRC = https://sqlite.org/$(SQLITE_YEAR)/sqlite-src-$(SQLITE_VERSION).zip
SQLITE_AUTO = https://sqlite.org/$(SQLITE_YEAR)/sqlite-autoconf-$(SQLITE_VERSION).tar.gz
SRC = sqlite-src-$(SQLITE_VERSION)
AUTO = sqlite-autoconf-$(SQLITE_VERSION)

FTS5 = vendor/ext/fts5
TOOL = vendor/tool
TARGET = fts5x.so

$(TARGET): fts5x.c $(FTS5)/fts5parse.c
	$(CC) -shared -fPIC $(CFLAGS) -Ivendor -I$(FTS5) -o $@ $<

$(TOOL)/lemon: $(TOOL)/lemon.c
	$(CC) -o $@ $<

$(FTS5)/fts5parse.c: $(FTS5)/fts5parse.y $(TOOL)/lemon $(TOOL)/lempar.c
	$(TOOL)/lemon -T$(TOOL)/lempar.c $<

vendor:
	@mkdir -p vendor/ext/misc
	curl -fsSL "$(SQLITE_SRC)" | \
		bsdtar -xf - -C vendor --strip-components=1 \
			"$(SRC)/ext/fts5/*.c" \
			"$(SRC)/ext/fts5/*.h" \
			"$(SRC)/ext/fts5/fts5parse.y" \
			"$(SRC)/tool/lemon.c" \
			"$(SRC)/tool/lempar.c"
	curl -fsSL "$(SQLITE_AUTO)" | \
		bsdtar -xf - -C vendor --strip-components=1 \
			"$(AUTO)/sqlite3.h" \
			"$(AUTO)/sqlite3ext.h"
	patch -p1 < patches/fts5x.patch

clean:
	rm -f $(TARGET) $(TOOL)/lemon $(FTS5)/fts5parse.c $(FTS5)/fts5parse.h $(FTS5)/fts5parse.out

distclean: clean
	rm -rf vendor

install: $(TARGET)
	install -m 644 $(TARGET) /usr/local/lib/

.PHONY: clean distclean install vendor
