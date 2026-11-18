# FileSearcher implementation gameplan

Status: Phases 1–4 are implemented and locally verified. Folder indexing,
filename/content search, text previews, automatic updates, and pause/resume are usable. The Phase 0
shell/build foundation is present; independent-machine deployment checks remain
outstanding. See `docs/PHASE_1_FEASIBILITY.md`, `docs/PHASE_2_METADATA.md`,
`docs/PHASE_3_INCREMENTAL.md`, and `docs/PHASE_4_CONTENT_SEARCH.md` for evidence and limitations.

This document is the working roadmap for a Windows desktop application that finds files by name, contents, and meaning, then helps users organize them safely. Work proceeds in phase order. A phase is complete when its acceptance criteria pass, not merely when its code exists.

## 1. Product direction

Build a local application for searching selected folders containing university notes, PDFs, resumes, receipts, and programming projects. Search should stay useful while indexing runs. Results should explain themselves through matching passages, filenames, and paths.

The first release is a search companion to Windows File Explorer. Users can open a result or reveal it in Explorer. Replacing Explorer, implementing shell extensions, and reproducing Everything's whole-drive indexing performance are outside the first release.

### Release boundaries

| Release | Included | Exit point |
| --- | --- | --- |
| Foundation preview | Folder selection, metadata indexing, filename/path search, live updates | Phase 3 |
| Search MVP | Text extraction, keyword and semantic search, snippets, filters, reliable local packaging | Phases 4–7 plus Phase 10 release checks |
| Organizer release | Related files, approved organization suggestions, undo, cleanup review | Phases 8–10 |

The first useful deliverable is metadata search. The main product milestone is hybrid search over real documents. Organization and cleanup build on that index.

### Operating principles

- Index only folders the user selects; show what is included and excluded.
- Run extraction and AI inference locally. No cloud account or remote inference is required.
- Obtain explicit approval for file moves and cleanup actions.
- Never overwrite a destination file to resolve a naming collision automatically.
- Keep the interface responsive through bounded background work and cancellation.
- Treat SQLite as authoritative application state and vector indexes as rebuildable caches.
- Show unsupported, failed, skipped, and still-processing files separately.
- Prefer evidence-backed snippets over generated explanations.

## 2. Technology decisions

| Component | Planned choice | Implementation notes |
| --- | --- | --- |
| Core | C++20 | RAII, explicit ownership, bounded queues, cancellation, minimal global state |
| Interface | Qt 6 Quick, QML, Qt Quick Controls | QML for presentation; C++ models and services for behavior |
| Compiler and target | MSVC, Windows x64 | Start with Windows 11 x64; verify the exact minimum OS when pinning dependencies |
| Builds | CMake, Ninja, CMake Presets | Debug and Release presets; buildable without a specific editor |
| Dependencies | vcpkg manifest and pinned baseline | Install Qt through its official installer initially; pin its version separately and use a matching MSVC kit |
| Metadata and keyword index | SQLite with FTS5 | Prepared statements, migrations, WAL mode, serialized writes, explicit transactions |
| Embeddings | ONNX Runtime CPU | No GPU requirement; accelerated execution is a later measured optimization |
| Initial model | BAAI/bge-small-en-v1.5 | English baseline, 384 dimensions; benchmark before finalizing the model revision |
| Tokenization | Model-specific C++ BERT WordPiece | Validated against the pinned BGE reference in Phase 1 |
| Vector retrieval | hnswlib | Stable chunk IDs, normalized vectors, serialized mutation, persisted generations |
| Extraction | Qt PDF/PDFium, libzip + XML for DOCX, optional IFilter | Bundled PDF/DOCX routes validated in Phase 1; direct text readers follow |
| Monitoring | ReadDirectoryChangesW | Notifications plus reconciliation scans; events are hints, not an authoritative log |
| Duplicate hashing | SHA-256 through Windows CNG | Stream files; hash equal-size candidates on demand |
| Tests | Catch2, CTest, Qt Test where needed | Core behavior, temporary-directory integration tests, focused UI tests |
| Packaging | windeployqt and a Windows installer | Start with a portable Release folder; choose an installer after verifying its current terms |

Use no Python runtime in the shipped application. Python may be used as a development-only reference for validating model output or preparing a pinned model export.

