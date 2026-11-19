# Phase 9 — Duplicate and cleanup review

Implementation date: 2026-11-19. This phase adds explicit, per-file recycling reviews. Phase 10 and the independent-machine/public-distribution gate are not completed by this work.

## Using the feature

Open **Duplicates & cleanup** in the left sidebar. Choose one indexed folder, then **Scan folder**. The scan uses that folder's indexed membership and exclusions, never an implicit whole-drive traversal. Hashing is on demand and cancellable. No suggestion is selected or recycled automatically.

- **Exact duplicates:** same-size candidates are hashed with SHA-256 and compared byte for byte. Each row names a retained copy and a proposed recycle path. **Keep this copy instead…** reverses those roles before review. The confirmation checks both files again and requires **Send to Recycle Bin**. Cancel leaves both files in place.
- **Similar documents / possible versions:** inspect the selected search result. Existing BGE passage similarities provide semantic suggestions; filename/folder context remains available with AI off. Names with version/copy/final/draft suffixes can be labeled possible versions. Dates and indexed passages are context, not evidence that a particular revision is disposable. These rows only offer Open, never recycling.
- **Old installers:** off by default. The rule requires an `.msi`, or an `.exe` name containing `setup`/`installer`, and a modification date more than 180 days old. A generic old file or executable does not qualify. The confirmation explicitly states that no backup copy was verified.
- **Generated build artifacts:** off by default. The rule covers `.obj`, `.pch`, `.ilk`, `.tlog` and `CMakeCache.txt` inside a path component named `build`, `CMakeFiles`, `Debug` or `Release`. It does not recursively remove directories or select source code. The user must verify that a file is rebuildable.

Only one reviewed file is recycled per approval. After success the candidate list is cleared and indexed roots are refreshed (subject to the existing pause setting). Scan again for current suggestions. Changing or removing a retained file makes dependent stale reviews fail; the service does not silently choose a substitute retained copy.

## Checks before changing files

Cleanup accepts ordinary local drive paths and pins their ancestor directories. Linked/reparse/cloud-placeholder paths, alternate data streams, hard links, read-only and encrypted files are excluded. Application, index, system, dependency and VCS-internal folders are protected. Ordinary repository/build cleanup is refused; only an explicitly enabled, recognized generated-build rule can operate in build trees. This conservative stream rule also excludes many downloads carrying `Zone.Identifier` metadata.

Snapshots record volume/file identity, creation and last-write timestamps, primary-stream size, SHA-256 and allocated bytes. The review and execution each reopen and validate the candidate. Exact-duplicate execution also reopens, hashes and byte-compares the retained copy. The retained handle disallows writes and deletion until the Shell operation finishes. The candidate handle disallows writes while allowing the Shell's recycle operation; the Shell callback reopens the original name to detect ordinary replacement/rename races. Linked or altered candidates stop the operation.

The guarantee is protection of the held, verified retained copy during duplicate recycling. This is not a security boundary against a malicious process with equivalent privileges, privileged filesystem manipulation or arbitrary disk failure. Rules without a retained copy require the user's explicit disposable-file judgment and depend on Windows recycling for restoration.

Hard-link aliases never count as independent copies or reclaimable bytes. The displayed value is the filesystem's reported allocated size for that proposed candidate, not a promise of physical space recovered. Resident/compressed/sparse files, filesystem metadata and storage deduplication can affect allocation. **Recycling alone does not free disk space**: the Recycle Bin still holds the file. The app never empties it.

## Recycle-only Windows operation

The worker initializes a COM single-threaded apartment and uses `IFileOperation` with `FOFX_RECYCLEONDELETE`, early failure, no error/progress dialogs, no connected-item expansion and no recursive directory operation. An `IFileOperationProgressSink` checks every `PreDeleteItem`: if `TSF_DELETE_RECYCLE_IF_POSSIBLE` is absent, it aborts. It also checks cancellation and reruns identity/content validation at that callback. There is no permanent-delete implementation or fallback.

Completion requires a successful per-file callback, no aborted operation, an absent original path and a returned Recycle Bin item path. If Windows cannot confirm that outcome, the history requests review instead of reporting success or retrying. Restore through **Windows Recycle Bin**. No app-managed cleanup undo is promised.

