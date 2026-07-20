# sqlite-fts5x

SQLite's FTS5 built as a separate loadable extension named `fts5x`, so it coexists
with a runtime's built-in `fts5` (Bun's, for one) without a module-name clash. Load
with `sqlite3_fts5x_init`.

It adds `store_offsets=N` to the FTS5 config: while indexing, the module records the
last token in each N-byte frame of the source content and appends that frame table to
`%_docsize`. A match can then be mapped to the byte range of its frame and the snippet
recovered by seekable decompression of just that frame, never a full-document read.
Paired with external content (`content='...'`), the index holds tokens only, no copy
of the source text.

## Auxiliary functions

- `match_tokens(fts)`: matched token text from the inverted index (no content read)
- `match_position(fts)`: token offset of the first match (from `xInst`)
- `offset_lookup(docsize, nCol, tokPos, interval)`: byte offset of the frame holding a token
- `snippet_text(text, tokens, open, close, ellipsis, n)`: highlight matched tokens in a text
- `tokenize(text)`: the FTS5 `unicode61` tokenizer as a scalar

`match_tokens` is the text expression the companion
[`sqlite-mmr`](https://github.com/MayCXC/sqlite-mmr) extension consumes: its `mmr`
virtual table reranks an FTS5 MATCH for diversity by Jaccard similarity over token
sets, and `match_tokens` feeds it clean, already-tokenized text straight from the
index with no content decompression. `offset_lookup` and `snippet_text` together
recover the highlighted snippet for a hit from its stored frame, so neither the
reranker nor the snippet path ever reads the full document.

## Build

    make vendor   # fetch SQLite's fts5 sources and apply patches/fts5x.patch
    make          # -> fts5x.so

`make vendor` downloads the pinned SQLite amalgamation (`SQLITE_VERSION` in the
Makefile), extracts the FTS5 sources and the `lemon` parser tool, and applies
`patches/fts5x.patch`. `make install` copies `fts5x.so` to `/usr/local/lib`.