All selected components must have a no-purchase-fee path appropriate for this project. Before shipping, check the actual module and dependency licenses, preserve notices, and satisfy Qt's LGPL requirements if that license route is used. Dynamic linking is part of that approach, not the entire compliance process. A commercial license, cloud API, or paid installer is not a default dependency.

### Decisions to validate early

1. The pinned Qt/MSVC/CMake combination builds and packages on a clean Windows machine.
2. The tokenizer, ONNX graph, pooling, normalization, and query instruction reproduce the model's reference embeddings.
3. PDF extraction works on the declared supported installation. If IFilter coverage is insufficient, select and license-review a bundled PDF extractor before promising PDF support. PDFium is a candidate to evaluate, not a committed dependency.
4. The initial English embedding model retrieves useful passages from the user's representative document categories.

## 3. Architecture and ownership

```text
QML interface and C++ UI models
                |
         Application services
         /          |          \
 Search service  Indexing jobs  Suggestion/operation services
       |             |                   |
       |      Scan -> Extract -> Chunk -> Embed
       |             |                   |
       +--------- SQLite ----------------+
       |       metadata, text, vectors, operation journal
       +--------- HNSW cache
```

Keep search and indexing logic independent of QML. Windows-specific filesystem code lives behind adapters. The initial application owns its background workers; it is not a Windows service. Closing the application stops indexing safely and leaves resumable work for the next launch.

### Planned repository layout

```text
src/
  core/          Domain types, configuration, error and cancellation types
  storage/       SQLite access, migrations, repositories
  indexing/      Scan, reconcile, job scheduling, chunking
  extraction/    Text readers, IFilter integration, extractor worker
  ai/            Tokenization, model execution, embedding versioning
  search/        Filename, lexical, vector, and hybrid ranking
  organization/  Categories, related files, suggestions
  operations/    Approved moves, undo journal, cleanup actions
  platform/      Windows watching, identity, shell, hashing adapters
  app/           Composition, C++ UI models, QML interface
tests/
  unit/
  integration/
  fixtures/      Small synthetic documents only
tools/           Development utilities and model validation
docs/            Architecture decisions, benchmark method, release checklist
```

### Concurrency and persistence rules

- The UI thread only handles presentation and short orchestration tasks.
- Use bounded extraction and embedding queues, with search prioritized over bulk indexing.
- Serialize database writes. Use separate read connections where appropriate.
- Do not concurrently mutate an HNSW index while readers use it without explicit coordination.
- Run untrusted document parsers/IFilters in a separate worker process with a timeout and resource limits. Process isolation is crash containment, not a complete security sandbox.
- Publish extracted chunks and embeddings only if their source file revision is still current.
- Use a persisted job state to resume interrupted work; processing the same revision twice must be harmless.
- File watchers debounce save storms. Overflow, restarts, and uncertain rename events schedule reconciliation.

## 4. Data model

| Entity | Main fields and purpose |
| --- | --- |
| indexed_roots | Path, enabled state, exclusions, scan generation, last successful scan |
| files | Stable application ID, root, path, name, extension, size, modified time, Windows file identity when available, revision, extraction status |
| chunks | Stable chunk ID, file ID and revision, ordinal, text, character/page locator when available, content hash |
| text_fts | FTS5 projection of names, paths, and chunk text; independently rebuildable |
| embeddings | Chunk ID, model fingerprint, dimension, normalized vector, source revision |
| model_versions | Model/tokenizer hashes, pooling and normalization configuration, query prefix, chunking version |
| indexing_jobs | File revision, stage, attempts, state, error, timestamps |
| vector_generations | Model fingerprint, persisted index generation, applied change sequence |
| categories | User label, description, destination, optional example files |
| suggestions | File revision, proposed action, supporting evidence, score, review state |
| file_operations | Source/destination, expected identity/hash, operation state, timestamps, undo/recovery state |

Store database, model files, and caches under the user's local application-data directory. Keep caches out of watched roots. Do not put document contents, models, indexes, or real user paths into Git fixtures.

SQLite transactions cannot atomically include an external HNSW file or a filesystem move. Persist intent and progress explicitly. On startup, reconcile incomplete operations and vector generations. Filter stale/deleted chunk IDs from results immediately, even before vector compaction.

Changing a model or its preprocessing creates a new embedding version. Never mix vectors from incompatible versions. Keep keyword search available during rebuilding.

## 5. Implementation phases

### Phase 0 — Buildable skeleton

**Goal:** a reproducible application that launches and has a testable core.

