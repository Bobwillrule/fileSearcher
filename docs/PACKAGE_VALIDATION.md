# Windows validation package

This is a local validation candidate, not an approved public release. The independent clean-machine and distribution checks below remain release gates. No account, paid API, Python, Qt SDK, or compiler is needed to run the application. This package contains the pinned local model.

## Running

1. Extract to a writable local folder. If Windows reports a missing MSVC runtime, run the included official `vc_redist.x64.exe` installer, then retry.
2. Run `filesearcher.exe`. Add a small test folder first. Indexing and virtual tags leave files in place. In Phase 8, only a separately reviewed and approved move changes file locations.
3. Run `powershell -NoProfile -ExecutionPolicy Bypass -File .\verify-package.ps1` from the extracted directory to verify checksums, Qt startup, PDF/DOCX reading, local semantic search, category tagging, approved move/undo, restart, and clearing saved data. The check uses temporary synthetic documents and restores the moved fixture before clearing the index. Add `-Scale` to index 1,001 physical small files and four large text documents with AI indexing disabled, measuring elapsed time and event-loop responsiveness.

The verifier removes developer paths and Qt overrides from its child environment, but running it on a development machine does **not** prove clean-machine compatibility. It does not disable the machine's network or install prerequisites automatically.

## Storage, update and removal

The app saves its index under the current user's local app-data directory (`FileSearcher/FileSearcher`); the Storage & help dialog shows the exact path and size. Saved extracted text and vectors are not encrypted. All computation runs locally; the application has no telemetry or network requests. The bundled model is about 127 MB. Disabling AI preserves keyword search; Release AI memory closes the query worker until needed again.

To upgrade, close the old app and extract the new package into another folder. It reuses the existing per-user database. The search schema remains version 3; Phase 8 adds a separate version-1 `organization.sqlite` for categories, tags, examples and move history. Older search-schema migration is covered by automated storage tests. To remove the app, close it and delete its extracted application folder. This leaves your original documents and per-user data intact. Storage & help > Clear index removes derived search content but deliberately retains collections and move history. SQLite VACUUM is not a secure-erasure guarantee. Do not place your own documents in the application directory.

## Independent Windows release check (pending)

- Use a supported x64 Windows machine/VM without Qt, Visual Studio, Python, or this repository. Record OS, CPU, RAM and runtime installation result.
- Copy only the candidate package. Install the included Microsoft runtime if required. Disconnect networking, then run the verifier and open the app normally.
- Check real font/DPI rendering, keyboard-only navigation, folder picker, opening/revealing a selected synthetic document, pause/resume, exclusions, progress, no-results, invalid search recovery, storage controls and clear confirmation.
- Test a previous-version index with a copied synthetic corpus, then an upgrade; verify search results and original hashes. Test removing the application and clearing its index separately; verify original hashes again.
- Save verification logs and screenshots. An offscreen test on the development machine does not replace these checks.

## Distribution gate (pending)

Included notices cover BGE, ONNX Runtime and its bundled components, hnswlib, libzip and zlib. Qt's SDK SBOMs are included to inventory deployed modules and bundled third-party components. Before public distribution, complete the Qt/PDFium third-party notice collection, provide the exact corresponding Qt source and required license texts/relink or library-replacement information, and choose this project's distribution license. The source snapshot here is for reproducibility; it does not assign a new license or by itself satisfy every dependency's obligations.

Deployment follows [Qt's Windows deployment documentation](https://doc.qt.io/qt-6.8/windows-deployment.html). Consult [Qt's LGPL obligations](https://www.qt.io/licensing/open-source-lgpl-obligations) and [Microsoft's Visual C++ deployment guidance](https://learn.microsoft.com/en-us/cpp/windows/deploying-native-desktop-applications-visual-cpp?view=msvc-170) when completing this gate. Runtime binaries are collected by windeployqt; the official Microsoft redistributable installer is supplied separately. No public publishing or installer registration is performed by this phase.
