/*
** fts5x.c - Patched FTS5 as a loadable extension ("fts5x").
**
** Vendors the full sqlite3 amalgamation (patched) with hidden visibility.
** Exports only our entry point which initializes FTS5 and custom functions.
** Uses SQLITE_EXTENSION_INIT2 to route all sqlite3_* calls through bun's
** API pointer, avoiding conflicts between our embedded sqlite3 and bun's.
*/

/* FTS5 as loadable extension using individual source files.
** No SQLITE_CORE: uses bun's sqlite3 via extension API. */
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

/* Suppress duplicate SQLITE_EXTENSION_INIT1 in fts5Int.h */
#undef SQLITE_EXTENSION_INIT1
#define SQLITE_EXTENSION_INIT1

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* FTS5 source files (from ext/fts5/, -I flags resolve headers) */
#include "fts5_aux.c"
#include "fts5_buffer.c"
#include "fts5_config.c"
#include "fts5parse.c"
#include "fts5_expr.c"
#include "fts5_hash.c"
#include "fts5_index.c"
#include "fts5_main.c"
#include "fts5_storage.c"
#include "fts5_tokenize.c"
#include "fts5_unicode2.c"
#include "fts5_varint.c"
#include "fts5_vocab.c"

/* fts5tokenize: a tokenizer's output as rows (input HIDDEN, token, start, end,
** position). SQLite compiles this file only into its test harness, so its
** guard is met here for this one file and released after it, and the unused
** parameters its methods leave unmarked are not reported under -Wextra. */
#define SQLITE_TEST
#define SQLITE_ENABLE_FTS5
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "fts5_test_tok.c"
#pragma GCC diagnostic pop
#undef SQLITE_ENABLE_FTS5
#undef SQLITE_TEST

/* ---- match_tokens() -------------------------------------------------- */
static void fts5x_match_tokens(const Fts5ExtensionApi *pApi,
    Fts5Context *pFts, sqlite3_context *pCtx, int nVal, sqlite3_value **apVal) {
  (void)nVal; (void)apVal;
  int nInst = 0, rc = pApi->xInstCount(pFts, &nInst);
  if (rc != SQLITE_OK || nInst == 0) { sqlite3_result_text(pCtx, "", 0, SQLITE_STATIC); return; }
  int nAlloc = 0, nTok = 0; char **azTok = 0;
  for (int i = 0; i < nInst; i++) {
    int iP, iC, iO; rc = pApi->xInst(pFts, i, &iP, &iC, &iO);
    if (rc != SQLITE_OK) break;
    int nP = pApi->xPhraseSize(pFts, iP);
    for (int t = 0; t < nP; t++) {
      const char *tok = 0; int nT = 0;
      rc = pApi->xInstToken(pFts, i, t, &tok, &nT);
      if (rc != SQLITE_OK || !tok) continue;
      int dup = 0;
      for (int j = 0; j < nTok; j++)
        if ((int)strlen(azTok[j]) == nT && memcmp(azTok[j], tok, nT) == 0) { dup = 1; break; }
      if (dup) continue;
      if (nTok >= nAlloc) { nAlloc = nAlloc ? nAlloc*2 : 16; azTok = sqlite3_realloc(azTok, nAlloc*sizeof(char*)); }
      azTok[nTok] = sqlite3_malloc(nT+1); memcpy(azTok[nTok], tok, nT); azTok[nTok][nT] = 0; nTok++;
    }
  }
  int total = 0; for (int i = 0; i < nTok; i++) total += strlen(azTok[i]) + 1;
  if (total == 0) { sqlite3_result_text(pCtx, "", 0, SQLITE_STATIC); }
  else { char *out = sqlite3_malloc(total), *p = out;
    for (int i = 0; i < nTok; i++) { if (i) *p++ = ' '; int l = strlen(azTok[i]); memcpy(p, azTok[i], l); p += l; }
    *p = 0; sqlite3_result_text(pCtx, out, (int)(p-out), sqlite3_free); }
  for (int i = 0; i < nTok; i++) sqlite3_free(azTok[i]); sqlite3_free(azTok);
}

/* ---- match_position() ------------------------------------------------ */
static void fts5x_match_position(const Fts5ExtensionApi *pApi,
    Fts5Context *pFts, sqlite3_context *pCtx, int nVal, sqlite3_value **apVal) {
  (void)nVal; (void)apVal;
  int nInst = 0, iP, iC, iO;
  int rc = pApi->xInstCount(pFts, &nInst);
  if (rc != SQLITE_OK || nInst == 0) { sqlite3_result_null(pCtx); return; }
  rc = pApi->xInst(pFts, 0, &iP, &iC, &iO);
  if (rc != SQLITE_OK) { sqlite3_result_null(pCtx); return; }
  sqlite3_result_int(pCtx, iO);
}