- Pin toolchain and library versions, add CMake presets and the vcpkg manifest.
- Add `.gitignore` entries for build outputs, local databases, downloaded models, and editor state.
- Create a core library, application executable, and test target.
- Add a QML window with search, results, folder settings, and indexing-status placeholders.
- Add structured error handling and logs that exclude document contents by default.
- Document build prerequisites and commands; establish a Windows build/test CI job.

**Done when:** a fresh checkout can configure, build, run, and execute CTest using documented commands. The shell opens without needing a model download.

### Phase 1 — Technical feasibility checks

**Goal:** resolve the highest-risk integrations before building the whole pipeline.

- Run BGE embeddings from C++ using pinned model/tokenizer assets and reference sentences.
- Compare token IDs and final embeddings against the model's reference implementation; test Unicode, empty text, padding, truncation, and multi-item batches.
- Verify the model-specific pooling and normalization; do not assume every embedding model uses mean pooling.
- Test IFilter extraction on text PDFs, DOCX, missing filters, malformed documents, encrypted documents, and scanned PDFs.
- Verify the isolated extractor can time out or crash without terminating the app.
- Save and reload a small HNSW index; verify stable chunk mappings and deletion behavior.
- Create a small synthetic retrieval set with paraphrased queries, exact filenames, and negative examples.
- Record dependency versions, extraction coverage, and the selected PDF/tokenization approach in architecture decision notes.

**Done when:** model output matches the reference within a documented numerical tolerance, a supported PDF route is established, and feasibility failures have an explicit resolution. A scanned PDF is reported as needing OCR, not silently treated as successfully indexed text.

### Phase 2 — Metadata index and ordinary file search

**Goal:** deliver the first useful search application.

- Add folder selection, root persistence, exclusion rules, and an initial recursive scan.
- Handle access-denied folders, locked files, Unicode names, long paths, and disappearing files.
- Skip directory junctions/symlinks by default to avoid loops and indexing outside selected roots.
- Avoid duplicate records for overlapping roots; distinguish hard-link paths when presenting files.
- Implement fast exact, prefix, and substring filename/path lookup. FTS word matching alone is insufficient for punctuation and short filename fragments.
- Add type, folder, size, and modified-date filters with parameterized queries.
- Display name, path, type, size, date, and indexing status; open or reveal a file through Windows shell APIs.
- Support cancellation and progress reporting without blocking the UI.

**Done when:** repeated scans do not create duplicate records; known files are found; missing files and permission errors do not crash the app; search works after restart. Tests use temporary directories rather than personal files.

### Phase 3 — Reliable incremental indexing

**Goal:** keep indexed metadata accurate as files change.

- Add directory watchers, debouncing, and a durable work queue.
- Process creates, modifications, renames, and deletes; use file identity where available and safe fallbacks otherwise.
- Reconcile on startup, after watcher overflow, and periodically while running.
- Mark files deleted only after their accessible root has been reconciled successfully; a disconnected drive must not erase its index.
- Add bounded concurrency, retry limits, pause/resume, and user-requested reindexing.
- Define initial cloud-placeholder behavior: skip content reads that would trigger downloads unless the user opts in.

**Done when:** a fixture directory subjected to rapid saves, renames, deletions, and restart scenarios converges to the real filesystem state. Removing an indexed root removes its search data without deleting its files.

### Phase 4 — Text extraction and keyword search

**Goal:** search contents and show useful evidence.

- Implement encoding-aware readers for TXT, Markdown, source code, JSON, and other explicitly supported text formats.
- Integrate the validated document extractors through the isolated worker.
- Set file-size, processing-time, and extracted-text limits; report truncation and unsupported formats explicitly.
- Split documents into token-aware passages while retaining section/page locations when available. Start around 300–400 tokens with modest overlap, then tune against the model's actual input limit and retrieval results.
- Store text and FTS5 records transactionally; replace old revisions atomically.
- Treat ordinary user queries as text; provide advanced FTS syntax only through an explicit mode.
- Add BM25 results, escaped snippets, and a text-preview pane. Do not execute embedded HTML or document content.

**Done when:** exact phrases and known terms retrieve the expected document and passage; changed content replaces stale content; malformed queries and documents produce usable errors; extraction never freezes the UI.

### Phase 5 — Local embeddings and vector storage

**Goal:** generate durable, versioned semantic representations.

