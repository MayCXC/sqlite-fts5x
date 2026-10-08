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

# Each check prints "ok" or fails the target through an integer overflow. An
# auxiliary function cannot be called inside an aggregate, so each check scores
# the rows in a MATERIALIZED CTE and aggregates over it. bm25w is compared with
# bm25 within 1e-12: the two may round differently where the compiler fuses a
# multiply and an add.
test: $(TARGET)
	@echo "=== bm25w (each check fails the target) ==="
	sqlite3 -bail :memory: ".load ./fts5x sqlite3_fts5x_init" \
	  "CREATE VIRTUAL TABLE d USING fts5x(body);" \
	  "INSERT INTO d(rowid, body) VALUES (1, 'cat cat dog'), (2, 'cat bird bird bird'), (3, 'dog dog dog dog dog'), (4, 'bird fish'), (5, 'fish fish fish cat'), (6, 'horse'), (7, 'horse horse cow'), (8, 'cow');" \
	  "WITH s AS MATERIALIZED (SELECT bm25(d) AS b, bm25w(d) AS w, bm25w(d, 1.0, 1.0) AS w1 FROM d WHERE d MATCH 'cat OR dog') SELECT 'unit weights give bm25: ' || CASE WHEN count(*) = 4 AND max(abs(w - b)) < 1e-12 AND max(abs(w1 - b)) < 1e-12 THEN 'ok' ELSE abs(-9223372036854775808) END FROM s;" \
	  "WITH w AS MATERIALIZED (SELECT rowid AS id, bm25w(d, 2.0, 0.5) AS w FROM d WHERE d MATCH 'cat OR dog'), c AS MATERIALIZED (SELECT rowid AS id, bm25(d) AS c FROM d WHERE d MATCH 'cat'), g AS MATERIALIZED (SELECT rowid AS id, bm25(d) AS g FROM d WHERE d MATCH 'dog') SELECT 'each weight scales its phrase''s term: ' || CASE WHEN count(*) = 4 AND max(abs(w.w - (2.0 * coalesce(c.c, 0) + 0.5 * coalesce(g.g, 0)))) < 1e-12 THEN 'ok' ELSE abs(-9223372036854775808) END FROM w LEFT JOIN c ON c.id = w.id LEFT JOIN g ON g.id = w.id;" \
	  "WITH s AS MATERIALIZED (SELECT bm25w(d, 2.0, 0.5, 9.0) AS x, bm25w(d, 2.0, 0.5) AS y FROM d WHERE d MATCH 'cat OR dog') SELECT 'a weight past the last phrase is unused: ' || CASE WHEN count(*) = 4 AND max(abs(x - y)) = 0 THEN 'ok' ELSE abs(-9223372036854775808) END FROM s;" \
	  "WITH u AS MATERIALIZED (SELECT rowid FROM d WHERE d MATCH 'cat OR dog' ORDER BY bm25(d), rowid), v AS MATERIALIZED (SELECT rowid FROM d WHERE d MATCH 'cat OR dog' ORDER BY bm25w(d, 2.0, 0.5), rowid) SELECT 'the weights reorder this fixture: ' || CASE WHEN (SELECT group_concat(rowid) FROM u) <> (SELECT group_concat(rowid) FROM v) THEN 'ok' ELSE abs(-9223372036854775808) END;" \
	  "WITH r AS MATERIALIZED (SELECT rowid, rank FROM d WHERE d MATCH 'cat OR dog' AND rank MATCH 'bm25w(2.0, 0.5)' ORDER BY rank, rowid), s AS MATERIALIZED (SELECT rowid, bm25w(d, 2.0, 0.5) AS s FROM d WHERE d MATCH 'cat OR dog' ORDER BY s, rowid) SELECT 'rank MATCH ranks by it: ' || CASE WHEN (SELECT group_concat(rowid || ':' || printf('%.12f', rank)) FROM r) = (SELECT group_concat(rowid || ':' || printf('%.12f', s)) FROM s) THEN 'ok' ELSE abs(-9223372036854775808) END;"

.PHONY: clean distclean install test vendor