/* ---- bm25w() --------------------------------------------------------- */
/* bm25w(fts, w0, w1, ...): bm25() with one weight per phrase of the query in
** place of one per column. Phrase i's term of the BM25 sum is multiplied by
** wi (1.0 for a phrase past the last weight), which scores a weighted bag of
** terms as Lucene scores a BooleanQuery of boosted TermQuery clauses under
** BM25, the form Anserini's Rm3Reranker runs its expanded query in. The IDFs
** and the average row length come from FTS5's own fts5Bm25GetData (fts5_aux.c,
** included above) and each term is bm25()'s expression, so unit weights give
** bm25()'s score; it is negated the same way, so ORDER BY rank puts the best
** match first. Reach it per query with rank MATCH 'bm25w(w0, w1, ...)'.
** https://www.sqlite.org/fts5.html#the_bm25_function
** https://github.com/castorini/anserini/blob/master/src/main/java/io/anserini/rerank/lib/Rm3Reranker.java */
static void fts5x_bm25w(const Fts5ExtensionApi *pApi,
    Fts5Context *pFts, sqlite3_context *pCtx, int nVal, sqlite3_value **apVal) {
  const double k1 = 1.2, b = 0.75;
  Fts5Bm25Data *pData = 0;
  int nInst = 0, nTok = 0;
  int rc = fts5Bm25GetData(pApi, pFts, &pData);
  if (rc == SQLITE_OK) {
    memset(pData->aFreq, 0, sizeof(double) * pData->nPhrase);
    rc = pApi->xInstCount(pFts, &nInst);
  }
  for (int i = 0; rc == SQLITE_OK && i < nInst; i++) {
    int ip, ic, io;
    rc = pApi->xInst(pFts, i, &ip, &ic, &io);
    if (rc == SQLITE_OK) pData->aFreq[ip] += 1.0;
  }
  if (rc == SQLITE_OK) rc = pApi->xColumnSize(pFts, -1, &nTok);
  if (rc != SQLITE_OK) { sqlite3_result_error_code(pCtx, rc); return; }
  double D = (double)nTok, score = 0.0;
  for (int i = 0; i < pData->nPhrase; i++) {
    double w = i < nVal ? sqlite3_value_double(apVal[i]) : 1.0;
    double *aFreq = pData->aFreq;
    score += w * (pData->aIDF[i] * (
        ( aFreq[i] * (k1 + 1.0) ) /
        ( aFreq[i] + k1 * (1 - b + b * D / pData->avgdl) )
    ));
  }
  sqlite3_result_double(pCtx, -1.0 * score);
}

/* ---- tokenize() ------------------------------------------------------ */
typedef struct { fts5_tokenizer tok; Fts5Tokenizer *pTok; } Fts5ExtGlobal;
static int fts5x_tok_cb(void *p, int f, const char *t, int n, int s, int e) {
  (void)f; (void)s; (void)e;
  sqlite3_str *str = p; if (sqlite3_str_length(str) > 0) sqlite3_str_appendchar(str, 1, ' ');
  sqlite3_str_append(str, t, n); return SQLITE_OK; }
static void fts5x_tokenize(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  (void)argc; Fts5ExtGlobal *g = sqlite3_user_data(ctx);
  const char *text = (const char *)sqlite3_value_text(argv[0]); int n = sqlite3_value_bytes(argv[0]);
  if (!text||!g||!g->pTok) { sqlite3_result_text(ctx,"",0,SQLITE_STATIC); return; }
  sqlite3_str *s = sqlite3_str_new(0); g->tok.xTokenize(g->pTok, s, 0, text, n, fts5x_tok_cb);
  sqlite3_result_text(ctx, sqlite3_str_finish(s), -1, sqlite3_free); }

/* ---- offset_lookup() ------------------------------------------------- */
static int fts5x_get_varint(const unsigned char *p, int mx, sqlite3_int64 *v) {
  *v = 0; int i; for (i = 0; i < mx && i < 9; i++) {
    *v |= ((sqlite3_int64)(p[i]&0x7f))<<(7*i); if (!(p[i]&0x80)) { i++; break; } } return i; }
static void fts5x_offset_lookup(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  (void)argc; const unsigned char *blob = sqlite3_value_blob(argv[0]);
  int nBlob = sqlite3_value_bytes(argv[0]), nCol = sqlite3_value_int(argv[1]);
  int tokPos = sqlite3_value_int(argv[2]), interval = sqlite3_value_int(argv[3]);
  if (!blob||nBlob==0||interval<=0) { sqlite3_result_int(ctx,0); return; }
  int off = 0; for (int i = 0; i < nCol && off < nBlob; i++) { sqlite3_int64 v; off += fts5x_get_varint(&blob[off],nBlob-off,&v); }
  if (off>=nBlob) { sqlite3_result_int(ctx,0); return; }
  sqlite3_int64 nE; off += fts5x_get_varint(&blob[off],nBlob-off,&nE);
  for (int i = 0; i < (int)nE && off < nBlob; i++) { sqlite3_int64 lt; off += fts5x_get_varint(&blob[off],nBlob-off,&lt);
    if ((int)lt >= tokPos) { sqlite3_result_int64(ctx,(sqlite3_int64)i*interval); return; } }
  sqlite3_result_int64(ctx,((sqlite3_int64)nE-1)*interval); }

