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
# multiply and an add. fts5tokenize's rows are compared, in order, with the
# terms the index records for the same text and with tokenize(). A refusal is
# checked against its working twin, the same statement with the refused value
# changed: the twin's output is the whole output only when the twin ran and the
# refused statement stopped the CLI before it printed.
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
	@echo "=== fts5tokenize (each check fails the target) ==="
	sqlite3 -bail :memory: ".load ./fts5x sqlite3_fts5x_init" \
	  "CREATE VIRTUAL TABLE d USING fts5x(body);" \
	  "INSERT INTO d(rowid, body) VALUES (1, 'Le Café au LAIT, café-au-lait! 42 Ünïcode words; words & WORDS.');" \
	  "CREATE VIRTUAL TABLE v USING fts5vocab(d, instance);" \
	  "CREATE VIRTUAL TABLE t USING fts5tokenize;" \
	  "SELECT 'its rows are the terms the index records: ' || CASE WHEN (SELECT group_concat(term, ' ' ORDER BY offset) FROM v WHERE doc = 1) = (SELECT group_concat(token, ' ' ORDER BY position) FROM t WHERE input = (SELECT body FROM d WHERE rowid = 1)) THEN 'ok' ELSE abs(-9223372036854775808) END;" \
	  "SELECT 'they are tokenize()''s tokens: ' || CASE WHEN (SELECT group_concat(token, ' ' ORDER BY position) FROM t WHERE input = (SELECT body FROM d WHERE rowid = 1)) = (SELECT tokenize(body) FROM d WHERE rowid = 1) THEN 'ok' ELSE abs(-9223372036854775808) END;" \
	  "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 100000) SELECT 'a 100,000-token text is 100,000 rows: ' || CASE WHEN (SELECT count(*) FROM t WHERE input = (SELECT group_concat('w' || i, ' ') FROM n)) = 100000 THEN 'ok' ELSE abs(-9223372036854775808) END;"
	@echo "=== fts5rm3 (each check fails the target) ==="
	sqlite3 -bail :memory: ".load ./fts5x sqlite3_fts5x_init" \
	  "CREATE VIRTUAL TABLE d USING fts5x(body);" \
	  "INSERT INTO d(rowid, body) VALUES (1, 'the alpha beta'), (2, 'the gamma'), (3, 'the delta'), (4, 'the epsilon'), (5, 'the zeta'), (6, 'eta theta'), (7, 'iota'), (8, 'kappa'), (9, 'lambda mu'), (10, 'nu xi'), (11, 'omicron'), (12, 'pi'), (13, 'rho'), (14, 'sigma'), (15, 'tau'), (16, 'upsilon'), (17, 'phi'), (18, 'chi'), (19, 'psi'), (20, 'omega cat');" \
	  "CREATE VIRTUAL TABLE temp.voc USING fts5vocab(main, d, row);" \
	  "CREATE VIRTUAL TABLE temp.tok USING fts5tokenize;" \
	  "CREATE VIRTUAL TABLE temp.r USING fts5rm3(d);" \
	  "CREATE TEMP TABLE fb(text TEXT, query TEXT);" \
	  "INSERT INTO fb VALUES ('Alpha alpha ALPHA beta beta gamma delta epsilon zeta eta theta iota kappa lambda mu the the the the x 12345678901234567890123 日本 cat-dog', 'cat fish cat'), ('the the the', ''), ('', 'fish');" \
	  "WITH query AS (SELECT fb.rowid AS id, t.token AS term, CAST(count(*) AS REAL) / sum(count(*)) OVER (PARTITION BY fb.rowid) AS w FROM fb, temp.tok t WHERE t.input = fb.query GROUP BY fb.rowid, t.token), held AS (SELECT fb.rowid AS id, t.token AS term, count(*) AS n FROM fb, temp.tok t WHERE t.input = fb.text AND length(t.token) BETWEEN 2 AND 20 AND NOT t.token GLOB '*[^a-z0-9]*' GROUP BY fb.rowid, t.token), kept AS (SELECT * FROM (SELECT h.*, row_number() OVER (PARTITION BY h.id ORDER BY h.n DESC, h.term) AS i FROM held h WHERE NOT EXISTS (SELECT 1 FROM temp.voc v WHERE v.term = h.term AND v.doc > (SELECT count(*) FROM d_docsize) * 0.1)) WHERE i <= 10), feedback AS (SELECT id, term, CAST(n AS REAL) / sum(n) OVER (PARTITION BY id) AS w FROM kept), model AS (SELECT id, term, sum(w) AS w FROM (SELECT id, term, 0.5 * w AS w FROM query UNION ALL SELECT id, term, 0.5 * w AS w FROM feedback) GROUP BY id, term), s AS (SELECT fb.rowid AS id, (SELECT group_concat('\"' || term || '\"', ' OR ' ORDER BY term) FROM model m WHERE m.id = fb.rowid) AS m, (SELECT 'bm25w(' || group_concat(printf('%.6f', w), ', ' ORDER BY term) || ')' FROM model m WHERE m.id = fb.rowid) AS k, (SELECT count(*) FROM feedback f WHERE f.id = fb.rowid) AS n FROM fb) SELECT 'its model is the RM3 statement''s, row by row: ' || CASE WHEN count(*) = 3 AND sum(coalesce(s.m, '') || '|' || coalesce(s.k, '') || '|' || s.n IS coalesce(r.match, '') || '|' || coalesce(r.rank, '') || '|' || r.terms) = 3 THEN 'ok' ELSE abs(-9223372036854775808) END FROM fb JOIN s ON s.id = fb.rowid, temp.r r WHERE r.feedback = fb.text AND r.query = fb.query;" \
	  "SELECT 'a term in more than a tenth of the rows is dropped, the heaviest ten are kept: ' || CASE WHEN match NOT LIKE '%\"the\"%' AND match LIKE '%\"alpha\"%' AND terms = 10 THEN 'ok' ELSE abs(-9223372036854775808) END FROM temp.r WHERE feedback = (SELECT text FROM fb WHERE rowid = 1) AND query = '';" \
	  "SELECT 'expand, common, shortest, longest and mix are honoured: ' || CASE WHEN (SELECT terms FROM temp.r WHERE feedback = (SELECT text FROM fb WHERE rowid = 1) AND query = '' AND expand = 2) = 2 AND (SELECT match FROM temp.r WHERE feedback = 'the the the' AND query = '' AND common = 0.5) = '\"the\"' AND (SELECT match FROM temp.r WHERE feedback = 'xy abcd' AND query = '' AND shortest = 3 AND longest = 4) = '\"abcd\"' AND (SELECT rank FROM temp.r WHERE feedback = 'cat' AND query = 'fish' AND mix = 0.25) = 'bm25w(0.250000, 0.250000)' THEN 'ok' ELSE abs(-9223372036854775808) END;" \
	  "SELECT 'no feedback term and no query token give NULLs: ' || CASE WHEN match IS NULL AND rank IS NULL AND terms = 0 THEN 'ok' ELSE abs(-9223372036854775808) END FROM temp.r WHERE feedback = 'the the' AND query = '';" \
	  "SELECT 'a row given as relevant is the model of its text, row by row: ' || CASE WHEN count(*) = 20 AND sum(a.match IS b.match AND a.rank IS b.rank AND a.terms = b.terms) = 20 THEN 'ok' ELSE abs(-9223372036854775808) END FROM d, temp.r a, temp.r b WHERE a.relevant = d.rowid AND a.query = 'cat' AND b.feedback = d.body AND b.query = 'cat';" \
	  "CREATE TABLE src(id INTEGER PRIMARY KEY, body TEXT, note TEXT);" \
	  "INSERT INTO src SELECT rowid, body, 'unindexed' || rowid FROM d;" \
	  "CREATE VIEW srcv(rowid, body, note) AS SELECT id, group_concat(body, ' '), group_concat(note, ' ') FROM src GROUP BY id;" \
	  "CREATE VIRTUAL TABLE e USING fts5x(body, note UNINDEXED, content='srcv', content_rowid='rowid');" \
	  "INSERT INTO e(e) VALUES ('rebuild');" \
	  "CREATE VIRTUAL TABLE temp.re USING fts5rm3(e);" \
	  "SELECT 'an external content row is read through its content table, indexed columns only: ' || CASE WHEN count(*) = 20 AND sum(a.match IS b.match AND a.rank IS b.rank AND a.terms = b.terms) = 20 THEN 'ok' ELSE abs(-9223372036854775808) END FROM src s, temp.re a, temp.re b WHERE a.relevant = s.id AND a.query = 'cat' AND b.feedback = s.body AND b.query = 'cat';" \
	  "WITH x(v) AS (SELECT NULL) SELECT 'a NULL relevant is no document, so the model is the query''s: ' || CASE WHEN a.match IS b.match AND a.rank IS b.rank AND a.terms = 0 AND a.match IS NOT NULL THEN 'ok' ELSE abs(-9223372036854775808) END FROM x, temp.r a, temp.r b WHERE a.relevant = x.v AND a.query = 'cat fish cat' AND b.feedback = '' AND b.query = 'cat fish cat';"
	@echo "=== fts5rm3 refuses what is not an fts5 table ==="
	! sqlite3 -bail :memory: ".load ./fts5x sqlite3_fts5x_init" "CREATE TABLE d(body);" "CREATE VIRTUAL TABLE temp.r USING fts5rm3(d);" "SELECT match FROM temp.r WHERE feedback = 'x';" 2>/dev/null
	@echo "=== fts5rm3 refuses a relevant row it cannot read (each prints its working twin, then stops) ==="
	test "$$(sqlite3 -bail :memory: ".load ./fts5x sqlite3_fts5x_init" "CREATE VIRTUAL TABLE d USING fts5x(body);" "INSERT INTO d(rowid, body) VALUES (1, 'alpha');" "CREATE VIRTUAL TABLE temp.r USING fts5rm3(d);" "SELECT 'twin' FROM temp.r WHERE relevant = 1 AND query = '';" "SELECT 'reached' FROM temp.r WHERE relevant = 2 AND query = '';" 2>/dev/null)" = twin
	test "$$(sqlite3 -bail :memory: ".load ./fts5x sqlite3_fts5x_init" "CREATE VIRTUAL TABLE d USING fts5x(body);" "INSERT INTO d(rowid, body) VALUES (1, 'alpha');" "CREATE VIRTUAL TABLE temp.r USING fts5rm3(d);" "SELECT 'twin' FROM temp.r WHERE relevant = 1 AND query = '';" "SELECT 'reached' FROM temp.r WHERE relevant = 'one' AND query = '';" 2>/dev/null)" = twin
	test "$$(sqlite3 -bail :memory: ".load ./fts5x sqlite3_fts5x_init" "CREATE VIRTUAL TABLE d USING fts5x(body);" "INSERT INTO d(rowid, body) VALUES (1, 'alpha');" "CREATE VIRTUAL TABLE temp.r USING fts5rm3(d);" "SELECT 'twin' FROM temp.r WHERE relevant = 1 AND query = '';" "SELECT 'reached' FROM temp.r WHERE relevant = 1 AND feedback = 'alpha' AND query = '';" 2>/dev/null)" = twin
	test "$$(sqlite3 -bail :memory: ".load ./fts5x sqlite3_fts5x_init" "CREATE VIRTUAL TABLE d USING fts5x(body, content='');" "INSERT INTO d(rowid, body) VALUES (1, 'alpha');" "CREATE VIRTUAL TABLE temp.r USING fts5rm3(d);" "SELECT 'twin' FROM temp.r WHERE feedback = 'alpha' AND query = '';" "SELECT 'reached' FROM temp.r WHERE relevant = 1 AND query = '';" 2>/dev/null)" = twin

.PHONY: clean distclean install test vendor
