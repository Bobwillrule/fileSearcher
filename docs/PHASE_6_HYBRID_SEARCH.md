# Phase 6: natural-language and hybrid file search

The default **AI + keywords** mode accepts descriptions as well as names. It
combines filename/path matches, SQLite FTS5 BM25 keywords and local BGE semantic
similarity. Existing literal word, phrase, filename and advanced FTS modes remain
available. Search does not send text or queries to a service.

## Retrieval policy

Each channel contributes up to 200 distinct files. Filename/path matching treats
wildcards literally and prioritizes exact names. Hybrid keyword search uses a
bounded OR of up to 64 Unicode word tokens after removing a fixed set of common
English request/stop words. This differs deliberately from the existing
**Content: all words** mode. Advanced query operators are only interpreted in
**Advanced content syntax**.

The pinned BGE query instruction is applied by the same verified worker used for
indexing. Semantic matches below cosine 0.60 are omitted. This is a conservative
ranking heuristic, not a calibrated confidence probability. Results above the
floor can still be irrelevant; inspect the passage and location shown.

Each channel groups passages by file before fusion. The semantic channel keeps
the highest-scoring span per file. Weighted reciprocal-rank fusion sums
`weight / (60 + rank)` using weights 1 for names, 1 for keywords and 2 for meaning.
The semantic weight was selected on the development set before opening held-out
results. Exact filename or exact full-path matches precede fused ranking; ties
are deterministic by file ID. Output is capped at 200 ranked files, explicitly
labelled as ranked candidates rather than an exhaustive match count.

Each result identifies its matching channels. Keyword/semantic results carry a
live passage ID, page/line location and plain-text evidence for the preview.
Where both match, the best semantic span is shown. Scores are not displayed as
percentages. A matching filename can appear without extracted content.

## Filters, cache use and changing files

All hybrid channels share one metadata predicate for extension (case-insensitive,
with optional leading dot), indexed root, minimum/maximum bytes and modification
date. Root membership uses EXISTS so overlapping roots do not duplicate results.

Up to 2,048 eligible chunks are scored exactly from SQLite. Larger sets use the
read-only HNSW cache when its model, generation, count, checksum and bounded graph
format validate. Candidates expand from 64 to 8,192 when duplicates or filters
leave too few files. Missing/stale/corrupt caches, graph exhaustion or insufficient
distinct results fall back to a streaming exact scan. This also handles graph
connectivity problems caused by thousands of identical vectors. Memory retains
at most 200 best semantic file records; SQLite vectors are streamed.

Every HNSW label is resolved through current model-bound chunks, passages, files
and filters; a cache is never used as evidence on its own. SQLite generation and
data-version checks discard semantic results if writes occur during retrieval,
refresh lexical results and show a retry notice. File/root changes also schedule
a fresh controller search. IDs are never recycled. Query code does not mutate or
rebuild cache files, avoiding contention with the indexing worker's publication.

## Cancellation and fallback

Every new request cancels the previous query token. Queued obsolete work skips
execution; an active inference child is interrupted. Only the current request
generation may update the UI, including notices and errors. Closing the app
cancels queries before joining workers. Filter validation and disabling AI also
cancel old requests. Lexical modes do not start an inference process.

Missing model/runtime files, failed inference, disabled AI or a semantic-index
failure retain filename/keyword results with an explicit notice. No ready vectors
for the selected filters is distinguished from a successful search with no strong
semantic match. Empty queries browse the metadata index without embedding.

## Evaluation and limitations

The checked-in fixture contains 30 synthetic documents and 46 labelled queries:
30 paraphrases, 8 exact names, 4 ambiguous requests and 4 absent subjects. There
are 23 development and 23 held-out queries. The evaluator uses the actual worker,
production ranking and filters, and checks both partitions separately plus the
aggregate. Four sampled production queries are checked against harness ranking.

