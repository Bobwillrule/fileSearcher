# Phase 8 — Related files and reviewed organization

Implementation date: 2026-11-18. The user explicitly authorized this phase after the Phase 7 stop checkpoint. Phase 7's independent-machine and distribution gates remain open. Phase 9 is not started.

## Workflow

Select a search result, then open **Related & organize**. The panel has related evidence/suggestions, category management and move history. Background work leaves the main UI responsive; buttons prevent overlapping organization operations.

- Related results exclude the selected file and show their paths, indexed excerpts and the contributing filename/folder/project reasons. Their scores are ranking signals, not proof of a relationship.
- Categories have a name, description, optional existing destination folder and up to 12 example paths. The inspected file can become an example; examples can be cleared. Missing/unindexed examples contribute no semantic evidence.
- Accept tag creates a virtual collection entry. Reject removes its tag and suppresses the suggestion for the same indexed size/mtime and category revision. Editing a category or reindexing a changed file permits reevaluation. Collections can be inspected and individual tags removed, including unavailable paths.
- Review move prepares the exact source/destination and opens a separate approval dialog. Choosing another destination prepares a fresh plan. Saving categories, accepting tags, rejecting suggestions and cancelling unstarted plans never move files.
- Move history supports reviewed undo and review/finish of a verified interrupted copy. No startup operation resumes deletion automatically. Collection paths, decisions and examples follow successfully recorded moves and undo.

## Evidence and abstention

No new model or paid service is introduced. The existing pinned BGE vectors supply document similarity; HNSW is the candidate index, not the model. AI-off disables vector-based relationships and examples, retaining deterministic context/term matching and manual collections.

For related files, first/middle/last ready chunks (at most three) form a normalized centroid, which queries the existing retrieval path. Up to 1,000 additional context/name candidates are considered. Scores combine semantic similarity with shared filename terms, same-directory and project-root boosts. A semantic score of at least 0.65, two shared filename terms, or a shared term plus folder/project context is required. Same-directory membership alone is insufficient. The panel shows the best 12 files. Sampling and bounded candidates mean this is discovery, not an exhaustive relationship graph.

Category terms come from the name/description and match the filename plus the first eight indexed passages. At least two matching terms are required for a lexical score. Example matching compares sampled source/example vectors, with a 0.72 semantic floor. The winning category must score at least 0.70 and lead the next candidate by at least 0.06; otherwise the system abstains. These are transparent engineering heuristics, not calibrated probabilities. A manual collection choice remains available.

The source must still match its indexed size/mtime before inspection or a decision. Suggestions may become stale after subsequent filesystem/index changes, so destructive actions use a separate fresh snapshot and revalidation instead of trusting suggestion scores.

## Durable data and move protocol

`organization.sqlite` is separate from disposable metadata/vectors. Schema version 1 stores categories, revisions, example paths, decisions, collection membership and operation JSON. WAL plus synchronous FULL commits persist operation intent before mutations. Clearing the search index preserves this database; Storage & help reports its size. The UI lists the newest 100 operations, while recovery inspects all unfinished records.

Prepare locks and validates source/destination ancestors, rejects linked/cloud/device paths and protected locations, and records source volume/file identity, creation/last-write time, size and primary-stream SHA-256. It also records source/destination parent identities and a unique temporary pathname. Preparing does not move anything.

Execute reopens the reviewed file, pins ancestors and checks identities, timestamps and contents again. Parent folders replaced after review are refused. The final target must be absent. Handles prevent ordinary concurrent writes/deletes during execution. Same-volume moves rename the held file without replacement. Cross-volume moves follow the copy/verify/delete protocol below. These operations use Windows handle APIs; no shell move/delete command is constructed from user paths.

| State/checkpoint | Filesystem action or recovery meaning |
| --- | --- |
| `planned` | Persisted review; no file mutation. May be cancelled. |
| `executing` | Persisted before rename or temporary-file creation. |
| `copying` | Exclusive temporary copy identity persisted before data writes. |
| `publishing` | Copy flushed, hashed and checked; last-write time retained. |
| `removing` | Verified copy published without overwrite and flushed; source is rechecked before removal through its held handle. |
| `completed` | Outcome and virtual paths committed. |
| `copy_ready` | Recovery found both expected intact copies. A fresh review/approval may finish source removal. |
| `needs_review` | State cannot be safely inferred or a file changed. No automatic filesystem change; exact source/destination/temporary paths are shown. |
| `undone` | A separately journaled reverse move completed and updated the original record. |