/* ---- snippet_text() -------------------------------------------------- */
typedef struct { int s, e; char *z; } SnipTok;
static int fts5x_snip_cb(void *p, int f, const char *t, int n, int s, int e) {
  (void)f; struct { SnipTok *a; int n; int c; } *l = p;
  if (l->n >= l->c) { int nc = l->c?l->c*2:64; l->a = sqlite3_realloc(l->a,nc*sizeof(SnipTok)); if(!l->a) return SQLITE_NOMEM; l->c = nc; }
  SnipTok *tk = &l->a[l->n++]; tk->s = s; tk->e = e;
  tk->z = sqlite3_malloc(n+1); memcpy(tk->z,t,n); tk->z[n] = 0; return SQLITE_OK; }
static void fts5x_snippet_text(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  if (argc<6) { sqlite3_result_error(ctx,"snippet_text: 6 args",-1); return; }
  Fts5ExtGlobal *g = sqlite3_user_data(ctx);
  const char *text = (const char*)sqlite3_value_text(argv[0]); int nText = sqlite3_value_bytes(argv[0]);
  const char *zSrch = (const char*)sqlite3_value_text(argv[1]);
  const char *zO = (const char*)sqlite3_value_text(argv[2]);
  const char *zC = (const char*)sqlite3_value_text(argv[3]);
  const char *zE = (const char*)sqlite3_value_text(argv[4]); if (!zE) zE = "...";
  int mx = sqlite3_value_int(argv[5]); if (mx<=0) mx = 16;
  if (!text||!zSrch||!zO||!zC) { sqlite3_result_null(ctx); return; }
  /* Parse search tokens */
  int nS = 0, nSA = 0; char **aS = 0; const char *sp = zSrch;
  while (*sp) { while(*sp==' ')sp++; if(!*sp)break; const char*ss=sp; while(*sp&&*sp!=' ')sp++;
    int l=(int)(sp-ss); if(nS>=nSA){nSA=nSA?nSA*2:8;aS=sqlite3_realloc(aS,nSA*sizeof(char*));}
    aS[nS]=sqlite3_malloc(l+1);memcpy(aS[nS],ss,l);aS[nS][l]=0;nS++; }
  /* Tokenize frame */
  struct { SnipTok *a; int n; int c; } toks = {0,0,0};
  if (g&&g->pTok) g->tok.xTokenize(g->pTok,&toks,0,text,nText,fts5x_snip_cb);
  int fm = -1;
  for (int i = 0; i < toks.n && fm < 0; i++)
    for (int j = 0; j < nS; j++) if (strcmp(toks.a[i].z,aS[j])==0) { fm=i; break; }
  if (fm<0&&toks.n>0) fm=0;
  if (toks.n==0) { sqlite3_result_text(ctx,text,nText<200?nText:200,SQLITE_TRANSIENT); goto done; }
  { int wS=fm-mx/2; if(wS<0)wS=0; int wE=wS+mx; if(wE>toks.n)wE=toks.n;
    sqlite3_str *out = sqlite3_str_new(0);
    if(wS>0) sqlite3_str_appendall(out,zE);
    int prev = wS>0 ? toks.a[wS].s : 0;
    for (int i=wS; i<wE; i++) {
      if(toks.a[i].s>prev) sqlite3_str_append(out,text+prev,toks.a[i].s-prev);
      int m=0; for(int j=0;j<nS;j++) if(strcmp(toks.a[i].z,aS[j])==0){m=1;break;}
      if(m) sqlite3_str_appendall(out,zO);
      sqlite3_str_append(out,text+toks.a[i].s,toks.a[i].e-toks.a[i].s);
      if(m) sqlite3_str_appendall(out,zC);
      prev=toks.a[i].e; }
    if(wE<toks.n) sqlite3_str_appendall(out,zE);
    sqlite3_result_text(ctx,sqlite3_str_finish(out),-1,sqlite3_free); }
done:
  for(int i=0;i<toks.n;i++) sqlite3_free(toks.a[i].z); sqlite3_free(toks.a);
  for(int i=0;i<nS;i++) sqlite3_free(aS[i]); sqlite3_free(aS); }