See [the evaluation protocol](PHASE_6_EVALUATION_PROTOCOL.md) for the original
targets, the development-only feasibility concern, and the rule freezing
parameters before held-out scoring. The original absolute-gain target was met
on both required acceptance populations: full set and held-out set. No lower
acceptance target was needed. Original development scores are retained.
No fixture labels or split memberships were changed after scoring.

The executable writes full per-query rankings and metrics to
`build/<preset>/retrieval-evaluation.json`. These small synthetic fixtures do not
establish quality on an arbitrary personal collection. The model and stop words
are English-oriented. OCR and unsupported formats remain outside current scope.

The current desktop creates a bounded inference process per semantic request;
it closes before graph loading. Cold query latency includes model loading and
checksum verification. The evaluator separately measures a reused-worker harness
and retrieval-only latency; those faster measurements are not presented as
desktop latency. Persistent inference, large-corpus latency/memory benchmarks,
further UX work and independent-machine deployment belong to phase 7.

## Recorded results — 2026-11-18

Release evaluation on Intel Core Ultra 7 258V (8 logical processors), CPU inference,
the pinned BGE/ORT stack and this 30-document fixture:

| Metric | Lexical, full | Hybrid, full | Lexical, held-out | Hybrid, held-out |
| --- | ---: | ---: | ---: | ---: |
| Recall@10 | 89.88% | 98.21% | 82.14% | 96.43% |
| Mean reciprocal rank | 84.13% | 97.62% | 77.38% | 97.62% |
| Paraphrase MRR | 82.78% | 98.33% | 75.00% | 96.67% |
| Exact filename top-1 | 100% | 100% | 100% | 100% |
| Negative queries returning no results | 100% | 100% | 100% | 100% |

The original paraphrase gain is +15.56 percentage points overall and +21.67 on
held-out queries. Ranking-error reduction is 90.32% overall and 86.67% held-out.
The development subset improves from 90.56% to 100% paraphrase MRR; it cannot
achieve an absolute 15-point gain because its lexical baseline is already high.
The final evaluator enforces original absolute-gain requirements on the full and
held-out sets and additionally enforces relative-error improvement on each split.
The user selected at least 15% ranking-error reduction as the acceptance target;
both measured reductions exceed it.

Recorded Release latency (median / p95): lexical 1.33 / 2.60 ms; hybrid retrieval
with a precomputed query vector 1.93 / 3.12 ms; reused-worker evaluation harness
9.69 / 12.61 ms. Actual desktop-path queries including fresh model startup:
1,264 / 1,299 ms, from four samples. This small cold-query sample is descriptive,
not a performance guarantee. Every desktop semantic query currently reloads the
model, so do not interpret the harness figures as shipped warm-search latency.

The full Debug and Release suites each passed 19 CTest entries, including unchanged
phase 1–5 coverage and the new ranking/filter and quality checks. The final added
HNSW coverage and original acceptance gates also passed the scoped Release checks.
Reports are retained under [evaluation](evaluation/phase6-release.json), alongside
the historical development results and Debug report. The offscreen synthetic
preview showed a paraphrased project query, source labels and matching text.

## Reproduce

```powershell
.\tools\build.ps1 -Preset windows-release -ModelTests
.\tools\build.ps1 -Preset windows-debug -ModelTests
# The build helper adds Qt to PATH for this shell.
.\build\windows-release\retrieval_eval.exe tests/fixtures/hybrid_retrieval.json models/bge-small-en-v1.5 heldout build/windows-release/retrieval-heldout.json
```

`hybrid_ranking_filters` exercises exact-name priority, bounded file grouping,
evidence, every filter, literal query syntax, HNSW/SQLite fallback, deleted content,
low similarity and cancellation. `desktop_embedding_workflow` now also covers
real paraphrase search, matching preview text, rapid query replacement, filters
and switching AI off. The screenshot helper waits for indexing, search and preview
to settle before capture, with a 60-second deadline.