Recovery is observational. A verified destination with an absent source can complete the journal record. Two expected intact files permit reviewed finishing. Other interrupted/partial states retain files for manual inspection, including incomplete temporary copies. The app never deletes a partial copy on startup or guesses that an unfamiliar file is expendable.

Undo is another reviewed, journaled move. It refuses a changed/replaced destination file, a substituted original parent folder, a new occupant at the original path, or an original operation that is no longer eligible. Identity and SHA-256 checks catch same-size modifications even if the last-write time is restored. A reverse operation is linked to its forward operation, and recovery reconciles interrupted undo records.

## Supported scope and limits

- Ordinary local Windows files with one hard-link name. Read-only, encrypted, linked/reparse and cloud-placeholder files are refused. UNC/device/alternate-stream path syntax is refused. Existing destinations are never replaced.
- Repository markers (`.git`, `.hg`, `.svn`, `CMakeLists.txt`), known build/tool/dependency folders, system/application directories and app index storage are protected. These files can still be virtually tagged.
- Same-volume renames preserve the file and its streams. Cross-volume copying currently refuses alternate data streams rather than dropping them; use Explorer for those files. Identity/content checks use the primary stream and the recorded timestamps. This is not an archival metadata/ACL backup tool.
- Copy/verify/delete is tested through its exact production path using a forced-copy test mode on the available local volume. Separate physical-volume and filesystem combinations still need independent validation; no second disk was mounted or reformatted for these tests.
- Files moved outside indexed roots remain in their new locations and in collections; they become searchable again after that destination is added. Root rescans are queued after moves/undo and honor saved pause state.
- Interrupted temporary copies are retained for review. Hashing/copying large files can take time. Process-interruption recovery is tested; arbitrary disk failure/power-loss guarantees are outside this validation.

## Validation

The organization suite covers evidence/self-exclusion, lexical fallback, category examples and ambiguity, rejection/revision handling, collection persistence across search reset, Unicode moves, tag remapping, conflicts, cancelled plans, same-size changes with restored timestamps, replaced parent folders, undo, copy/verify/delete, junctions, hard links, alternate streams and changed interrupted copies.

Fault injection covers intent, rename, temporary creation, copy start/partial copy, publish, pre-removal and post-removal boundaries. Additional child-process tests terminate the process abruptly after rename, publication and source removal, then reopen the journal and verify observational recovery. An integration test moves and undoes while live folder monitoring updates the search results. The QML test checks minimum-size organization/category/review screens and verifies that rejecting the move confirmation leaves all fixture files in place.

Real-model validation checks related passages and example-only suggestions using actual BGE embeddings, then checks AI-off behavior. The packaged smoke workflow additionally accepts a tag, reviews/executes a move, reviews/executes undo and checks preserved source bytes after clearing the search index.

Final local validation on 2026-11-18:

| Check | Result |
| --- | --- |
| Release suite, with real model | 21/21 passed, 74.09 seconds |
| Debug suite before final AI-off/model/package additions | 21/21 passed, 119.72 seconds |
| Final affected Debug checks: organization, desktop search/embeddings, QML and startup | 5/5 passed, 45.08 seconds |
| Organization suite in final Release | 14 test cases, 386 assertions passed |
| Real-model desktop workflows in final Release | 3 test cases, 48 assertions passed |
| Existing full retrieval evaluation | Passed; MRR 0.97619, recall@10 0.98214; no quality regression |
| Portable package, sanitized developer paths | Manifest hashes, actual model/extractors, tagging, approved move, undo, restart/index-clear and QML startup passed |
| Package ZIP integrity | 1,519 entries checked, no corrupt entry |

Reports: [packaged workflow](evaluation/phase8-package-smoke.json), [retrieval evaluation](evaluation/phase8-quality-release.json), and [artifact identity](evaluation/phase8-package-artifact.json). The local validation package is `build/FileSearcher-phase8-local.zip` (142,636,481 bytes); its source snapshot was created before these final validation notes. UI screenshots are in `build/windows-release/ui-checks/phase8-*.png`.

These results complete Phase 8's local implementation gate. The package was tested on the development Windows machine with developer paths removed, not a separate clean machine. The physical cross-volume and public-distribution checks described above remain open. Phase 9 was not started.

## Primary implementation references

- [FILE_RENAME_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_rename_info): handle rename and refusal to replace existing targets.
- [SetFileInformationByHandle](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileinformationbyhandle): rename/disposition through held file handles.
- [FindFirstStreamW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirststreamw): inspecting streams before cross-volume copying.