/* ---- fts5rm3 --------------------------------------------------------- */
/* RM3 relevance feedback over an FTS5 table, as one row:
**
**   CREATE VIRTUAL TABLE temp.rm3 USING fts5rm3(fts);        -- main.fts
**   CREATE VIRTUAL TABLE temp.rm3 USING fts5rm3(aux, fts);   -- aux.fts
**   SELECT match, rank, terms FROM temp.rm3 WHERE relevant = ? AND query = ?;
**   SELECT match, rank, terms FROM temp.rm3 WHERE feedback = ? AND query = ?;
**
** RM3 in explicit-feedback mode, as Anserini's Rm3Reranker builds it. The query
** model is the query's tokens, each weighted by its share of them. The feedback
** model is the feedback document's tokens of `shortest` to `longest` [a-z0-9]
** characters held by no more than `common` of the table's rows, the heaviest
** `expand` of them by count, each weighted by its share of those. The final
** model is `mix` of each. Tokens come from the table's own tokenizer and a
** term's rows from its own index, so the model is in the index's terms; a
** term's rows are counted only until it proves common, and only until `expand`
** terms are kept, since the heaviest terms are tried first.
** https://github.com/castorini/anserini/blob/master/src/main/java/io/anserini/rerank/lib/Rm3Reranker.java
**
** The feedback document is a row of the table, named by its rowid in
** `relevant`, as Anserini names a feedback document by its docid, or a text
** given as `feedback`. A row is read the way FTS5 reads one for an auxiliary
** function, through its own content lookup by rowid, so an external content
** table's rowid constraint reaches its content table and only that row is
** read, and its indexed columns are the document. A NULL `relevant` is no
** document, so the model is the query's alone.
**
** The model comes out as the two strings an FTS5 query takes: `match`, a flat
** OR of its terms, one phrase each, and `rank`, 'bm25w(w0, w1, ...)' with the
** weights in phrase order, both ordered by term, so a phrase and its weight
** cannot come apart. `terms` is how many feedback terms were kept. With no
** feedback term and no query token, match and rank are NULL. The hidden columns
** default to expand 10, common 0.1, shortest 2, longest 20 and mix 0.5. */
typedef struct { const char *z; int n; int c; double w; } Rm3Term;
typedef struct {
  char *a; i64 nA, nAlloc;          /* token bytes, back to back */
  i64 *aOff; int *aLen; int nT, nTAlloc;
  int bFilter, nShort, nLong;       /* keep only [a-z0-9] tokens of this length */
  int rc;
} Rm3Tokens;
typedef struct {
  sqlite3_vtab base;
  sqlite3 *db;
  Fts5Global *pGlobal;
  char *zDb, *zTbl;
  int bBusy;
} Rm3Table;
typedef struct {
  sqlite3_vtab_cursor base;
  sqlite3_stmt *pStmt;              /* holds the FTS5 table open, as fts5vocab does */
  Fts5Table *pFts5;
  i64 nRow;                         /* the table's rows, which `common` is a share of */
  char *zMatch, *zRank;
  i64 nTerms;
  int bEof;
} Rm3Cursor;

enum { RM3_MATCH, RM3_RANK, RM3_TERMS, RM3_FEEDBACK, RM3_RELEVANT, RM3_QUERY,
       RM3_EXPAND, RM3_COMMON, RM3_SHORTEST, RM3_LONGEST, RM3_MIX };

static int rm3_tok_cb(void *p, int f, const char *t, int n, int s, int e) {
  Rm3Tokens *k = p; (void)f; (void)s; (void)e;
  if (k->bFilter) {
    if (n < k->nShort || n > k->nLong) return SQLITE_OK;
    for (int i = 0; i < n; i++)
      if (!((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= '0' && t[i] <= '9'))) return SQLITE_OK;
  }
  if (k->nA + n > k->nAlloc) {
    i64 m = k->nAlloc ? k->nAlloc * 2 : 4096; while (m < k->nA + n) m *= 2;
    char *a = sqlite3_realloc64(k->a, m); if (!a) return k->rc = SQLITE_NOMEM;
    k->a = a; k->nAlloc = m;
  }
  if (k->nT >= k->nTAlloc) {
    int m = k->nTAlloc ? k->nTAlloc * 2 : 256;
    i64 *o = sqlite3_realloc64(k->aOff, m * sizeof(i64)); if (!o) return k->rc = SQLITE_NOMEM;
    k->aOff = o;
    int *l = sqlite3_realloc64(k->aLen, m * sizeof(int)); if (!l) return k->rc = SQLITE_NOMEM;
    k->aLen = l; k->nTAlloc = m;
  }
  memcpy(k->a + k->nA, t, n); k->aOff[k->nT] = k->nA; k->aLen[k->nT] = n; k->nT++; k->nA += n;
  return SQLITE_OK;
}

/* SQLite's BINARY order: bytes, then the shorter first. */
static int rm3_cmp_term(const void *x, const void *y) {
  const Rm3Term *a = x, *b = y;
  int c = memcmp(a->z, b->z, a->n < b->n ? a->n : b->n);
  return c ? c : a->n - b->n;
}
static int rm3_cmp_heavy(const void *x, const void *y) {
  const Rm3Term *a = x, *b = y;
  return a->c != b->c ? (a->c > b->c ? -1 : 1) : rm3_cmp_term(x, y);
}

/* A text's tokens, appended to k. */
static int rm3_tokenize(Fts5Config *pConfig, const char *z, int n, Rm3Tokens *k) {
  int rc = n > 0 ? sqlite3Fts5Tokenize(pConfig, FTS5_TOKENIZE_DOCUMENT, z, n, k, rm3_tok_cb) : SQLITE_OK;
  return rc == SQLITE_OK ? k->rc : rc;
}