## Durable history and interruption

An additive `cleanup_operations` table in the existing version-1 `organization.sqlite` stores each review. The same WAL / synchronous FULL settings apply. Older Phase 8 code ignores this new table; search schema version 3 is unchanged. Clearing the search index preserves cleanup history and virtual collections. Recycled collection paths remain visible as unavailable until a user removes their tags or restores the file.

| State | Meaning |
| --- | --- |
| `planned` | Separate approval pending; no filesystem change |
| `cancelled` | Unstarted review dismissed |
| `failed` | Validation or pre-operation failure; reason recorded |
| `recycling` | Approved intent committed before calling Windows |
| `recycled` | Windows confirmed the per-file Recycle Bin destination |
| `needs_review` | Interrupted/failed/unconfirmed Shell operation; inspect original path and Windows Recycle Bin |

Startup recovery only turns unfinished `recycling` records into `needs_review`. It never recycles, retries or permanently removes a file. There is an unavoidable interruption window after Windows acts and before the app records completion; that window is deliberately reported as uncertain. All operation records persist; the UI shows the newest 100.

## Bounds and validation

Each scan considers at most 50,000 indexed files and returns at most 1,000 suggestions; hitting either limit is reported with a request to choose a smaller indexed folder. Size groups avoid hashing unrelated unique-size files unless a rule requires them. Hashing/byte comparison stream through bounded buffers and honor cancellation. Similar/version discovery uses the existing bounded related-files candidates (up to 12 displayed), not an exhaustive version graph. Thresholds remain engineering heuristics, not probabilities.

Automated checks cover size/hash/byte comparison, scope, hard links, stale same-size content with preserved timestamps, retained-copy changes, reversed keep choices, cancellation, write/delete locks, opt-in rules, repository protections, alternate streams, junctions, read-only/zero-byte files, durable history and discovery-only versions. Real Windows tests recycle only generated temporary fixtures and verify the returned bin path and retained bytes. Child-process tests terminate abruptly before and after the Shell operation, reopen the journal and verify that recovery does not change files. Tests never empty the user's Recycle Bin; a few tiny, clearly synthetic fixture files remain there.

The QML test scans eight identical fixtures, displays seven candidate rows, opens the confirmation and cancels it, checking all originals remain. Screenshots are written to `build/windows-release/ui-checks/phase9-*.png`. The desktop model test checks actual BGE similar-document evidence and AI-off behavior. The package smoke test includes duplicate discovery, approved recycling and retention of the original before restart/index clearing.

Final local results on 2026-11-19:

| Check | Result |
| --- | --- |
| Release, real model enabled | 22/22 checks passed, 78.32 seconds |
| Debug, real model enabled | 22/22 checks passed, 134.20 seconds |
| Cleanup service/controller suite | 15 test cases, 260 assertions passed in both builds |
| Desktop real-model integration | 3 test cases, 53 assertions passed in both builds |
| Existing retrieval quality evaluation | Passed; MRR 0.97619, recall@10 0.98214, no quality regression |
| Package with developer paths removed | Checksums, real extraction/AI, tagging, move/undo, duplicate recycling, retained-original check, restart, index clearing and QML startup passed |
| ZIP integrity | 1,533 entries checked; no corrupt entry |

Reports: [packaged workflow](evaluation/phase9-package-smoke.json), [retrieval results](evaluation/phase9-quality-release.json), [test summary](evaluation/phase9-validation.json), and [artifact identity](evaluation/phase9-package-artifact.json). The local package is `build/FileSearcher-phase9-local.zip` (142,757,443 bytes). Its source snapshot was created before these final validation notes.

Phase 9 is complete for local validation. Separate clean-machine and varied-filesystem/Recycle Bin-policy checks remain part of release readiness. No Phase 10 work or public release sign-off is implied.

## Primary Windows references

- [IFileOperation flags](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperation-setoperationflags)
- [PreDeleteItem cancellation contract](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifileoperationprogresssink-predeleteitem)
- [Transfer-source flags](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/ne-shobjidl_core-_transfer_source_flags)
- [IFileOperation STA requirements](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ifileoperation)