- Package a pinned model or provide an explicit download flow with license information and checksum verification.
- Apply the validated tokenizer, query instruction, pooling, and normalization consistently.
- Embed only new or changed chunks. Persist vectors and model fingerprints in SQLite.
- Batch inference with bounded memory, cancellation, and adjustable indexing resource use.
- Build HNSW using stable chunk IDs; start with documented defaults and tune only with measurements.
- Persist an index to a temporary file, validate it, then publish the completed generation. Replay changes or rebuild after mismatch/corruption.
- Keep indexing and lexical search functional when model download or inference fails.

**Done when:** embeddings survive restart, unchanged content is not repeatedly embedded, stale revisions cannot overwrite newer ones, and deleting the vector-cache file triggers recovery from SQLite.

### Phase 6 — Hybrid search and retrieval evaluation

**Goal:** natural-language queries reliably find the right files.

- Retrieve candidates from filename/path matching, FTS5 BM25, and semantic similarity.
- Fuse ranked lists with reciprocal-rank fusion; give exact filename matches explicit priority.
- Group passages by file so long documents cannot fill the result list with duplicate entries.
- Apply filters consistently across every retrieval path. Over-fetch filtered vector candidates, expanding the search or using an exact fallback for small filtered subsets when needed.
- Show a best matching passage and simple match labels such as filename, keyword, or semantic similarity.
- Cancel or ignore obsolete query work so typing does not show late results from an older query.
- Evaluate using at least 30 representative queries with known relevant files, including paraphrases, exact names, ambiguous terms, and no-good-match cases.
- Record Recall@10 and reciprocal rank alongside latency; compare against lexical search alone. Treat numerical similarity as a ranking score, not a probability of correctness.

**Done when:** the agreed evaluation set meets the recorded retrieval target, semantic search improves paraphrased queries, exact-name retrieval does not regress, and every result can be traced to indexed evidence. Pin the quality target before tuning and keep a held-out subset.

### Phase 7 — Search MVP usability and performance

**Goal:** an application suitable for daily use.

- Finish keyboard navigation, focus states, result selection, clear search, loading states, empty states, and error recovery.
- Expose indexed roots, supported formats, progress by stage, exclusions, pause/resume, and rebuild controls.
- Add visible model storage and index storage management; clearing an index must not remove original files.
- Benchmark with documented hardware and corpus sizes, including many small files and a few large documents.
- Test a packaged Release build on a machine without the development environment.

**Provisional performance targets:** warm filename search below 100 ms and warm hybrid search below 500 ms at the 95th percentile on a recorded reference machine with about 50,000 metadata records and 100,000 chunks. Measure cold model loading separately. These are engineering targets to validate, not product guarantees.

**Done when:** the UI stays responsive during indexing, measured bottlenecks are addressed or documented, and Phase 10's applicable release checks pass. This is the first public search release gate.

### Phase 8 — Related files and organization suggestions

**Goal:** propose useful relationships and destinations with clear evidence.

- Add a related-files panel using chunk similarity plus filename, directory, and project context.
- Exclude the selected file and avoid presenting semantic similarity as proof of a relationship.
- Let users create categories with descriptions, destination folders, and example files.
- Score suggestions using category examples, document passages, and simple deterministic rules. Allow abstention when evidence is weak.
- Show the proposed destination and supporting text; let users accept, reject, or edit suggestions.
- Prefer virtual tags/collections first. Never reorganize files inside source repositories, application folders, or build trees automatically.
- Implement an approved-move service that revalidates source revision and destination immediately before execution.
- Persist operation intent before touching the filesystem, then record completion. For cross-volume moves, use a tracked copy/verify/delete process with recoverable partial states.
- Add undo with identity/content checks; refuse destructive reversal if source or destination has since changed.

**Done when:** accepted moves succeed without overwrites, rejected suggestions do nothing, undo works for unchanged files, and interruption at each operation stage can be recovered or clearly explained.

### Phase 9 — Duplicate and cleanup review

**Goal:** identify redundant files without confusing similarity with safe deletion.

