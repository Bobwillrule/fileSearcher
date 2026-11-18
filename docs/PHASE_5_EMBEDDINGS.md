# Phase 5: local embeddings and durable vector storage

Phase 5 adds background embedding generation. Search still uses filenames and
FTS5; natural-language retrieval, ranking fusion, filters and quality evaluation
belong to phase 6. No account, paid API, GPU, Python runtime or network request is
needed during indexing.

## Model and packaging

The existing setup helper explicitly downloads BAAI/bge-small-en-v1.5 at revision
`5c38ec7c405ec4b44b94cc5a9bb96e735b38267a`. The full asset hashes remain in
`tools/model-assets.lock.json`. When prepared assets are present, CMake copies
the ONNX file, vocabulary, model card, lock manifest and MIT license into
`build/<preset>/models/bge-small-en-v1.5/`. It also builds `embedding_worker.exe`
and copies ONNX Runtime 1.23.2 beside it. The app itself does not link ONNX Runtime.
An installation without ORT can still build and run lexical search; model tests
require the runtime and prepared model.

Both the indexing service and worker verify the ONNX, vocabulary and model card
against compiled SHA-256 values. The supplied manifest is documentation, not an
authority that can change which model is trusted. A missing/damaged model or
missing/crashed worker produces an AI status message while lexical search works.
Repair by running `tools/setup-phase1.ps1` and rebuilding. No automatic download
occurs when the app opens.

The pinned model card declares MIT and links the upstream
[FlagEmbedding license](https://github.com/FlagOpen/FlagEmbedding/blob/master/LICENSE).
The accompanying notice is preserved in `docs/licenses/BGE-MIT.txt` and copied
beside the model. These are local development build artifacts; complete third-party
notices and independent-machine deployment remain release gates.

## Transformations and identity

The production engine and parity executable share the same implementation:
uncased BERT WordPiece, the validated BGE query instruction, attention-mask
padding, CLS pooling, 384 dimensions, and L2 normalization. Documents receive no
query instruction. Parity compares single and padded batch inference with the
independent PyTorch references from phase 1.

Lexical passages are subdivided into contiguous spans with at most 384 content
tokens (386 including CLS/SEP), counted by the exact tokenizer without truncation.
Splitting prefers nearby whitespace, preserves UTF-16 character boundaries and
records offsets into the original passage. The existing lexical passage overlap
remains; these additional splits do not add overlap. No input is silently
truncated by the worker, including future query requests.

The fingerprint hashes the model revision, ONNX/vocabulary hashes, tokenizer and
partition versions, instruction, pooling, normalization, dimensions and runtime
version. Changing a transformation requires a new fingerprint. Vectors from
different fingerprints never share an index.

## SQLite is authoritative

Schema version 3 adds `embedding_models` and `embedding_chunks` to metadata.sqlite.
Chunk IDs are AUTOINCREMENT integers. Rows bind a model, passage revision, span,
source SHA-256 and token count to a nullable vector. Vectors are 384 little-endian
float32 values, checked for size, finiteness and unit norm on read/write.

Preparation is transactional and verifies the passage still exists with the same
text. Pending rows form the durable embedding queue. Each completed batch is
committed atomically with ID/model/source-hash guards, and only fills empty vector
slots. Changed documents delete their old passages and cascade into old vectors;
late inference cannot recreate deleted IDs. Unchanged scans preserve passage IDs
and do not rerun inference. Removing a root deletes vectors for files with no
remaining indexed root. Model generation counters advance on vector writes and
deletions, including cascading deletions. Old model rows are retained until a
future explicit maintenance policy removes them.

## Scheduling, limits and controls

The root indexing worker performs metadata and text work first, publishes lexical
results, then prepares embeddings. Inference runs in one child process for that
pass, reused for batches and closed before cache construction. Text travels over
bounded local pipes, without staging documents on disk or logging their contents.

| Resource setting | CPU inference threads | Batch size |
| --- | ---: | ---: |
| Low | 1 | 1 |
| Balanced (default) | 2 | 4 |
| Faster | 4 | 8 |

Preparation reads at most 32 passages per page. Worker input is capped at 128 KiB,
responses at 256 KiB, and each item at 4,000 UTF-16 units / 512 tokens. A Windows
job caps inference process committed memory at 1 GiB and forbids child processes.
ORT memory arenas/pattern caching are disabled. Startup and each batch have a
60-second deadline, checked at short intervals. Pause, shutdown, disable or a
profile change interrupts inference and preserves committed batches. Fresh filesystem
events for the active root also interrupt its AI pass so metadata changes run next. Local AI
settings persist in the index directory. Failed AI jobs do not count as metadata
failures; the next rescan, restart or periodic reconciliation retries pending work.

## HNSW cache and recovery

HNSW uses cosine distance through normalized inner products, stable SQLite chunk
IDs, M=16, efConstruction=100, efSearch=64 and seed 42. These are initial defaults,
not measured retrieval-quality claims. The cache is currently rebuilt on any
vector generation change rather than incrementally patched.

`vectors/current.json` names a generation-specific graph payload, its SHA-256,
model fingerprint, generation and count. A rebuild streams vectors from SQLite,
writes a temporary graph, reloads and validates it, renames the complete payload,
then atomically replaces the manifest with QSaveFile. Only after publication are
obsolete app-owned payloads reclaimed. An interrupted publication leaves either
the old manifest or an unreferenced file; recovery checks SQLite again.

The `FSHNSW01` wrapper format stores node IDs, vectors, deletion flags, levels and
adjacency. It supports Unicode filesystem paths without the library's narrow
filename loader. The reader checks dimensions, counts, norms, duplicate labels,
levels, degrees, edge bounds, neighbor levels, entry point and trailing data before
swapping live state. It never trusts serialized allocation sizes. The format pins
the adapter's HNSW defaults; changing defaults/layout requires a new format version.
The cache supports up to 100,000 nodes, a 512 MiB file ceiling, and a 256 MiB vector
payload ceiling. Larger corpora retain SQLite vectors and lexical search but show
an AI cache limit message. Large-corpus tuning belongs to later performance work.

Absent, damaged, stale-generation or mismatched-model caches rebuild from SQLite
without re-embedding. Readers must compare the returned generation to SQLite and
resolve every result against live passage rows; phase 6 will apply this at query
time. SQLite corruption is not repaired by the cache layer.

## Validation

Automated coverage includes real model parity, bounded exact-token partitioning,
durable batches, unchanged-content reuse, model isolation, rejection of stale
results and invalid vectors, root deletion, Unicode cache paths, cache removal and
corruption, cancelled inference, missing-worker/model fallback, and query embedding
through the production worker. The full Debug and Release suites passed all 17 CTest entries
on 2026-11-17. The shared production engine matched all 21 independent PyTorch
reference cases within 1e-4 (observed maximum absolute error 3.57628e-7).
The desktop integration test checks persisted AI preferences and reuse after an
actual controller restart. A synthetic offscreen UI preview was inspected, and
the final layout/status copy changes were rechecked with startup and desktop AI tests.
The UI executable's imports were checked to confirm it has no ONNX Runtime DLL dependency.
Independent-machine packaging validation remains open.
