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
