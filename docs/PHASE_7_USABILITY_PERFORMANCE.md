# Phase 7 — Search MVP usability and performance

Local implementation and validation, 2026-11-18. The public-release gate is still open: this environment has no Windows Sandbox installation or separate clean Windows machine, and the distribution checklist is not complete. See [package validation](PACKAGE_VALIDATION.md). Phase 8 is not started.

## Delivered behavior

- Ctrl+F focuses/selects the query; Escape clears it from the search field or returns focus from results. Down/Up, Home/End and Page Down/Page Up navigate results. Enter opens the selected file; F5 rescans. Enter cancels the pending debounce to avoid duplicate requests.
- Result selection follows stable file identity across refreshed results. Keyboard focus is visible. Clear search and Clear filters are separate actions. Existing stage progress, pending work, pause/resume, per-root exclusions, loading/no-results messages and recoverable keyword fallback remain visible.
- The compact layout preserves at least 120 pixels of results at 1000×650. Long folder paths are capped at two lines with a full-path tooltip so their action buttons remain reachable. Storage and format help fits at minimum size.
- Storage & help shows counts, paths, index/cache and model sizes, supported formats/limits, Open folder actions, Release AI memory, Rebuild AI data and Clear index confirmation. Full clear removes selected roots and derived data; AI-only clear retains text and roots. Both run off the UI thread and preserve original files/model assets. Full clear releases SQLite free pages with VACUUM; this is not secure erasure. File IDs remain monotonic. Rebuild respects saved pause state.
- Cache clearing removes only the app's manifest and generated graph filenames in its cache directory; linked/junction cache directories are left untouched. There is no recursive source-file deletion.

## Persistent inference and search costs

A dedicated query thread owns a persistent SearchSession, child embedding process and validated HNSW graph. New queries rebind cancellation safely; obsolete responses cannot replace current results. Cancellation/failure during inference drops the worker and the next query starts a fresh one. AI-off, manual memory release and shutdown close it on its owning thread. Model asset size/mtime changes reload the worker (startup still checks the pinned hashes). Graph generation changes invalidate the retained graph. Each query resolves candidates through live SQL filters; no query/result cache is retained.

Indexing still uses a separate bounded worker. Query and indexing inference can overlap, so total memory can exceed the isolated query measurement below. The query model remains resident until released/disabled/exit; there is no idle timeout yet.

The readiness probe now stops at the first eligible vector rather than counting all vectors twice. A partial index covers ready vectors. Keyword retrieval ranks all hits but generates snippets only for the 200 winning files. Ranking/filter semantics are unchanged. All 46 quality queries compare the production persistent session against the reference retrieval path; full/held-out quality remains at Phase 6 levels. The user's accepted target of at least 15% ranking-error reduction remains satisfied.

## Reference retrieval benchmark

Windows 11 25H2 build 26200.9974; Intel Core Ultra 7 258V; 8 logical processors; 33,840,226,304 bytes physical RAM; local workspace drive (drive model not measured). MSVC 14.44 x64 Release, Qt 6.8.3, ONNX Runtime 1.23.2, CPU inference with two threads.

50,000 metadata records and 100,000 vector chunks: 49,990 one-passage files plus ten 5,001-passage documents. The corpus is written directly to SQLite. It uses eight real BGE embeddings with deterministic normalized perturbations; this measures retrieval/load cost, **not** full-corpus embedding throughput or semantic quality. Quality uses the separate real-text evaluation. Each warm category has 24 samples; p95 is the nearest-rank percentile. Queries are sequential, with no simultaneous indexing/build in this measurement; normal workstation activity is uncontrolled.

| Measurement | Median | p95 | Provisional target |
| --- | ---: | ---: | --- |
| Warm filename | 41.7 ms | 55.5 ms | <100 ms: met |
| Warm hybrid | 371.3 ms | 533.5 ms | <500 ms: missed by 33.5 ms |
| Warm hybrid, size filter | 363.9 ms | 519.9 ms | Reported separately |

First model-and-graph-cold query: 3,964 ms (not a reboot/OS-cache-cold test). Graph build: 45.3 s. All 24 unfiltered semantic searches used HNSW; one query worker served the run. Database: 270,438,400 bytes; graph files: 164,818,273 bytes. Query harness resident set: 221,220,864 bytes; worker: 180,748,288 bytes. Peak process resident sets were approximately 390 MB and 221 MB respectively (separate peaks, not a simultaneous total).

