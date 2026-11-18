# Phase 1 feasibility record

Status: technical feasibility gates implemented and locally validated.
Evidence date: November 10, 2026. The desktop remains a shell; folder indexing and
UI search belong to subsequent phases.

## Decisions established

| Area | Decision | Evidence |
| --- | --- | --- |
| Model | BAAI/bge-small-en-v1.5, float32 CPU ONNX | Native output matches independent PyTorch reference |
| Tokenizer | Model-specific C++ uncased BERT WordPiece | Exact token IDs for all 21 reference cases |
| Pooling | CLS / first token, then L2 normalization | Matches the model author's recipe |
| Query prefix | `Represent this sentence for searching relevant passages: ` | Applied only to queries |
| Runtime | Official ONNX Runtime 1.23.2 Windows x64 | SHA-256 verified archive, native inference |
| Vector index | hnswlib 0.9.0 | Persistence/deletion tests and real embedding retrieval |
| PDF | Qt PDF 6.8.3, bundling PDFium | Text, scanned, encrypted, blank, malformed fixtures |
| DOCX | libzip 1.11.4 and QXmlStreamReader | Body text without installed Office filters |
| Parser containment | Separate process, timeout, 256 MiB committed-memory Job Object limit, one process | Crash, timeout, denied 512 MiB allocation, and recovery verified |

IFilter could not extract PDF/DOCX fixtures on this machine. The bundled worker
resolves that dependency gap; IFilter remains an optional diagnostic.

The C++ tokenizer is deliberately specific to the locked BGE configuration.
ONNX Runtime Extensions was a candidate; the validated small BERT implementation
avoids another runtime dependency. It is not a general Hugging Face tokenizer.
Changing models requires revalidating the tokenizer. These cases do not prove
equivalence for every possible Unicode input.

## Reproducible assets

- MSVC Build Tools 2022 17.14.44; compiler 19.44.35229.0; Windows x64.
- Qt 6.8.3 MSVC x64 including Qt PDF, dynamically linked.
- vcpkg baseline `19e7a138440f005fea23d92b19df87f8040e66c1`.
- hnswlib 0.9.0, Catch2 3.16.1, libzip 1.11.4, patched zlib 1.3.2.
- ONNX Runtime 1.23.2 archive SHA-256:
  `0b38df9af21834e41e73d602d90db5cb06dbd1ca618948b8f1d66d607ac9f3cd`.
- BGE revision `5c38ec7c405ec4b44b94cc5a9bb96e735b38267a`.
- Eight model/tokenizer assets are locked in `tools/model-assets.lock.json`.
  Both preparation and native validation reject mismatching hashes.
- Reference: PyTorch 2.9.1, Transformers 4.57.3, eager attention, evaluation mode,
  CPU. Development requirements: `tools/requirements-phase1.txt`.

Assets and tools remain in ignored `models/` and `.tools/` folders. Python
prepares references and fixtures; native inference/extraction never launches it.

## Model evidence

The 21 cases contain five documents, seven queries, and nine edge cases.
Edges cover empty/whitespace text, accents, CJK, emoji, control characters,
special tokens, overlong words, and truncation beyond 512 tokens.

- All token IDs match exactly.
- All normalized embeddings have 384 dimensions.
- Maximum observed absolute difference from PyTorch: **3.57628e-7**.
- Acceptance tolerance, set before testing: **1e-4** per component.
- Every case runs singly and in mixed-length batches of up to four; padding
  must not change its embedding beyond that tolerance.
- HNSW searches over actual model vectors return the intended document first
  for **5/5** paraphrased semantic queries.
- The unrelated cake-recipe query's nearest similarity is approximately
  **0.456806**. This is an observed negative example, not a calibrated threshold.
- The filename query participates in parity checks; filename ranking belongs
  to later metadata/hybrid search.

This is a feasibility smoke set, not a production retrieval benchmark. Broader
held-out evaluation and an abstention policy remain Phase 6 work.

## Document evidence

| Synthetic input | Expected and observed result |
| --- | --- |
| Text PDF | `ok`, contains “Master Theorem” |
| DOCX | `ok`, contains “Dynamic programming” |
| Encrypted PDF | `password_protected` |
| Image-only/scanned PDF | `no_text_or_ocr_required` |
| Blank PDF | `no_text_or_ocr_required` |
| Malformed PDF | `invalid_document` |
| Malformed DOCX | `invalid_document` |
| DOCX with 9 MiB uncompressed XML | `xml_limit` |
| Unknown extension | `unsupported_format` |
| Missing file | `missing_file` |

The Windows text-filter test independently checks actual TXT contents.
No personal files were used. OCR is not implemented; blank and scanned PDFs
share a conservative no-text status.

DOCX support reads `word/document.xml` only, including paragraphs/tables. It
does not unpack entries to disk, execute macros, or follow external relationships.
Headers, footnotes, comments, drawings, embedded objects, and encrypted Office
containers are not yet supported. Limits: 32 MiB input, 8 MiB XML, one million
UTF-16 text units, 10,000 PDF pages. DTDs/entity references are rejected.

## Acceptance and verification

The ten-test CTest gate contains four HNSW cases, worker isolation/resource limits,
fixture generation, bundled-document coverage, Windows text extraction, BGE
parity/retrieval, and QML startup. Debug and Release pass locally.
The `windows-clean` preset provides a fresh, separate Release build directory.

The documented setup script has been rerun successfully. Windows CI is configured
to run setup and all ten tests; no remote run has been initiated or claimed.
A fresh local build is **not** independent-machine deployment testing. That
previously listed environment check remains an open Phase 0/10 packaging item,
not a passed test in this feasibility report.

- [x] Pin and verify model/tokenizer/runtime assets.
- [x] Run BGE in C++ and compare tokenization, CLS pooling, normalization, prefix.
- [x] Check Unicode, empty input, batching, padding, and truncation parity.
- [x] Run retrieval smoke queries over real model vectors.
- [x] Verify HNSW persistence, IDs, updates, and deletion behavior.
- [x] Test text, encrypted, scanned, malformed, and unsupported documents.
- [x] Establish a PDF route independent of installed filters.
- [x] Test parser crash, timeout, memory limit, and recovery.
- [x] Record versions, architecture decisions, evidence, and limitations.

## Remaining product limitations

- Search/indexing are not connected to the UI. Probe APIs are blocking.
- Job Objects provide resource containment, not a full security sandbox.
- HNSW caches remain trusted developer caches with fixed dimensions.
  Versioned/atomic checkpoints, Unicode paths, and corruption hardening are Phase 5.
- The model runner is a feasibility executable, not yet a production embedding
  service with cancellation, scheduling, and model-management UI.
- An upstream hnswlib C4701 warning occurs in deleted-slot replacement code.
  This adapter does not enable that feature.
- Distribution still requires notices, Qt source/relinking compliance, runtime
  packaging, and testing on a separate clean Windows machine.

## License decision and sources

Qt PDF offers LGPLv3/GPLv2 alongside commercial licensing and bundles PDFium and
other dependencies. Use the LGPL route consistently with the shell and retain
distribution obligations. libzip is BSD-3-Clause; ONNX Runtime and BGE are MIT.
No paid service is introduced.

- [BGE and reference pooling recipe](https://huggingface.co/BAAI/bge-small-en-v1.5)
- [Qt 6.8 PDF licensing](https://doc.qt.io/qt-6.8/qtpdf-licensing.html)
- [ONNX Runtime 1.23.2](https://github.com/microsoft/onnxruntime/releases/tag/v1.23.2)
- [libzip](https://libzip.org/)