/* The tokens of the table's row iRow, appended to k: each indexed column of the
** row FTS5's content lookup returns, as fts5SeekCursor reads a row for an
** auxiliary function. A rowid naming no row is an error, as it is for vec0's
** rocchio, since a document that is not there cannot be judged relevant. */
static int rm3_row(Fts5Table *pFts5, i64 iRow, Rm3Tokens *k, char **pzErr) {
  Fts5Config *pConfig = pFts5->pConfig;
  if (pConfig->eContent == FTS5_CONTENT_NONE) {
    *pzErr = sqlite3_mprintf("fts5rm3: %s is contentless, so no row of it can be read as feedback", pConfig->zName);
    return SQLITE_ERROR;
  }
  Fts5Storage *pStorage = ((Fts5FullTable*)pFts5)->pStorage;
  sqlite3_stmt *pLookup = 0;
  int rc = sqlite3Fts5StorageStmt(pStorage, FTS5_STMT_LOOKUP, &pLookup, pzErr);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_int64(pLookup, 1, iRow);
  pConfig->bLock++;
  int rcStep = sqlite3_step(pLookup);
  pConfig->bLock--;
  if (rcStep == SQLITE_ROW) {
    for (int i = 0; rc == SQLITE_OK && i < pConfig->nCol; i++) {
      if (pConfig->abUnindexed[i]) continue;
      const char *z = (const char*)sqlite3_column_text(pLookup, i + 1);
      rc = rm3_tokenize(pConfig, z, sqlite3_column_bytes(pLookup, i + 1), k);
    }
  } else if (rcStep == SQLITE_DONE) {
    *pzErr = sqlite3_mprintf("fts5rm3: no row %lld in %s to read as feedback", iRow, pConfig->zName);
    rc = SQLITE_ERROR;
  }
  int rcReset = sqlite3_reset(pLookup);
  if (rc == SQLITE_OK) rc = rcReset;
  sqlite3Fts5StorageStmtRelease(pStorage, FTS5_STMT_LOOKUP, pLookup);
  return rc;
}

/* The distinct tokens in k with their counts, in BINARY order. */
static int rm3_count(Rm3Tokens *k, Rm3Term **paT, int *pnT) {
  *paT = 0; *pnT = 0;
  if (k->nT == 0) return SQLITE_OK;
  Rm3Term *a = sqlite3_malloc64(k->nT * sizeof(Rm3Term));
  if (!a) return SQLITE_NOMEM;
  for (int i = 0; i < k->nT; i++) { a[i].z = k->a + k->aOff[i]; a[i].n = k->aLen[i]; a[i].c = 1; a[i].w = 0; }
  qsort(a, k->nT, sizeof(Rm3Term), rm3_cmp_term);
  int m = 0;
  for (int i = 0; i < k->nT; i++) {
    if (m > 0 && rm3_cmp_term(&a[m-1], &a[i]) == 0) a[m-1].c++;
    else a[m++] = a[i];
  }
  *paT = a; *pnT = m;
  return SQLITE_OK;
}

/* Whether more than nCap of the table's rows hold a term, counting only that far. */
static int rm3_common(Fts5Index *pIndex, const char *z, int n, double nCap, int *pbCommon) {
  Fts5IndexIter *pIter = 0; i64 nDoc = 0;
  int rc = sqlite3Fts5IndexQuery(pIndex, z, n, FTS5INDEX_QUERY_NOTOKENDATA, 0, &pIter);
  while (rc == SQLITE_OK && !sqlite3Fts5IterEof(pIter) && (double)nDoc <= nCap) {
    nDoc++;
    rc = sqlite3Fts5IterNext(pIter);
  }
  sqlite3Fts5IterClose(pIter);
  *pbCommon = (double)nDoc > nCap;
  return rc;
}

static int rm3Connect(sqlite3 *db, void *pAux, int argc, const char *const *argv,
                      sqlite3_vtab **ppVtab, char **pzErr) {
  if (argc != 4 && argc != 5) {
    *pzErr = sqlite3_mprintf("fts5rm3 takes the FTS5 table, as fts5rm3(table) or fts5rm3(schema, table)");
    return SQLITE_ERROR;
  }
  int rc = sqlite3_declare_vtab(db, "CREATE TABLE x(match, rank, terms, feedback HIDDEN, relevant HIDDEN,"
                                    " query HIDDEN, expand HIDDEN, common HIDDEN, shortest HIDDEN, longest HIDDEN,"
                                    " mix HIDDEN)");
  if (rc != SQLITE_OK) return rc;
  Rm3Table *p = sqlite3_malloc(sizeof(*p));
  if (!p) return SQLITE_NOMEM;
  memset(p, 0, sizeof(*p));
  p->db = db; p->pGlobal = pAux;
  p->zDb = sqlite3_mprintf("%s", argc == 5 ? argv[3] : "main");
  p->zTbl = sqlite3_mprintf("%s", argv[argc - 1]);
  if (!p->zDb || !p->zTbl) { sqlite3_free(p->zDb); sqlite3_free(p->zTbl); sqlite3_free(p); return SQLITE_NOMEM; }
  sqlite3Fts5Dequote(p->zDb); sqlite3Fts5Dequote(p->zTbl);
  *ppVtab = &p->base;
  return SQLITE_OK;
}
static int rm3Disconnect(sqlite3_vtab *pVtab) {
  Rm3Table *p = (Rm3Table*)pVtab;
  sqlite3_free(p->zDb); sqlite3_free(p->zTbl); sqlite3_free(p);
  return SQLITE_OK;
}

