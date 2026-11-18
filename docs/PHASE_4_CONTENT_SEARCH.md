# Phase 4 — Text extraction and keyword content search

Implemented November 12, 2026. Content indexing now follows metadata reconciliation
in the background. The application searches stored passages and displays indexed
text with page/line evidence. No model inference or cloud service is involved in
this phase's keyword search.

## Supported input

| Input | Behavior |
| --- | --- |
| TXT, Markdown | Unicode text with normalized line endings |
| Source/configuration | C/C++, Python, JS/TS, Java, C#, Rust, Go, SQL, shell/PowerShell, CMake, CSS |
| Structured text | JSON/JSONL, CSV, logs, INI, YAML, TOML, XML, HTML displayed as source text |
| PDF | Qt PDF/PDFium text extraction, with physical page numbers |
| DOCX | libzip and streaming XML; document-body text with body-line locations |
| Other extensions | Searchable metadata, explicit unsupported-content status |

The exact extension allowlist lives in `src/extraction/passages.cpp`. Text readers
accept UTF-8 (with or without BOM), UTF-16 LE/BE with BOM, and UTF-32 LE/BE with BOM.
Invalid/incomplete sequences and binary control bytes receive explicit statuses;
the app does not guess legacy Windows code pages. BOM-less ASCII is valid UTF-8.
Decoding uses [Qt's QStringDecoder](https://doc.qt.io/qt-6/qstringdecoder.html)
in stateless mode so incomplete input is not silently retained for another chunk.

## Reader isolation and bounds

All supported content, including plain text, is read by `document_worker`. One
worker runs at a time, outside the GUI process, with the established 256 MiB
Windows Job Object limit and a five-second parent timeout. Pause/close interrupts
the process wait and kills the active worker. Filename search remains available
while content extraction is running. Metadata-only watcher stress tests explicitly
disable content extraction; a separate desktop test exercises the real reader
and verifies event-loop heartbeat activity while indexing.

| Limit | Outcome |
| --- | --- |
| Text input over 4 MiB | `file_too_large`; no content indexed |
| PDF/DOCX input over 32 MiB | `file_too_large` |
| More than 1,000,000 extracted UTF-16 characters | `text_limit`; retained prefix indexed and marked truncated |
| DOCX body XML over 8 MiB | `xml_limit` |
| PDF over 10,000 pages | `page_limit` |
| Worker output over 8 MiB | `output_limit` |
| Timeout/crash/locked input | Visible failure status and bounded retry behavior |

The worker copies input into a bounded in-memory snapshot, holding a read handle
that disallows concurrent writing/replacement during that copy. It rejects
reparse/offline/recall flags, requests no recall, and compares the opened handle's
final path with the requested path before reading bytes. A parent changed to a
junction is therefore rejected. PDFium reads the memory buffer; DOCX reads its
archive from memory. No plaintext snapshot files are written to temporary folders.
Real cloud-provider hydration behavior remains outside local fixture validation.

Encrypted PDFs are marked password-protected. Empty/image-only PDFs are marked
as needing OCR or having no text; no OCR is performed. DOCX headers, footers,
embedded objects, and complex layout are not extracted. XML entities/DTDs are
rejected. Original files are never modified by indexing.

## Passage boundaries

The splitter targets 350 model-independent lexical tokens, with 40 tokens of
overlap and a 4,000 UTF-16-character passage cap. Long unbroken runs are subdivided
without cutting a surrogate pair. Page boundaries remain separate; other text
retains normalized line ranges. DOCX locations are body-text lines, not invented
Word page numbers. There is no rendered document preview.

These token counts are not BGE WordPiece counts. Phase 5 must re-budget passages
with the pinned model tokenizer before embedding; Phase 4 does not require a
downloaded model vocabulary. Phrase matching is within a passage. Overlap handles
short boundary-spanning phrases, but does not guarantee a long phrase spanning
multiple pages or passages.

## Storage and revision handling

Schema version 2 migrates the existing metadata index in place, retaining roots,
file IDs, exclusions, and work queues. `documents` stores extraction status,
source size/modification time, and attempt count. `passages` stores ordered text,
location, and lexical token count. An external-content FTS5 table is maintained by
insert/delete triggers. Foreign-key cascades remove passages and FTS records when
their document or file is removed.

Metadata changes invalidate the prior text revision in the same transaction as
the metadata change. Extraction runs outside database write transactions. The
source is checked before and after reading, and publication rechecks the expected
metadata revision. A new document's passages and FTS records are committed in one
transaction. A newer failed extraction cannot leave the previous text searchable.
Concurrent readers continue to see a consistent published snapshot.

The durable root work item stays claimed through content processing. Restart
replays unfinished work, while already-published documents are cached. Transient
read failures/timeouts/crashes receive at most three attempts for an unchanged
revision. Permanent notices are cached. **Reindex file contents** explicitly
invalidates all selected folders' cached text and queues a rebuild, including
failed documents. While paused, this waits for Resume.

Revision detection currently uses normalized path, size, and modification time
at millisecond precision. An external tool that changes bytes while preserving
both size and timestamp can evade automatic invalidation; use Reindex file
contents in that case. Content hashing and finer-grained revision tracking remain
future work. Already-published text can remain visible while a root is unavailable,
following Phase 3's retained-index policy.

## Search and preview

- **Names & contents:** combines filename/path matches with content candidates;
  exact names and name prefixes rank ahead of other matches.
- **Names & paths:** retains literal substring lookup.
- **Content: all words:** quotes each input term for FTS and requires every term
  in the same passage. `AND`, `OR`, and `NOT` are ordinary words here.
- **Content: exact phrase:** quotes the entire input phrase. Matching follows the
  FTS Unicode tokenizer, so case and punctuation are not byte-for-byte constraints.
- **Advanced content syntax:** explicitly enables FTS expressions such as
  `"master theorem" OR recurrence`. Invalid expressions produce a search error.

Content candidates use BM25 and select the best matching passage per file, so
overlapping chunks do not produce duplicate file results. Folder/type/size/date
filters apply to content results too. Queries are limited to 1,024 characters;
ordinary term searches allow up to 64 terms. Results remain capped at 500 with a
refine-search hint. See [SQLite FTS5](https://www.sqlite.org/fts5.html) for its
phrase quoting, BM25, snippet, and external-content trigger semantics.

Snippets mark matches with plain-text brackets. Both snippets and the selectable
preview explicitly use Qt plain-text rendering, including for HTML/XML input.
Nothing in extracted text becomes executable markup or an automatic URL action.
The preview opens at the matching passage and provides previous/next navigation.
Search and preview requests use generation checks to discard obsolete responses.

## Validation coverage

The full 13-entry CTest suite passed in Debug and Release. The final reader changes
passed all affected Debug checks, and the final search-marker/UI refinements passed
content search and QML startup again in both configurations. The populated interface
was rendered with a real content-only match against synthetic files and no QML errors.

`content_search` contains ten integration cases covering:

- Unicode encodings, malformed/incomplete UTF-8, and binary input.
- Size/text limits, preserved truncated text, and cancellation of an active worker.
- Passage overlap, line/page labels, character bounds, and Unicode integrity.
- Literal operators, phrases, filters, deduplication, snippets, and raw-markup previews.
- Changed revisions, invalidation, rejection of stale publications, and failed newer content.
- PDF/DOCX extraction, scanned PDFs, password protection, malformed files, and persistence.
- Atomic visibility, explicit rebuilds, and cascade cleanup of FTS records.
- Migration from a version-one metadata database.
- BM25 ordering and a phrase crossing a passage boundary.
- Rejection of an input reached through a parent junction.

The desktop workflow suite includes an additional end-to-end case with real
workers: responsive background indexing, content search, matching previews, live
edits, malformed advanced syntax, filters, and paused/resumed content rebuilds.
Existing metadata, watcher overflow, worker resource-limit, and BGE parity checks
remain in the full suite. All fixtures are synthetic.

## Remaining scope

This is keyword retrieval, not semantic search. Scanned PDFs need OCR, which is
not implemented. Large collections pay for one worker startup per changed
supported file; performance optimization and independent-machine packaging
remain later gates. Phase 5 adds local embeddings and durable vector storage.
