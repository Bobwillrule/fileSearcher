# fileSearcher

A Windows desktop application for local semantic file search, related-file discovery,
virtual collections, user-approved organization and duplicate cleanup review.

## Download for Windows x64

[Download the setup EXE](https://github.com/Bobwillrule/fileSearcher/releases/download/v0.2.0-rc.1/FileSearcher-Setup.exe) for a guided installation and a Start menu shortcut.
For the portable edition, [download the ZIP](https://github.com/Bobwillrule/fileSearcher/releases/download/v0.2.0-rc.1/FileSearcher-0.2.0-Portable-x64.zip), extract it, and open `filesearcher.exe`.
Both editions include the local search model. The [0.2.0 release candidate page](https://github.com/Bobwillrule/fileSearcher/releases/tag/v0.2.0-rc.1) includes checksums, dependency sources, and testing notes.

## Project overview

See [the implementation gameplan](IMPLEMENTATION_PLAN.md) for the technology
stack, architecture, phased roadmap, acceptance criteria, and release checks.

Version 0.2.0 is a Windows release candidate. It backs up existing state before upgrade
and includes a restore tool, dependency notices and a corresponding source companion.
The independent clean Windows offline test is pending. See the [release guide](docs/PHASE_10_RELEASE.md).
Select local folders, edit exclusions, scan in the background, filter results,
and open files or show them in Explorer. The index survives restart.
Selected folders are watched for changes, checked at startup, and reconciled
periodically. **Pause indexing** saves pending work; **Resume indexing** catches up.
Search document contents, inspect matching passages, and switch to exact-phrase
or advanced keyword search. Plain text/source files, text PDFs, and DOCX body text
are supported. A local BGE model now prepares and saves embeddings in the background.
Use **AI + keywords** to describe a file in your own words. Results combine
filenames, keywords and semantic similarity, with matching passages and source labels.

Use **Ctrl+F** to focus search, **Escape** to clear it, and **Down/Up**, **Home/End**,
and **Page Down/Page Up** to select results. Enter opens the selected file; **F5**
rescans. Selection survives result refreshes when the same file remains present.
**Storage & help** shows local index/model usage and offers AI memory release,
AI-data rebuild and index clearing with confirmation. Original files are preserved.
The query model stays loaded between searches; turning AI off or releasing memory
closes it. See [Phase 7 results](docs/PHASE_7_USABILITY_PERFORMANCE.md).

Select a result and choose **Related & organize**. Related files show indexed evidence
and filename/folder context. Create categories with descriptions, optional existing
destination folders and example files. **Accept tag** adds a virtual collection entry;
**Review move** is a separate approval. Weak or ambiguous matches produce no category
suggestion. Turning AI off also disables semantic organization matching.

Move history records plans before filesystem changes, checks source identity/content,
refuses overwrites, and offers reviewed undo. Startup recovery inspects interrupted
operations without automatically deleting files. Repository, build, application and
index folders are protected from moves. See [Phase 8 details and supported move scope](docs/PHASE_8_ORGANIZATION.md).

Open **Duplicates & cleanup** in the left sidebar and scan one indexed folder.
Exact copies are verified by size, SHA-256 and byte comparison. Each review names
the file to keep; **Send to Recycle Bin** is a separate approval. Similar documents
and possible versions are discovery-only. Old-installer and generated-build rules
are off by default. Changed files and unsupported recycling stop the action.
Cleanup history survives index clearing. Restore recycled files through Windows
Recycle Bin; the app does not offer cleanup undo or empty the bin. See
[Phase 9 behavior, checks and limits](docs/PHASE_9_CLEANUP.md).

After a Release build with model tests, run `tools/prepare_release_sources.py`
with the development Python environment, then create a portable validation folder with
`./tools/package.ps1` (optionally `-Zip`). Run the bundled `verify-package.ps1`
from the generated folder to check it without developer paths. Add `-Scale`
for the physical-file indexing workload. See [package instructions and remaining
release checks](docs/PACKAGE_VALIDATION.md).

## Build on Windows

Prerequisites: Windows x64, Git, Visual Studio 2022 Build Tools with the Desktop
development with C++ workload (including Windows SDK, CMake, and Ninja), and
Qt 6.8.3 for MSVC 2022 x64. Visual Studio's C++ toolset 14.44 is the initial
reference toolset. Python is needed only for the optional Qt download helper and
development fixture generation; it is not part of the shipped application.

The local setup uses ignored `.tools/` and `build/` directories:

```powershell
git clone https://github.com/microsoft/vcpkg.git .tools/vcpkg
git -C .tools/vcpkg checkout 19e7a138440f005fea23d92b19df87f8040e66c1
.\.tools\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

Install Qt through the official Qt installer and set `QT_ROOT_DIR` to its
`msvc2022_64` folder, or download the same official binaries locally:

```powershell
python -m venv .tools/python
.\.tools\python\Scripts\python.exe -m pip install aqtinstall==3.3.0
.\.tools\python\Scripts\python.exe -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O .tools/qt --archives qtbase qtdeclarative qtshadertools qtsvg qttools
```

Build and test from PowerShell:

```powershell
.\tools\setup-phase1.ps1
.\tools\build.ps1 -Preset windows-debug -ModelTests
.\tools\build.ps1 -Preset windows-release -ModelTests
```

The helper finds the installed MSVC environment, configures CMake, builds, and
runs CTest. `core-debug` builds just the HNSW core/tests without Qt. To use a
different dependency-manager installation, set `VCPKG_ROOT` before invoking it.
Dependencies are pinned by the manifest's vcpkg baseline.

`setup-phase1.ps1` installs development-only Python reference tools, Qt PDF,
ONNX Runtime 1.23.2, and pinned BGE assets into ignored directories. It verifies
SHA-256 hashes and creates independent PyTorch reference embeddings. Downloads
require internet; inference and document extraction run locally afterward.
Allow approximately 1 GB of additional local storage for reference tools and models.
The downloaded model plus reference weights occupy about 270 MB.

Omit `-ModelTests` to skip real-model checks; use it for the full suite, including
model parity, persistent embeddings, recovery and desktop AI workflow checks.
`windows-clean` uses a separate Release build directory.
Python runs fixture generation and reference checks only, never native app inference.

Launch the developer build with the matching Qt runtime on PATH:

```powershell
$env:PATH = "$PWD\.tools\qt\6.8.3\msvc2022_64\bin;$env:PATH"
.\build\windows-debug\filesearcher.exe
```

This is a developer build, not a standalone distributable. Qt runtime packaging,
notices, and clean-machine deployment validation remain release work.

## Using file search

1. Choose **Add folder**. The default exclusions are `.git`, `node_modules`,
   `build`, and `.tools`; edit these before adding a folder or through its
   **Exclusions** button afterward.
2. Use **AI + keywords** for natural-language descriptions, such as "the component
   that lets electricity flow one way." This shows up to 200 ranked files and
   prioritizes exact filenames. If AI is unavailable, a notice explains the keyword fallback.
   **Names & contents** combines literal filename/path and text matches.
   **Content: all words** requires all words within a passage; **Content: exact
   phrase** requires them in order. **Names & paths** retains filename-fragment
   search. Operators such as `OR` are words unless **Advanced content syntax** is selected.
3. Filter by extension, indexed folder, byte size, or modified date. Results show
   up to 500 entries in lexical modes or 200 ranked files in hybrid mode; narrow
   the query to see a smaller set. Filters apply to every matching channel.
4. Select a result to see its matching text, page/line location, and extraction
   status. The preview's arrows move between indexed passages. Markup is displayed
   as plain text. **Reindex file contents** rebuilds cached text when needed.
5. Use **Open** or **Show in folder**. Double-click or Enter also opens the
   selected result. File changes update automatically; **Rescan folders** requests
   an immediate reconciliation. While paused, rescans wait until you resume.
6. Open **Local AI settings** to enable/disable AI indexing and search or choose
   Low, Balanced or Faster resource use. Completed batches survive pause and restart.
   Missing model/runtime files leave keyword search available and show an AI notice.

The app stores `metadata.sqlite` and the durable `work.sqlite` queue under Qt's per-user local app data directory
(normally `%LOCALAPPDATA%\FileSearcher\FileSearcher`). After scanning metadata,
it reads supported files locally and stores extracted passages in this index. Removing an
indexed folder removes search records and leaves original files in place.
Exclusions are comma-separated basename or relative-path wildcard patterns.
Linked folders/files, system entries, and offline/recall placeholders are skipped.
Pause cancels the active scan or document reader and saves pending work. Completed
metadata scans and completed per-file text revisions remain published. Pause survives
restart. Inaccessible roots retain their old records; bounded retries and a
five-minute reconciliation check allow later recovery. A second app window cannot
write the same index. Monitoring failures appear in the sidebar.

For an isolated developer preview containing only synthetic files:

```powershell
$env:PATH = "$PWD\.tools\qt\6.8.3\msvc2022_64\bin;$env:PATH"
.\build\windows-debug\metadata_probe.exe --demo build/demo-files
.\build\windows-debug\filesearcher.exe --data-dir build/demo-files/index
```

The demo destination must be new. For repeat launches, run only the second command.
See [Phase 2 notes](docs/PHASE_2_METADATA.md) for the base storage/search design
and benchmarks, and [Phase 3 notes](docs/PHASE_3_INCREMENTAL.md) for automatic
updates, recovery, validation, and current limitations.
See [Phase 4 notes](docs/PHASE_4_CONTENT_SEARCH.md) for supported formats,
reader limits, search syntax, passage storage, and validation.
See [Phase 5 notes](docs/PHASE_5_EMBEDDINGS.md) for model packaging, fingerprints,
SQLite vectors, resource limits and recoverable HNSW caches. Prepared builds copy
the pinned model and license next to the application. The `vectors/` cache in the
index directory is disposable; SQLite remains the source of truth. The current
HNSW cache limit is 100,000 chunks.
See [Phase 6 notes](docs/PHASE_6_HYBRID_SEARCH.md) for ranking, cancellation,
filtering and the fixed 46-query evaluation. Original quality targets passed on
the full and held-out sets. The first desktop semantic query currently includes
model startup (about 1.3 seconds on the recorded local machine); persistent-worker
and large-corpus performance work belongs to phase 7.

Text readers accept UTF-8, or UTF-16/UTF-32 with a byte-order mark. Text files are
limited to 4 MiB, PDF/DOCX to 32 MiB, and extracted text to one million UTF-16
characters. Truncation, unsupported encodings, unreadable files, password protection,
and PDFs requiring OCR have visible statuses. OCR is not implemented.

You can optionally launch with a query:

```powershell
.\build\windows-debug\filesearcher.exe --query "Master Theorem"
```

## Feasibility checks

CTest covers cosine normalization, stable chunk IDs, updates, save/reload,
deleted-vector exclusion, invalid vectors, and child-worker failure/timeout
recovery. Tests also enforce a 256 MiB parser memory limit, check ten document
scenarios, and compare native BGE output to 21 PyTorch reference cases, including
mixed-length batches. An offscreen startup test checks that the QML shell loads.

Generate synthetic extraction documents and probe one explicitly:

```powershell
.\.tools\python\Scripts\python.exe tools/create_extraction_fixtures.py
$env:PATH = "$PWD\.tools\qt\6.8.3\msvc2022_64\bin;$env:PATH"
.\build\windows-debug\extraction_probe.exe .\build\windows-debug\document_worker.exe .\build\fixtures\text.pdf
```

The probe emits JSON status and character counts, not document contents. A
nonzero exit status means extraction did not fully succeed. It does not scan
folders or change source documents. `document_worker` uses bundled Qt PDF/PDFium
for PDFs and libzip + streaming XML for DOCX body text. `ifilter_worker` remains a
separate diagnostic for registered Windows filters; it is not required for PDF/DOCX.
Scanned/image-only PDFs are identified as needing OCR; OCR is not implemented.

The synthetic retrieval set is in `tests/fixtures/retrieval.json`.
Detailed evidence, architecture decisions, and limitations are recorded in
[the feasibility notes](docs/PHASE_1_FEASIBILITY.md).