/* idxNum carries one bit per hidden column given, and argv holds them in column
** order. The hidden columns are arguments, so a plan that cannot supply one it
** names is refused, as json_each refuses one, and so is a plan with neither
** feedback form. */
static int rm3BestIndex(sqlite3_vtab *pVtab, sqlite3_index_info *pInfo) {
  (void)pVtab;
  int aCon[RM3_MIX + 1]; for (int i = 0; i <= RM3_MIX; i++) aCon[i] = -1;
  for (int i = 0; i < pInfo->nConstraint; i++) {
    const struct sqlite3_index_constraint *c = &pInfo->aConstraint[i];
    if (c->iColumn < RM3_FEEDBACK || c->op != SQLITE_INDEX_CONSTRAINT_EQ) continue;
    if (!c->usable) return SQLITE_CONSTRAINT;
    aCon[c->iColumn] = i;
  }
  if (aCon[RM3_FEEDBACK] < 0 && aCon[RM3_RELEVANT] < 0) return SQLITE_CONSTRAINT;
  int iArg = 1, idxNum = 0;
  for (int col = RM3_FEEDBACK; col <= RM3_MIX; col++) {
    if (aCon[col] < 0) continue;
    pInfo->aConstraintUsage[aCon[col]].argvIndex = iArg++;
    pInfo->aConstraintUsage[aCon[col]].omit = 1;
    idxNum |= 1 << (col - RM3_FEEDBACK);
  }
  pInfo->idxNum = idxNum;
  pInfo->estimatedCost = 10.0;
  pInfo->estimatedRows = 1;
  pInfo->idxFlags = SQLITE_INDEX_SCAN_UNIQUE;
  return SQLITE_OK;
}

static int rm3Open(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCsr) {
  Rm3Table *pTab = (Rm3Table*)pVtab;
  if (pTab->bBusy) {
    pVtab->zErrMsg = sqlite3_mprintf("recursive definition for %s.%s", pTab->zDb, pTab->zTbl);
    return SQLITE_ERROR;
  }
  Rm3Cursor *pCsr = sqlite3_malloc(sizeof(*pCsr));
  if (!pCsr) return SQLITE_NOMEM;
  memset(pCsr, 0, sizeof(*pCsr));
  int rc = SQLITE_OK;
  char *zSql = sqlite3Fts5Mprintf(&rc, "SELECT t.%Q FROM %Q.%Q AS t WHERE t.%Q MATCH '*id'",
                                  pTab->zTbl, pTab->zDb, pTab->zTbl, pTab->zTbl);
  if (zSql) rc = sqlite3_prepare_v2(pTab->db, zSql, -1, &pCsr->pStmt, 0);
  sqlite3_free(zSql);
  if (rc == SQLITE_ERROR) rc = SQLITE_OK;
  pTab->bBusy = 1;
  if (pCsr->pStmt && sqlite3_step(pCsr->pStmt) == SQLITE_ROW)
    pCsr->pFts5 = sqlite3Fts5TableFromCsrid(pTab->pGlobal, sqlite3_column_int64(pCsr->pStmt, 0));
  pTab->bBusy = 0;
  if (rc == SQLITE_OK && pCsr->pFts5 == 0) {
    pVtab->zErrMsg = sqlite3_mprintf("no such fts5 table: %s.%s", pTab->zDb, pTab->zTbl);
    rc = SQLITE_ERROR;
  }
  if (rc == SQLITE_OK) rc = sqlite3Fts5FlushToDisk(pCsr->pFts5);
  /* The table's rows are its %_docsize rows, one per row indexed; FTS5's own
  ** running total (the averages record bm25 reads) stands in only for a table
  ** kept without them (columnsize=0). */
  if (rc == SQLITE_OK && pCsr->pFts5->pConfig->bColumnsize) {
    sqlite3_stmt *pCount = 0;
    char *zCount = sqlite3_mprintf("SELECT count(*) FROM %Q.'%q_docsize'", pTab->zDb, pTab->zTbl);
    rc = zCount ? sqlite3_prepare_v2(pTab->db, zCount, -1, &pCount, 0) : SQLITE_NOMEM;
    sqlite3_free(zCount);
    if (rc == SQLITE_OK && sqlite3_step(pCount) == SQLITE_ROW) pCsr->nRow = sqlite3_column_int64(pCount, 0);
    int rcFinal = sqlite3_finalize(pCount);
    if (rc == SQLITE_OK) rc = rcFinal;
  } else if (rc == SQLITE_OK) {
    rc = sqlite3Fts5StorageRowCount(((Fts5FullTable*)pCsr->pFts5)->pStorage, &pCsr->nRow);
  }
  if (rc != SQLITE_OK) { sqlite3_finalize(pCsr->pStmt); sqlite3_free(pCsr); return rc; }
  *ppCsr = &pCsr->base;
  return SQLITE_OK;
}
static void rm3Reset(Rm3Cursor *pCsr) {
  sqlite3_free(pCsr->zMatch); sqlite3_free(pCsr->zRank);
  pCsr->zMatch = pCsr->zRank = 0; pCsr->nTerms = 0; pCsr->bEof = 0;
}
static int rm3Close(sqlite3_vtab_cursor *pCursor) {
  Rm3Cursor *pCsr = (Rm3Cursor*)pCursor;
  rm3Reset(pCsr);
  sqlite3_finalize(pCsr->pStmt);
  sqlite3_free(pCsr);
  return SQLITE_OK;
}

