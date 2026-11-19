import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Dialog {
    id: panel
    property var organizer
    property double selectedFileId: 0
    signal openFileRequested(double id)
    title: "Related files and organization"
    modal: true
    standardButtons: Dialog.Close
    function editCategory(category) {
        categoryEditor.categoryId = category ? category.id : 0
        categoryName.text = category ? category.name : ""
        categoryDescription.text = category ? category.description : ""
        categoryDestination.text = category ? category.destination : ""
        categoryEditor.open()
    }
    Connections {
        target: panel.organizer
        function onMovePlanReady() { moveConfirmation.open() }
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        RowLayout {
            Button { text: "Inspect selected result"; enabled: panel.organizer && !panel.organizer.busy && panel.selectedFileId > 0; onClicked: panel.organizer.inspect(panel.selectedFileId) }
            Label { text: panel.organizer && panel.organizer.selected.path || "Choose a result in the search window first."; textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideMiddle }
        }
        Label { text: panel.organizer ? panel.organizer.message : "The local index is unavailable."; textFormat: Text.PlainText; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#9fb2c5" }
        ProgressBar { visible: panel.organizer && panel.organizer.busy; indeterminate: true; Layout.fillWidth: true }
        TabBar {
            id: tabs; objectName: "organizationTabs"; Layout.fillWidth: true
            palette.dark: "#263747"
            palette.brightText: "#e6edf5"
            TabButton { text: "Related and suggestions" }
            TabButton { text: "Categories" }
            TabButton { text: "Move history" }
        }
        StackLayout {
            currentIndex: tabs.currentIndex
            Layout.fillWidth: true; Layout.fillHeight: true
            ScrollView {
                clip: true; contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width; spacing: 10
                    Label { text: "Possible relationships"; font.bold: true }
                    Label { visible: !panel.organizer || panel.organizer.related.length === 0; text: "No strong indexed relationship found. Matching names or a shared folder alone may not be enough."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Repeater {
                        model: panel.organizer ? panel.organizer.related : []
                        delegate: Frame {
                            required property var modelData
                            Layout.fillWidth: true
                            ColumnLayout {
                                width: parent.width
                                RowLayout {
                                    Label { text: modelData.name; textFormat: Text.PlainText; font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
                                    Button { text: "Open"; onClicked: panel.openFileRequested(modelData.fileId) }
                                }
                                Label { text: modelData.path; textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideMiddle; color: "#9fb2c5" }
                                Label { text: modelData.reason; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: "#64d8be" }
                                Label { text: modelData.snippet || "Context match; no semantic passage is available."; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                            }
                        }
                    }
                    Label { text: "Category suggestion"; font.bold: true }
                    Label { visible: !panel.organizer || panel.organizer.suggestions.length === 0; text: "No new, unambiguous suggestion. You can still choose a virtual tag in Categories."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Repeater {
                        model: panel.organizer ? panel.organizer.suggestions : []
                        delegate: Frame {
                            required property var modelData
                            Layout.fillWidth: true
                            ColumnLayout {
                                width: parent.width
                                Label { text: modelData.name; textFormat: Text.PlainText; font.bold: true }
                                Label { text: modelData.reason; textFormat: Text.PlainText; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                Label { text: modelData.snippet || "Evidence appears in the filename."; textFormat: Text.PlainText; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#9fb2c5" }
                                Label { text: modelData.destination ? "Proposed folder: " + modelData.destination : "Virtual collection only; no destination set."; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                                RowLayout {
                                    enabled: !panel.organizer.busy
                                    Button { text: "Accept tag"; onClicked: panel.organizer.decide(modelData.id, true) }
                                    Button { text: "Reject"; onClicked: panel.organizer.decide(modelData.id, false) }
                                    Button { text: "Edit category"; onClicked: panel.editCategory(modelData) }
                                    Button { text: "Review move…"; enabled: modelData.destination.length > 0; onClicked: panel.organizer.prepareMove(modelData.destination) }
                                }
                            }
                        }
                    }
                    Button { text: "Choose a different move destination…"; enabled: panel.organizer && !panel.organizer.busy && !!panel.organizer.selected.fileId; onClicked: moveFolder.open() }
                }
            }
            ColumnLayout {
                RowLayout {
                    Button { text: "New category"; objectName: "newCategoryButton"; enabled: panel.organizer && !panel.organizer.busy; onClicked: panel.editCategory(null) }
                    Label { text: "Tags leave files in place. Examples must already be indexed to supply semantic evidence."; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#9fb2c5" }
                }
                ListView {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 10
                    model: panel.organizer ? panel.organizer.categories : []
                    ScrollBar.vertical: ScrollBar {}
                    delegate: Frame {
                        required property var modelData
                        width: ListView.view.width; height: categoryContent.implicitHeight + 24
                        ColumnLayout {
                            id: categoryContent; width: parent.width
                            Label { text: modelData.name + " · " + modelData.members + " tagged files"; textFormat: Text.PlainText; font.bold: true }
                            Label { text: modelData.description; textFormat: Text.PlainText; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                            Label { text: modelData.destination || "Virtual collection (no move destination)"; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; color: "#9fb2c5" }
                            Label { text: modelData.examples.length + " examples: " + modelData.examples.join("; "); textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; maximumLineCount: 3; elide: Text.ElideRight }
                            RowLayout {
                                enabled: !panel.organizer.busy
                                Button { text: "Edit"; onClicked: panel.editCategory(modelData) }
                                Button { text: "Members"; onClicked: { membersDialog.categoryId = modelData.id; membersDialog.title = modelData.name; panel.organizer.showMembers(modelData.id); membersDialog.open() } }
                                Button { text: "Add tag"; enabled: !!panel.organizer.selected.fileId; onClicked: panel.organizer.decide(modelData.id, true) }
                                Button { text: "Remove tag"; enabled: !!panel.organizer.selected.fileId; onClicked: panel.organizer.decide(modelData.id, false) }
                                Button { text: "Delete category"; onClicked: { deleteCategory.categoryId = modelData.id; deleteCategory.open() } }
                            }
                            RowLayout {
                                enabled: !panel.organizer.busy
                                Button { text: "Use inspected file as example"; enabled: !!panel.organizer.selected.fileId; onClicked: panel.organizer.addExample(modelData.id) }
                                Button { text: "Clear examples"; enabled: modelData.examples.length > 0; onClicked: panel.organizer.clearExamples(modelData.id) }
                            }
                        }
                    }
                }
            }
            ColumnLayout {
                Button { text: "Refresh and inspect interrupted moves"; enabled: panel.organizer && !panel.organizer.busy; onClicked: panel.organizer.refresh() }
                Label { text: "Recovery never deletes files automatically. Undo requires unchanged content and identity and an empty original path. Most recent 100 operations shown."; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: "#9fb2c5" }
                ListView {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 10
                    model: panel.organizer ? panel.organizer.history : []
                    ScrollBar.vertical: ScrollBar {}
                    delegate: Frame {
                        required property var modelData
                        width: ListView.view.width; height: historyContent.implicitHeight + 24
                        ColumnLayout {
                            id: historyContent; width: parent.width
                            Label { text: modelData.state + " · " + modelData.createdAt; textFormat: Text.PlainText; font.bold: true }
                            Label { text: "From: " + modelData.source + "\nTo: " + modelData.destination; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                            Label { text: modelData.message; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: "#9fb2c5" }
                            Label { visible: modelData.state === "needs_review"; text: "Temporary path: " + modelData.temporary; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
                            RowLayout {
                                enabled: !panel.organizer.busy
                                Button { text: "Review undo…"; visible: modelData.state === "completed" && !modelData.undoOf; onClicked: panel.organizer.prepareUndo(modelData.id) }
                                Button { text: modelData.state === "copy_ready" ? "Review and finish…" : "Review plan…"; visible: modelData.state === "planned" || modelData.state === "copy_ready"; onClicked: panel.organizer.reviewMove(modelData.id) }
                                Button { text: "Cancel plan"; visible: modelData.state === "planned"; onClicked: panel.organizer.cancelMove(modelData.id) }
                            }
                        }
                    }
                }
            }
        }
    }
    FolderDialog { id: moveFolder; title: "Choose the proposed destination"; onAccepted: panel.organizer.prepareMove(panel.organizer.localPath(selectedFolder)) }
    FolderDialog { id: categoryFolder; title: "Choose a category destination"; onAccepted: categoryDestination.text = panel.organizer.localPath(selectedFolder) }
    Dialog {
        id: categoryEditor; objectName: "categoryEditor"
        property double categoryId: 0
        title: categoryId ? "Edit category" : "New category"
        anchors.centerIn: parent; width: Math.min(600, panel.width - 40); modal: true
        standardButtons: Dialog.Save | Dialog.Cancel
        ColumnLayout {
            width: parent.width
            TextField { id: categoryName; objectName: "categoryName"; placeholderText: "Category name"; maximumLength: 80; Layout.fillWidth: true; Accessible.name: "Category name" }
            TextArea { id: categoryDescription; placeholderText: "Describe what belongs here, such as semiconductor diodes and rectifier circuits."; Layout.fillWidth: true; Layout.preferredHeight: 90; wrapMode: TextEdit.Wrap; Accessible.name: "Category description" }
            TextField { id: categoryDestination; placeholderText: "Optional existing destination folder"; Layout.fillWidth: true; Accessible.name: "Category destination" }
            Button { text: "Choose folder…"; onClicked: categoryFolder.open() }
            Label { text: "Leave the destination empty for a virtual collection. Files never move just because a category matches."; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: "#9fb2c5" }
        }
        onAccepted: panel.organizer.saveCategory(categoryId, categoryName.text, categoryDescription.text, categoryDestination.text)
    }
    Dialog {
        id: deleteCategory; property double categoryId: 0
        title: "Delete this virtual category?"; anchors.centerIn: parent; modal: true; width: 420
        standardButtons: Dialog.Ok | Dialog.Cancel
        Label { width: parent.width; text: "This removes its tags and examples. Original files remain unchanged."; wrapMode: Text.WordWrap }
        onAccepted: panel.organizer.removeCategory(categoryId)
    }
    Dialog {
        id: membersDialog; anchors.centerIn: parent; width: Math.min(700, panel.width - 40); height: Math.min(440, panel.height - 40); modal: true
        property double categoryId: 0
        standardButtons: Dialog.Close
        ListView {
            anchors.fill: parent; clip: true; spacing: 8; model: panel.organizer ? panel.organizer.members : []
            ScrollBar.vertical: ScrollBar {}
            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width
                Label { text: (modelData.available ? "" : "Unavailable: ") + modelData.path; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                Button { text: "Remove tag"; enabled: !panel.organizer.busy; onClicked: panel.organizer.removeMember(membersDialog.categoryId, modelData.path) }
            }
        }
    }
    Dialog {
        id: moveConfirmation; objectName: "moveConfirmation"
        title: "Approve this file move?"; anchors.centerIn: parent; width: Math.min(680, panel.width - 40); modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        ColumnLayout {
            width: parent.width
            Label { text: "Source: " + (panel.organizer ? panel.organizer.movePlan.source || "" : ""); textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
            Label { text: "Destination: " + (panel.organizer ? panel.organizer.movePlan.destination || "" : ""); textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere }
            Label { text: panel.organizer && panel.organizer.movePlan.state === "copy_ready" ? "A verified destination copy already exists. Approving finishes the move by removing the verified original." : "The source and destination will be checked again. Existing files are never overwritten. Undo will be available only while the moved file is unchanged."; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        }
        onAccepted: panel.organizer.approveMove(panel.organizer.movePlan.id)
        onRejected: { if(panel.organizer.movePlan.state === "planned") panel.organizer.cancelMove(panel.organizer.movePlan.id) }
    }
}