- Group candidate duplicates by file size, calculate SHA-256 on demand, and revalidate file state. Use a final byte comparison before treating destructive cleanup candidates as exact copies.
- Keep hard links distinct from separate copies and report realistic reclaimable storage.
- Label semantic matches as similar documents; label versions as possible versions using names, dates, and content evidence.
- Add opt-in rules for old installers and known build artifacts, with visible scope and reasons. Age alone is not proof a file is disposable.
- Present a review screen showing which file will be retained and which files are proposed for removal.
- Send approved removals to the Recycle Bin when supported. If recycling is unavailable, stop that action rather than silently permanently deleting.
- Revalidate each file immediately before cleanup; record per-file success/failure. Do not promise app-managed undo for Recycle Bin deletions unless it is implemented and verified.

**Done when:** changed files are excluded from stale cleanup decisions, retained copies are protected, and failure or cancellation cannot silently remove the remaining known copy.

### Phase 10 — Release readiness and maintenance

**Goal:** ship a reproducible, documented application.

- Package the Release build with Qt runtime dependencies, MSVC redistributable requirements, ONNX libraries, and the chosen model delivery method.
- Verify license notices and redistribution obligations for exact dependency versions, models, extractors, and installer tooling.
- Verify network behavior and configure supported telemetry opt-outs; keep document contents and queries out of analytics and default logs.
- Test install, launch, upgrade, database migration, uninstall, and a clean offline run after model availability.
- Back up application state before migrations that cannot be safely reversed; document downgrade limits.
- Keep original user files outside installer/uninstaller deletion scope. Make removal of indexes/models explicit.
- Document supported formats, resource use, model provenance, limitations, troubleshooting, and recovery steps.
- Publish benchmark methodology and retrieval results with synthetic or consented data.

**Done when:** the packaged build passes a clean-machine checklist, search works without a development environment, interrupted upgrades recover, and documented behavior matches the release.

## 6. Verification strategy

Tests should exercise outcomes and failure boundaries, not mirror internal implementation.

| Area | Required evidence |
| --- | --- |
| Scanning | Unicode/long paths, overlapping roots, links, permission errors, files disappearing mid-scan |
| Watching | Save storms, overflow/reconciliation, rename across roots, disconnected roots, restart recovery |
| Extraction | Supported documents, missing filters, malformed/encrypted/scanned PDFs, timeout and crash containment |
| Model | Tokenizer and embedding parity, padding, truncation, batch consistency, model revision isolation |
| Search | Exact names, short fragments, punctuation, phrases, paraphrases, filters, no duplicate file rows, stale result suppression |
| Persistence | Crash during text replacement, vector checkpoint mismatch, deletion propagation, migrations |
| Operations | Name collisions, changed source, partial cross-volume copy, cancellation, conflicting undo |
| Cleanup | Hash collisions handled by byte comparison, hard links, changed candidates, unavailable Recycle Bin |
| Packaging | Missing runtimes, clean install, offline model use, upgrades, uninstall preserving original files |

Keep a small deterministic fixture suite in Git. Larger performance corpora are generated locally. Developer tests must never move or delete real personal documents.

## 7. Resource and privacy considerations

- At 100,000 chunks, 384 float32 values per chunk consume about 154 MB decimal before graph edges, IDs, text, and database overhead. Persisting vectors in SQLite and HNSW adds storage duplication; measure it explicitly.
- Prefer a single loaded model session, small batches, and adjustable worker counts over launching many model instances.
- Extracted text and embeddings are sensitive local data. Ordinary SQLite is not encrypted by default. Use per-user file permissions, document this limitation, and do not imply the index is encrypted.
- Removing a root must purge its logical search records and derived vectors. Ordinary database deletion is not a secure-erasure guarantee; document backup and WAL behavior.
- Restrict default log output to stage/error metadata and redact paths where practical.
- Files and their text are untrusted input. If an LLM is added later, document text must never authorize filesystem operations or supply executable commands.

## 8. Deferred features

Revisit these only after the search MVP is measured and stable:

- OCR for scanned PDFs and images.
- Multilingual embeddings, after testing a representative multilingual corpus.
- Code-aware parsing and project-level indexing.
- Optional local LLM explanations or query rewriting through llama.cpp.
- GPU/NPU acceleration through an evaluated Windows ML path.
- A separate tray/background process for indexing while the main window is closed.
- NTFS USN journal integration for larger indexes.
- Network shares, removable-drive indexing, and explicit cloud-file hydration support.
- Whole-drive search, Explorer extensions, and automatic update delivery.

## 9. Working process