static int rm3Filter(sqlite3_vtab_cursor *pCursor, int idxNum, const char *idxStr, int argc, sqlite3_value **argv) {
  Rm3Cursor *pCsr = (Rm3Cursor*)pCursor;
  Fts5Table *pFts5 = pCsr->pFts5;
  (void)idxStr; (void)argc;
  rm3Reset(pCsr);
  sqlite3_value *aVal[RM3_MIX + 1] = {0};
  for (int col = RM3_FEEDBACK, i = 0; col <= RM3_MIX; col++)
    if (idxNum & (1 << (col - RM3_FEEDBACK))) aVal[col] = argv[i++];
  const char *zFb = aVal[RM3_FEEDBACK] ? (const char*)sqlite3_value_text(aVal[RM3_FEEDBACK]) : 0;
  int nFb = zFb ? sqlite3_value_bytes(aVal[RM3_FEEDBACK]) : 0;
  const char *zQ = aVal[RM3_QUERY] ? (const char*)sqlite3_value_text(aVal[RM3_QUERY]) : 0;
  int nQ = zQ ? sqlite3_value_bytes(aVal[RM3_QUERY]) : 0;
  int nExpand = aVal[RM3_EXPAND] ? sqlite3_value_int(aVal[RM3_EXPAND]) : 10;
  double rCommon = aVal[RM3_COMMON] ? sqlite3_value_double(aVal[RM3_COMMON]) : 0.1;
  int nShort = aVal[RM3_SHORTEST] ? sqlite3_value_int(aVal[RM3_SHORTEST]) : 2;
  int nLong = aVal[RM3_LONGEST] ? sqlite3_value_int(aVal[RM3_LONGEST]) : 20;
  double rMix = aVal[RM3_MIX] ? sqlite3_value_double(aVal[RM3_MIX]) : 0.5;

  sqlite3_value *pRel = aVal[RM3_RELEVANT];
  int bRel = pRel && sqlite3_value_type(pRel) != SQLITE_NULL;
  if (bRel && aVal[RM3_FEEDBACK]) {
    pCursor->pVtab->zErrMsg = sqlite3_mprintf("fts5rm3 takes its feedback as a row or as a text, not both");
    return SQLITE_ERROR;
  }
  if (bRel && sqlite3_value_numeric_type(pRel) != SQLITE_INTEGER) {
    pCursor->pVtab->zErrMsg = sqlite3_mprintf("fts5rm3: relevant must be a rowid");
    return SQLITE_ERROR;
  }

  Rm3Tokens kq, kf;
  memset(&kq, 0, sizeof(kq)); memset(&kf, 0, sizeof(kf));
  kf.bFilter = 1; kf.nShort = nShort; kf.nLong = nLong;
  Rm3Term *aQ = 0, *aF = 0, *aModel = 0;
  int nQT = 0, nFT = 0, nKept = 0, nModel = 0;
  i64 nRow = pCsr->nRow;

  int rc = rm3_tokenize(pFts5->pConfig, zQ, nQ, &kq);
  if (rc == SQLITE_OK) rc = rm3_count(&kq, &aQ, &nQT);
  if (rc == SQLITE_OK) {
    rc = bRel ? rm3_row(pFts5, sqlite3_value_int64(pRel), &kf, &pCursor->pVtab->zErrMsg)
              : rm3_tokenize(pFts5->pConfig, zFb, nFb, &kf);
  }
  if (rc == SQLITE_OK) rc = rm3_count(&kf, &aF, &nFT);

  /* The heaviest first, so the rows are counted only for terms that might be kept. */
  if (rc == SQLITE_OK && nFT > 0) {
    qsort(aF, nFT, sizeof(Rm3Term), rm3_cmp_heavy);
    for (int i = 0; rc == SQLITE_OK && i < nFT && nKept < nExpand; i++) {
      int bCommon = 0;
      rc = rm3_common(pFts5->pIndex, aF[i].z, aF[i].n, (double)nRow * rCommon, &bCommon);
      if (rc == SQLITE_OK && !bCommon) aF[nKept++] = aF[i];
    }
  }

  if (rc == SQLITE_OK && nQT + nKept > 0) {
    aModel = sqlite3_malloc64((nQT + nKept) * sizeof(Rm3Term));
    if (!aModel) rc = SQLITE_NOMEM;
  }
  if (rc == SQLITE_OK && aModel) {
    i64 nQAll = 0, nFAll = 0;
    for (int i = 0; i < nQT; i++) nQAll += aQ[i].c;
    for (int i = 0; i < nKept; i++) nFAll += aF[i].c;
    for (int i = 0; i < nQT; i++) { aModel[nModel] = aQ[i]; aModel[nModel++].w = rMix * ((double)aQ[i].c / (double)nQAll); }
    for (int i = 0; i < nKept; i++) { aModel[nModel] = aF[i]; aModel[nModel++].w = rMix * ((double)aF[i].c / (double)nFAll); }
    qsort(aModel, nModel, sizeof(Rm3Term), rm3_cmp_term);
    int m = 0;
    for (int i = 0; i < nModel; i++) {
      if (m > 0 && rm3_cmp_term(&aModel[m-1], &aModel[i]) == 0) aModel[m-1].w += aModel[i].w;
      else aModel[m++] = aModel[i];
    }
    nModel = m;
    sqlite3_str *zM = sqlite3_str_new(0), *zR = sqlite3_str_new(0);
    sqlite3_str_appendall(zR, "bm25w(");
    for (int i = 0; i < nModel; i++) {
      if (i) { sqlite3_str_appendall(zM, " OR "); sqlite3_str_appendall(zR, ", "); }
      sqlite3_str_appendf(zM, "\"%.*s\"", aModel[i].n, aModel[i].z);
      sqlite3_str_appendf(zR, "%.6f", aModel[i].w);
    }
    sqlite3_str_appendall(zR, ")");
    pCsr->zMatch = sqlite3_str_finish(zM);
    pCsr->zRank = sqlite3_str_finish(zR);
    if (!pCsr->zMatch || !pCsr->zRank) rc = SQLITE_NOMEM;
  }
  pCsr->nTerms = nKept;

  sqlite3_free(aQ); sqlite3_free(aF); sqlite3_free(aModel);
  sqlite3_free(kq.a); sqlite3_free(kq.aOff); sqlite3_free(kq.aLen);
  sqlite3_free(kf.a); sqlite3_free(kf.aOff); sqlite3_free(kf.aLen);
  return rc;
}
static int rm3Next(sqlite3_vtab_cursor *pCursor) { ((Rm3Cursor*)pCursor)->bEof = 1; return SQLITE_OK; }
static int rm3Eof(sqlite3_vtab_cursor *pCursor) { return ((Rm3Cursor*)pCursor)->bEof; }
static int rm3Rowid(sqlite3_vtab_cursor *pCursor, sqlite3_int64 *pRowid) { (void)pCursor; *pRowid = 1; return SQLITE_OK; }
static int rm3Column(sqlite3_vtab_cursor *pCursor, sqlite3_context *ctx, int i) {
  Rm3Cursor *pCsr = (Rm3Cursor*)pCursor;
  switch (i) {
    case RM3_MATCH: if (pCsr->zMatch) sqlite3_result_text(ctx, pCsr->zMatch, -1, SQLITE_TRANSIENT); break;
    case RM3_RANK: if (pCsr->zRank) sqlite3_result_text(ctx, pCsr->zRank, -1, SQLITE_TRANSIENT); break;
    case RM3_TERMS: sqlite3_result_int64(ctx, pCsr->nTerms); break;
    default: break;
  }
  return SQLITE_OK;
}
static sqlite3_module fts5x_rm3_module = {
  0, rm3Connect, rm3Connect, rm3BestIndex, rm3Disconnect, rm3Disconnect,
  rm3Open, rm3Close, rm3Filter, rm3Next, rm3Eof, rm3Column, rm3Rowid,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* ---- Entry point ----------------------------------------------------- */
int sqlite3_fts5x_init(sqlite3 *db, char **pzErrMsg,
                         const sqlite3_api_routines *pApi) {
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;

  /* Initialize FTS5 (registers as "fts5x" via patched source) */
  int rc = sqlite3_fts_init(db, pzErrMsg, pApi);
  if (rc != SQLITE_OK) return rc;

  fts5_api *pFtsApi = 0;
  { sqlite3_stmt *stmt = 0;
    rc = sqlite3_prepare_v2(db, "SELECT fts5(?1)", -1, &stmt, 0);
    if (rc == SQLITE_OK) { sqlite3_bind_pointer(stmt, 1, &pFtsApi, "fts5_api_ptr", 0); sqlite3_step(stmt); }
    sqlite3_finalize(stmt); }
  if (!pFtsApi) return SQLITE_ERROR;

  rc = pFtsApi->xCreateFunction(pFtsApi, "match_tokens", 0, fts5x_match_tokens, 0);
  if (rc != SQLITE_OK) return rc;
  rc = pFtsApi->xCreateFunction(pFtsApi, "match_position", 0, fts5x_match_position, 0);
  if (rc != SQLITE_OK) return rc;
  rc = pFtsApi->xCreateFunction(pFtsApi, "bm25w", 0, fts5x_bm25w, 0);
  if (rc != SQLITE_OK) return rc;
  rc = sqlite3Fts5TestRegisterTok(db, pFtsApi);
  if (rc != SQLITE_OK) return rc;
  /* fts5_api is the first member of FTS5's connection-wide Fts5Global. */
  rc = sqlite3_create_module_v2(db, "fts5rm3", &fts5x_rm3_module, (Fts5Global*)pFtsApi, 0);
  if (rc != SQLITE_OK) return rc;

  Fts5ExtGlobal *g = sqlite3_malloc(sizeof(*g)); memset(g, 0, sizeof(*g));
  { void *ud = 0;
    if (SQLITE_OK == pFtsApi->xFindTokenizer(pFtsApi, "unicode61", &ud, &g->tok))
      g->tok.xCreate(ud, 0, 0, &g->pTok); }

  rc = sqlite3_create_function_v2(db, "tokenize", 1, SQLITE_UTF8, g, fts5x_tokenize, 0, 0, 0);
  if (rc != SQLITE_OK) return rc;
  rc = sqlite3_create_function(db, "offset_lookup", 4, SQLITE_UTF8, 0, fts5x_offset_lookup, 0, 0);
  if (rc != SQLITE_OK) return rc;
  return sqlite3_create_function_v2(db, "snippet_text", 6, SQLITE_UTF8, g, fts5x_snippet_text, 0, 0, 0);
}