The initial persistent-session benchmark had hybrid p95 654.3 ms. Removing redundant counts and eager snippet creation reduced it to 533.5 ms. The remaining workload includes broad keyword hit ranking and filtered vector counts/joins; further profiling is needed before assigning the residual cost to one component. The target has not been relaxed and this miss is not presented as a pass. Cold graph validation/model startup remain visible costs. Graph capacity remains 100,000 vectors; larger indexes use the existing fallback and are not covered by this latency claim.

Raw data: [performance](evaluation/phase7-performance-release.json), [quality](evaluation/phase7-quality-release.json).

Reproduce after a Release build with `-ModelTests`:

```powershell
$env:PATH="$PWD\.tools\qt\6.8.3\msvc2022_64\bin;$env:PATH"
.\build\windows-release\search_benchmark.exe models/bge-small-en-v1.5 build/windows-release/phase7-benchmark.json
```

## Validation scope

All **20/20 CTest checks pass in both Debug and Release** (including the real-model tests). Release took 72.9 seconds; Debug took 126.0 seconds. These are test-suite elapsed times, not benchmark latency claims.

The packaged small-corpus workflow passed with a sanitized developer environment: text/PDF/DOCX extraction, semantic evidence across repeated queries, restart and clear with the source bytes preserved. Initial indexing took 2,781 ms; the UI heartbeat ran 249 times with a maximum 191 ms gap. This includes initialization and indexing, not just steady-state query service. See [package smoke data](evaluation/phase7-package-smoke.json).

The automated suite covers keyboard/focus/selection, screenshots at default/minimum sizes, safe full/AI-only clearing and re-adding a root, saved state, worker reuse/crash/retry/release, index/cache generation/filtering, cancellation, real embedding parity, document extraction and full/held-out retrieval quality. The storage workflow checks original bytes after clearing. The UI test records screenshots under each build's `ui-checks` directory.

The portable package includes app/parser/AI executables, Qt/QML/SQLite runtime, the offscreen plugin required by document workers, ONNX Runtime, libzip/zlib, pinned model, an official Microsoft runtime installer, available dependency notices/SBOMs, synthetic parser fixtures, source snapshot and SHA-256 manifest. Its verifier uses a temporary working directory and removes developer Qt/PATH overrides. No Python or compiler is required to run it.

The packaged `--scale` run passed: **1,001 physical small text files and four 290,000-byte text documents**, through real scanning/extraction/storage with AI indexing disabled. It produced 1,545 passages in **110,758 ms**. The event-loop heartbeat ran 9,994 times; the longest observed gap was **162 ms**. Restart and clearing the index preserved the original source bytes. This is a responsiveness proxy, not a frame-rate measurement. See [physical-file results](evaluation/phase7-physical-files.json).

Document extraction currently launches a bounded reader process for each file; the 111-second indexing run makes that startup cost an obvious next profiling candidate. Persistent/batched parser workers are not introduced here because their cancellation, crash recovery and resource boundaries need separate validation. The physical-file run is separate from the 50k/100k retrieval benchmark and does not measure AI embedding throughput.

Clean-machine offline startup, real DPI/Shell integration, upgrade/removal and final distribution obligations require the remaining release checks; local offscreen checks do not establish them. The runtime package passed locally with only Windows system directories in PATH. Developer-machine runtime availability still prevents a claim of independent-machine compatibility.

## Final local candidate

`build/FileSearcher-phase7-local.zip` is the final local validation archive (142,408,669 bytes). Its SHA-256 is recorded in [artifact details](evaluation/phase7-package-artifact.json). The final package passed manifest verification, document/semantic/restart/clear checks and UI startup with developer paths removed. The final verifier also supplies Windows fonts to the offscreen test. See [final package check](evaluation/phase7-package-smoke-final.json). The packaged application binaries match those used in the physical-file run; only packaging instructions and verification font setup changed afterward.

The archive contains a source snapshot taken before this final-results addendum. The original text-document size estimate in that snapshot was approximate; the exact size is recorded above. As requested, stop after Phase 7; Phase 8 has not started and needs a new instruction.