1. Start each phase by reviewing its acceptance criteria and prerequisite decisions.
2. Implement a small vertical slice that can be exercised through the application or a focused integration test.
3. Run checks appropriate to changed behavior and record concrete results.
4. Update the phase checklist and architecture notes when decisions change.
5. Keep known limitations visible; do not substitute mock AI results for a completed embedding integration.
6. Advance when the phase gate passes. Resolve failed feasibility checks before building dependent functionality.

### Progress tracker

- [ ] Phase 0: Buildable skeleton
- [x] Phase 1: Feasibility checks and pinned integration decisions (local validation)
- [x] Phase 2: Metadata index and ordinary search (local validation)
- [x] Phase 3: Incremental indexing (root-level reconciliation; local validation)
- [x] Phase 4: Extraction and keyword search (local validation)
- [x] Phase 5: Local embeddings and vector storage (local validation)
- [x] Phase 6: Hybrid search and evaluation (local validation)
- [ ] Phase 7: Search MVP usability and performance (local implementation validated; independent-machine/public-release gates pending)
- [ ] Phase 8: Related files and organization
- [ ] Phase 9: Duplicate and cleanup review
- [ ] Phase 10: Release readiness

Phase 1 validates real C++ BGE embeddings against PyTorch, HNSW behavior, bundled
PDF/DOCX extraction, and parser isolation/resource limits. Phase 2 adds selected-folder
metadata indexing, persistent filename/path search, filters, cancellation, and Windows
open/reveal actions. Phase 3 adds recursive Windows notifications, a durable work
queue, startup/periodic recovery, bounded retries, and saved pause/resume.
Updates reconcile affected roots; file-level optimization remains future work.
See [Phase 2 notes](docs/PHASE_2_METADATA.md) and [Phase 3 notes](docs/PHASE_3_INCREMENTAL.md).
Phase 4 adds bounded local readers, transactional text revisions, FTS5/BM25 keyword
search, exact phrases, snippets, and plain-text previews. See [Phase 4 notes](docs/PHASE_4_CONTENT_SEARCH.md).
Phase 5 adds checksum-verified local BGE inference, exact-token partitions, durable
model-specific SQLite vectors, adjustable/cancellable background batches, and
generation-validated HNSW cache recovery. See [Phase 5 notes](docs/PHASE_5_EMBEDDINGS.md).
Phase 6 adds natural-language queries, weighted reciprocal-rank fusion, exact-name
priority, file-level grouping, shared filters, matching evidence, cancellation and
lexical fallback. The fixed 46-query evaluation meets the original quality targets
on the full and held-out sets. See [Phase 6 notes](docs/PHASE_6_HYBRID_SEARCH.md).
Phase 7 adds keyboard navigation, stable selection, compact-window layout, storage
management, persistent query inference/graph reuse, large-library measurements and
portable validation tooling. Filename p95 meets the provisional target; hybrid p95
remains slightly above it on the recorded workload. See [Phase 7 notes](docs/PHASE_7_USABILITY_PERFORMANCE.md).
Independent-machine deployment and the distribution notice/source/license checklist
remain open for the first public search release gate. Phase 8 has not started.
User checkpoint (2026-11-18): finish Phase 7 only; do not start Phase 8 without
a new instruction. Keep the Phase 7 release gate open until its outstanding checks pass.

## 10. Primary references

- [Qt C++ models with QML](https://doc.qt.io/qt-6/qtquick-modelviewsdata-cppmodels.html)
- [Qt Windows deployment](https://doc.qt.io/qt-6/windows-deployment.html)
- [Qt open-source obligations](https://www.qt.io/development/open-source-lgpl-obligations)
- [vcpkg manifest mode](https://learn.microsoft.com/en-us/vcpkg/users/manifests)
- [SQLite FTS5](https://www.sqlite.org/fts5.html)
- [ONNX Runtime C++](https://onnxruntime.ai/docs/get-started/with-cpp.html)
- [ONNX Runtime Extensions](https://onnxruntime.ai/docs/extensions/)
- [BGE-small-en-v1.5 model card](https://huggingface.co/BAAI/bge-small-en-v1.5)
- [hnswlib](https://github.com/nmslib/hnswlib)
- [Windows IFilter extraction](https://learn.microsoft.com/en-us/windows/win32/search/-search-ifilter-about)
- [Windows directory notifications](https://learn.microsoft.com/en-us/windows/win32/fileio/obtaining-directory-change-notifications)

These references support the chosen direction. Recheck version-specific APIs, model artifacts, and license terms when pinning dependencies and before distribution.
