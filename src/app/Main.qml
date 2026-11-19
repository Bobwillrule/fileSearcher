import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    visible: true
    width: 1280
    height: 800
    minimumWidth: 1000
    minimumHeight: 650
    title: "FileSearcher"
    font.family: "Segoe UI"
    font.pixelSize: 14
    color: "#10171f"
    palette.text: "#e6edf5"
    palette.windowText: "#e6edf5"
    palette.buttonText: "#e6edf5"
    palette.base: "#1c2733"
    palette.window: "#16212c"
    palette.button: "#263747"
    palette.mid: "#41566c"
    palette.dark: "#a8bccc"
    palette.placeholderText: "#9fb2c5"
    palette.highlight: "#64d8be"
    palette.highlightedText: "#10171f"
    property double selectedRootId: 0
    property double selectedFileId: 0
    function selectResult(index) {
        resultsList.currentIndex = Math.max(-1, Math.min(index, fileApp.results.length - 1))
        const row = fileApp.results[resultsList.currentIndex]
        selectedFileId = row ? row.fileId : 0
        if (resultsList.currentIndex >= 0) resultsList.positionViewAtIndex(resultsList.currentIndex, ListView.Contain)
    }
    function readableBytes(value) { return (Number(value || 0) / (1024 * 1024)).toFixed(1) + " MB" }
    property var folderChoices: [{label: "All indexed folders", rootId: 0}].concat(fileApp.roots.map(function(r) { return {label: r.path, rootId: r.rootId} }))
    function contentLabel(status) {
        const labels = {pending: "Waiting for text", ok: "Text indexed", text_limit: "Text truncated", no_text: "No text found",
            no_text_or_ocr_required: "No text — OCR may be needed", unsupported_format: "Metadata only: unsupported type",
            unsupported_encoding: "Unsupported encoding — use UTF-8 or Unicode with BOM", binary_data: "Binary content skipped",
            file_too_large: "Too large for text extraction", password_protected: "Password protected", invalid_document: "Unreadable document",
            read_failed: "Cannot read contents", timeout: "Extraction timed out", worker_failed: "Document reader failed",
            changed_during_read: "Changed while reading; waiting to retry", placeholder_or_link: "Linked or cloud file skipped",
            xml_limit: "Document exceeds XML limit", page_limit: "Document exceeds page limit", missing_file: "File unavailable",
            output_limit: "Reader output exceeded the limit", resource_limit_setup_failed: "Reader limits unavailable",
            invalid_response: "Document reader returned invalid data", unsupported_xml: "Unsupported document XML",
            unsupported_location: "Unsupported file location", start_failed: "Document reader unavailable",
            cancelled: "Reading cancelled", preview_unavailable: "Preview unavailable", unavailable: "File no longer indexed"}
        return labels[status] || status || "Select a file"
    }
    function showCurrentPreview() {
        const row = fileApp.results[resultsList.currentIndex]
        fileApp.loadPreview(row && previewToggle.checked ? row.fileId : 0, row ? row.passageId : 0)
    }
    function runSearch() {
        debounce.stop()
        const choice = folderChoices[folderFilter.currentIndex]
        fileApp.search(searchInput.text, typeInput.text, choice ? choice.rootId : 0,
                       minInput.text, maxInput.text, dateFilter.currentIndex === 1 ? 1 : dateFilter.currentIndex === 2 ? 7 : dateFilter.currentIndex === 3 ? 30 : 0,
                       [5,1,0,2,3,4][searchMode.currentIndex])
    }
    Timer { id: debounce; interval: 180; onTriggered: window.runSearch() }
    Connections {
        target: fileApp
        function onRootsChanged() {
            const index = window.folderChoices.findIndex(function(r) { return r.rootId === window.selectedRootId })
            folderFilter.currentIndex = index >= 0 ? index : 0
            if (index < 0) window.selectedRootId = 0
            debounce.restart()
        }
        function onResultsChanged() {
            const old = fileApp.results.findIndex(function(row) { return row.fileId === window.selectedFileId })
            window.selectResult(old >= 0 ? old : fileApp.results.length ? 0 : -1)
            window.showCurrentPreview()
        }
    }
    Shortcut { sequence: "Ctrl+F"; onActivated: { searchInput.forceActiveFocus(); searchInput.selectAll() } }
    Shortcut { sequence: "F5"; onActivated: fileApp.scan() }
    OrganizationPanel {
        id: organizationPanel
        objectName: "organizationPanel"
        organizer: fileApp.organizer
        selectedFileId: window.selectedFileId
        anchors.centerIn: parent
        width: Math.min(980, window.width - 32)
        height: Math.min(700, window.height - 32)
        onOpenFileRequested: function(id) { fileApp.openFile(id, false) }
    }
    FolderDialog {
        id: folderPicker
        title: "Choose a folder to index"
        onAccepted: fileApp.addFolder(selectedFolder, defaultExclusions.text)
    }
    CleanupPanel {
        id: cleanupPanel
        objectName: "cleanupPanel"
        cleanup: fileApp.cleanup
        indexedRoots: fileApp.roots
        selectedFileId: window.selectedFileId
        anchors.centerIn: parent
        width: Math.min(980, window.width - 32)
        height: Math.min(700, window.height - 32)
        onOpenFileRequested: function(id) { fileApp.openFile(id, false) }
    }
    Dialog {
        id: exclusionsDialog
        property double rootId: 0
        title: "Folder exclusions"
        anchors.centerIn: parent
        width: 580
        modal: true
        standardButtons: Dialog.Save | Dialog.Cancel
        ColumnLayout {
            width: parent.width
            Label { text: "Comma-separated names or relative path patterns, such as *.tmp or cache/*."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            TextField { id: editedExclusions; Layout.fillWidth: true; Accessible.name: "Exclusion patterns" }
            Label { text: "Saving runs a new scan. Original files are never removed."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        }
        onAccepted: fileApp.changeExclusions(rootId, editedExclusions.text)
    }
    Dialog {
        id: aiDialog
        title: "Local AI indexing"
        anchors.centerIn: parent
        width: 480
        modal: true
        standardButtons: Dialog.Close
        ColumnLayout {
            width: parent.width
            CheckBox { text: "Prepare files for AI search"; checked: fileApp.aiEnabled; onClicked: fileApp.configureAi(checked,fileApp.aiProfile) }
            Label { text: "Computer resource use" }
            ComboBox { model: ["Low", "Balanced", "Faster"]; currentIndex: fileApp.aiProfile; enabled: fileApp.aiEnabled; Layout.fillWidth: true; onActivated: fileApp.configureAi(fileApp.aiEnabled,currentIndex) }
            Label { text: fileApp.aiStatus; textFormat: Text.PlainText; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Label { text: "BGE small English model · MIT license\nRuns on this computer. No account or paid API.\nAI search combines meaning, filenames and keywords. Turning AI off keeps keyword search available."; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#9fb2c5" }
        }
    }
    Dialog {
        id: storageDialog
        objectName: "storageDialog"
        title: "Storage and supported files"
        anchors.centerIn: parent
        width: Math.min(620, window.width - 40)
        modal: true
        standardButtons: Dialog.Close
        onOpened: fileApp.refreshStorage()
        ColumnLayout {
            width: parent.width; spacing: 10
            Label { text: "Search index: " + window.readableBytes(fileApp.storageInfo.indexBytes) + " · " + (fileApp.storageInfo.files || 0) + " files · " + (fileApp.storageInfo.vectors || 0) + " AI passages"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Label { text: fileApp.storageInfo.indexPath || "Loading storage details…"; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; color: "#9fb2c5" }
            Label { text: "AI model: " + window.readableBytes(fileApp.storageInfo.modelBytes); Layout.fillWidth: true }
            Label { text: "Collections, move and cleanup history: " + window.readableBytes(fileApp.storageInfo.organizationBytes) + " (kept when clearing the search index)"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Label { text: fileApp.storageInfo.modelPath || ""; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; color: "#9fb2c5" }
            RowLayout {
                Button { text: "Open index folder"; onClicked: fileApp.openStorageFolder(false) }
                Button { text: "Open model folder"; onClicked: fileApp.openStorageFolder(true) }
                Button { text: "Refresh"; onClicked: fileApp.refreshStorage() }
            }
            RowLayout {
                Button { text: "Release AI memory"; onClicked: fileApp.releaseAiMemory() }
                Button { text: "Rebuild AI data"; enabled: !fileApp.busy && fileApp.roots.length > 0; onClicked: { clearDialog.embeddingsOnly = true; clearDialog.open() } }
                Button { text: "Clear index…"; enabled: !fileApp.busy; onClicked: { clearDialog.embeddingsOnly = false; clearDialog.open() } }
            }
            Label { text: "Contents: UTF-8 / Unicode text and source code, text PDFs, and DOCX body text. Other types are searchable by name. Scanned PDFs need OCR, which is not included."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Label { text: "Text files: up to 4 MB. PDF/DOCX: up to 32 MB. Linked and cloud placeholder files are skipped. Saved text and embeddings are local and are not encrypted."; color: "#9fb2c5"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            Label { text: fileApp.storageInfo.error || "Disable AI in Local AI settings to keep keyword-only search. To restore missing model files, extract a complete application package again."; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#9fb2c5" }
        }
    }
    Dialog {
        id: clearDialog
        property bool embeddingsOnly: false
        title: embeddingsOnly ? "Rebuild saved AI data?" : "Clear the search index?"
        anchors.centerIn: parent; width: 480; modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { width: parent.width; text: clearDialog.embeddingsOnly ? "Saved vectors and their cache will be cleared and rebuilt. Text search stays available. Original files remain untouched." : "This removes selected folders and saved search data from the index. You will need to add folders again. Original files and the installed model remain untouched."; wrapMode: Text.WordWrap }
        onAccepted: fileApp.clearSavedIndex(embeddingsOnly)
    }
    Dialog {
        id: removeDialog
        property double rootId: 0
        title: "Remove indexed folder?"
        anchors.centerIn: parent
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { text: "This removes its search records, not its files."; textFormat: Text.PlainText }
        onAccepted: fileApp.removeFolder(rootId)
    }
    RowLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.preferredWidth: 290
            Layout.fillHeight: true
            color: "#16212c"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: window.height < 750 ? 8 : 14
                Label { text: "FileSearcher"; font.pixelSize: 25; font.bold: true }
                Label { text: "INDEXED FOLDERS"; color: "#8da3b7"; font.pixelSize: 11; Layout.topMargin: 14 }
                Button { text: "+ Add folder"; Layout.fillWidth: true; enabled: !fileApp.busy; onClicked: folderPicker.open() }
                Label { text: "Default exclusions"; color: "#9fb2c5" }
                TextField {
                    id: defaultExclusions
                    Layout.fillWidth: true
                    text: ".git, node_modules, build, .tools"
                    enabled: !fileApp.busy
                    Accessible.name: "Default folder exclusions"
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: fileApp.roots
                    spacing: 12
                    ScrollBar.vertical: ScrollBar {}
                    delegate: Rectangle {
                        required property var modelData
                        width: ListView.view.width
                        height: rootColumn.implicitHeight + 24
                        color: "#1c2936"
                        radius: 8
                        ColumnLayout {
                            id: rootColumn
                            anchors.left: parent.left; anchors.right: parent.right
                            anchors.top: parent.top; anchors.margins: 12
                            Label { text: modelData.path; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; maximumLineCount: 2; elide: Text.ElideMiddle; Layout.fillWidth: true; font.bold: true; ToolTip.visible: folderHover.hovered; ToolTip.text: modelData.path; HoverHandler { id: folderHover } }
                            Label { text: modelData.status; textFormat: Text.PlainText; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#9fb2c5"; font.pixelSize: 12 }
                            RowLayout {
                                Button {
                                    text: "Exclusions"; enabled: !fileApp.busy
                                    onClicked: { exclusionsDialog.rootId = modelData.rootId; editedExclusions.text = modelData.exclusions; exclusionsDialog.open() }
                                }
                                Button {
                                    text: "Remove"; enabled: !fileApp.busy
                                    onClicked: { removeDialog.rootId = modelData.rootId; removeDialog.open() }
                                }
                            }
                        }
                    }
                    Label { anchors.centerIn: parent; visible: fileApp.roots.length === 0; text: "Your selected folders\nappear here."; horizontalAlignment: Text.AlignHCenter; color: "#8da3b7" }
                }
                Label { text: fileApp.paused ? "Automatic updates paused" : fileApp.monitoring; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: fileApp.paused ? "#ffcd87" : "#8da3b7"; font.pixelSize: 12 }
                Label { text: fileApp.pending + " folder(s) awaiting updates"; visible: fileApp.pending > 0; color: "#8da3b7"; font.pixelSize: 12 }
                Button { text: fileApp.paused ? "Resume indexing" : "Pause indexing"; enabled: fileApp.roots.length > 0; Layout.fillWidth: true; onClicked: fileApp.setPaused(!fileApp.paused) }
                Button { text: "Rescan folders"; enabled: fileApp.roots.length > 0; Layout.fillWidth: true; onClicked: fileApp.scan() }
                Button { text: "Reindex file contents"; enabled: fileApp.roots.length > 0 && !fileApp.busy; Layout.fillWidth: true; onClicked: fileApp.reindexContent() }
                Button { objectName: "cleanupButton"; text: "Duplicates && cleanup"; Layout.fillWidth: true; enabled: fileApp.cleanup !== null; onClicked: cleanupPanel.open() }
                Label { text: "Text stays on this computer\nLinked and cloud placeholder files are skipped"; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#8da3b7"; font.pixelSize: 12; lineHeight: 1.4 }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.margins: window.height < 750 ? 16 : 28
            spacing: window.height < 750 ? 8 : 14
            RowLayout {
                Layout.fillWidth: true
                Label { text: "Find your files."; font.pixelSize: 32; font.bold: true; Layout.fillWidth: true }
                Button { text: "Local AI settings"; onClicked: aiDialog.open() }
                Button { objectName: "storageButton"; text: "Storage && help"; onClicked: storageDialog.open() }
                Button { objectName: "organizationButton"; text: "Related && organize"; enabled: fileApp.organizer !== null; onClicked: { organizationPanel.open(); if(window.selectedFileId > 0) fileApp.organizer.inspect(window.selectedFileId) } }
            }
            Label { text: fileApp.searchNotice || fileApp.aiStatus; textFormat: Text.PlainText; color: "#9fb2c5"; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout {
                ComboBox { id: searchMode; model: ["AI + keywords", "Names & contents", "Names & paths", "Content: all words", "Content: exact phrase", "Advanced content syntax"]; implicitWidth: 230; Accessible.name: "Search mode"; onActivated: debounce.restart() }
                Label { text: searchMode.currentIndex === 0 ? "Describe what you remember. Similar meaning is a ranking signal, not certainty." : searchMode.currentIndex === 5 ? 'FTS syntax: "master theorem" OR recurrence' : searchMode.currentIndex === 4 ? "Words must appear together, in order" : "Ordinary search treats operators as words"; color: "#9fb2c5"; font.pixelSize: 12; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
            RowLayout {
                Layout.fillWidth: true
              TextField {
                    id: searchInput
                    objectName: "searchInput"
                text: startupQuery
                Layout.fillWidth: true
                placeholderText: searchMode.currentIndex === 0 ? "e.g. the circuit that lets electricity flow one way…" : searchMode.currentIndex === 2 ? "Search filenames or paths…" : "Search words inside your files…"
                Accessible.name: "Search files"
                font.pixelSize: 18
                onTextChanged: debounce.restart()
                onAccepted: window.runSearch()
                focus: true
                selectByMouse: true
                Keys.onEscapePressed: { clear(); window.runSearch() }
                Keys.onDownPressed: { resultsList.forceActiveFocus(); window.selectResult(resultsList.currentIndex >= 0 ? resultsList.currentIndex : 0) }
              }
              Button { text: "Clear"; Accessible.name: "Clear search"; enabled: searchInput.text.length > 0; onClicked: { searchInput.clear(); searchInput.forceActiveFocus(); window.runSearch() } }
            }
            RowLayout {
                Layout.fillWidth: true
                TextField { id: typeInput; Layout.preferredWidth: 110; placeholderText: "Type: pdf"; Accessible.name: "File extension"; onTextChanged: debounce.restart() }
                ComboBox { id: folderFilter; Layout.fillWidth: true; Layout.minimumWidth: 0; implicitWidth: 280; model: window.folderChoices; textRole: "label"; Accessible.name: "Folder filter"; onActivated: { window.selectedRootId = window.folderChoices[currentIndex].rootId; debounce.restart() } }
                ComboBox { id: dateFilter; model: ["Any date", "Last day", "Last 7 days", "Last 30 days"]; Accessible.name: "Modified date filter"; onActivated: debounce.restart() }
            }
            RowLayout {
                Label { text: "Size (bytes)"; color: "#9fb2c5" }
                TextField { id: minInput; Layout.preferredWidth: 125; placeholderText: "Minimum"; Accessible.name: "Minimum bytes"; onTextChanged: debounce.restart() }
                TextField { id: maxInput; Layout.preferredWidth: 125; placeholderText: "Maximum"; Accessible.name: "Maximum bytes"; onTextChanged: debounce.restart() }
                Item { Layout.fillWidth: true }
                Button { text: "Clear filters"; onClicked: { typeInput.clear(); minInput.clear(); maxInput.clear(); window.selectedRootId = 0; folderFilter.currentIndex = 0; dateFilter.currentIndex = 0; debounce.restart() } }
            }
            Label { visible: fileApp.error.length > 0; text: fileApp.error; textFormat: Text.PlainText; color: "#ffb5ad"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            RowLayout {
                Label { text: fileApp.searching ? "Searching…" : fileApp.total + (searchMode.currentIndex === 0 && searchInput.text.trim() ? " ranked files" : fileApp.total === 1 ? " file found" : " files found"); color: "#64d8be" }
                Item { Layout.fillWidth: true }
                Label { text: searchMode.currentIndex === 0 && searchInput.text.trim() ? "Up to 200 best matches" : fileApp.total > 500 ? "Showing first 500 — refine your search" : ""; color: "#9fb2c5"; font.pixelSize: 12 }
                CheckBox { id: previewToggle; text: "Text preview"; checked: true; onToggled: window.showCurrentPreview() }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true
                color: "#16212c"; radius: 10
                ListView {
                    id: resultsList
                    objectName: "resultsList"
                    anchors.fill: parent; anchors.margins: 8
                    clip: true
                    spacing: 4
                    model: fileApp.results
                    onCurrentIndexChanged: window.showCurrentPreview()
                    ScrollBar.vertical: ScrollBar {}
                    keyNavigationEnabled: false
                    Keys.onDownPressed: window.selectResult(currentIndex + 1)
                    Keys.onUpPressed: { if (currentIndex <= 0) searchInput.forceActiveFocus(); else window.selectResult(currentIndex - 1) }
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Home) window.selectResult(0)
                        else if (event.key === Qt.Key_End) window.selectResult(count - 1)
                        else if (event.key === Qt.Key_PageDown) window.selectResult(currentIndex + 5)
                        else if (event.key === Qt.Key_PageUp) window.selectResult(currentIndex - 5)
                        else return
                        event.accepted = true
                    }
                    Keys.onEscapePressed: searchInput.forceActiveFocus()
                    Keys.onReturnPressed: { if (currentIndex >= 0) fileApp.openFile(fileApp.results[currentIndex].fileId, false) }
                    delegate: Rectangle {
                        id: resultRow
                        required property var modelData
                        required property int index
                        width: ListView.view.width
                        height: modelData.snippet ? 128 : 94
                        radius: 6
                        color: ListView.isCurrentItem ? "#243b47" : "#1a2733"
                        border.width: ListView.isCurrentItem && resultsList.activeFocus ? 2 : 0
                        border.color: "#64d8be"
                        TapHandler {
                            onTapped: { window.selectResult(resultRow.index); resultsList.forceActiveFocus() }
                            onDoubleTapped: fileApp.openFile(resultRow.modelData.fileId, false)
                        }
                        RowLayout {
                            anchors.fill: parent; anchors.margins: 12
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                Label { text: modelData.name + (modelData.matchLabel ? "  ·  " + modelData.matchLabel : ""); textFormat: Text.PlainText; font.bold: true; font.pixelSize: 15; Layout.fillWidth: true; elide: Text.ElideRight }
                                Label { text: modelData.path; textFormat: Text.PlainText; color: "#a8bccc"; Layout.fillWidth: true; elide: Text.ElideMiddle; font.pixelSize: 12 }
                                Label { text: (modelData.extension || "File") + "  ·  " + Number(modelData.size).toLocaleString() + " bytes  ·  " + Qt.formatDateTime(new Date(Number(modelData.modified)), "yyyy-MM-dd hh:mm") + "  ·  " + window.contentLabel(modelData.contentStatus); textFormat: Text.PlainText; color: "#8da3b7"; font.pixelSize: 11; Layout.fillWidth: true; elide: Text.ElideRight }
                                Label { visible: !!modelData.snippet; text: modelData.location + " · " + modelData.snippet; textFormat: Text.PlainText; color: "#c4e5dc"; wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight; Layout.fillWidth: true; font.pixelSize: 12 }
                            }
                            Button { text: "Open"; onClicked: fileApp.openFile(modelData.fileId, false) }
                            Button { text: "Show in folder"; onClicked: fileApp.openFile(modelData.fileId, true) }
                        }
                    }
                }
                ColumnLayout {
                    anchors.centerIn: parent
                    visible: !fileApp.searching && fileApp.results.length === 0
                    width: parent.width - 48
                    Label { text: fileApp.roots.length === 0 ? "Choose your first folder" : "No matching files"; font.pixelSize: 24; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
                    Label { text: fileApp.roots.length === 0 ? "Add a folder to build your local file index." : "Try a shorter name, clear the filters, or rescan."; color: "#9fb2c5"; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                }
            }
            Rectangle {
                visible: previewToggle.checked
                Layout.fillWidth: true; Layout.preferredHeight: window.height < 750 ? 110 : 185
                color: "#16212c"; radius: 10
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 12; spacing: 6
                    RowLayout {
                        Label { text: fileApp.previewLoading ? "Loading text…" : "Extracted text · " + window.contentLabel(fileApp.preview.status); textFormat: Text.PlainText; color: "#64d8be"; elide: Text.ElideRight; Layout.fillWidth: true }
                        Label { text: fileApp.preview.location || ""; textFormat: Text.PlainText; color: "#9fb2c5"; font.pixelSize: 12 }
                        Button { text: "‹"; implicitWidth: 34; Accessible.name: "Previous passage"; enabled: !fileApp.previewLoading && Number(fileApp.preview.ordinal) > 0; onClicked: fileApp.loadPreview(fileApp.preview.fileId,0,fileApp.preview.ordinal-1) }
                        Label { text: fileApp.preview.count ? (Number(fileApp.preview.ordinal)+1) + "/" + fileApp.preview.count : ""; color: "#9fb2c5" }
                        Button { text: "›"; implicitWidth: 34; Accessible.name: "Next passage"; enabled: !fileApp.previewLoading && Number(fileApp.preview.ordinal)+1 < Number(fileApp.preview.count); onClicked: fileApp.loadPreview(fileApp.preview.fileId,0,Number(fileApp.preview.ordinal)+1) }
                    }
                    ScrollView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                        TextArea { text: fileApp.preview.text || "Select a result to inspect its indexed text. Content notices appear above."; textFormat: TextEdit.PlainText; readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap; color: "#d7e3ee"; font.pixelSize: 13; background: null; Accessible.name: "Extracted file text" }
                    }
                }
            }
            ProgressBar { visible: fileApp.busy; indeterminate: true; Layout.fillWidth: true }
            Label { text: fileApp.status; textFormat: Text.PlainText; color: "#9fb2c5"; Layout.fillWidth: true; wrapMode: Text.WordWrap; font.pixelSize: 12 }
        }
    }
}
