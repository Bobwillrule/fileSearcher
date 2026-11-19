import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: panel
    property var cleanup
    property var indexedRoots: []
    property double selectedFileId: 0
    signal openFileRequested(double id)
    title: "Duplicates and cleanup review"
    modal: true
    standardButtons: Dialog.Close
    Connections { target: panel.cleanup; function onReviewReady() { review.open() } }
    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        Label { text: "Exact copies are checked byte for byte. Similarity is not a reason to delete. Nothing is recycled without your separate approval."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        RowLayout {
            Layout.fillWidth: true
            ComboBox { id: scope; objectName: "cleanupScope"; model: panel.indexedRoots; textRole: "path"; Layout.fillWidth: true; enabled: !panel.cleanup || !panel.cleanup.busy; Accessible.name: "Indexed folder to check" }
            Button { text: "Scan folder"; enabled: panel.cleanup && !panel.cleanup.busy && scope.currentIndex >= 0; onClicked: panel.cleanup.scan(panel.indexedRoots[scope.currentIndex].rootId, installers.checked, builds.checked) }
            Button { text: "Cancel work"; enabled: panel.cleanup && panel.cleanup.busy; onClicked: panel.cleanup.cancel() }
        }
        RowLayout {
            CheckBox { id: installers; text: "Include installers older than 180 days"; checked: false; enabled: panel.cleanup && !panel.cleanup.busy }
            CheckBox { id: builds; text: "Include known generated build files"; checked: false; enabled: panel.cleanup && !panel.cleanup.busy }
        }
        Label { text: panel.cleanup ? panel.cleanup.message : "Cleanup unavailable"; textFormat: Text.PlainText; wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#b7cce0" }
        ProgressBar { indeterminate: true; visible: panel.cleanup && panel.cleanup.busy; Layout.fillWidth: true }
        TabBar {
            id: tabs; objectName: "cleanupTabs"; Layout.fillWidth: true
            palette.dark: "#263747"
            palette.brightText: "#e6edf5"
            TabButton { text: "Exact copies / opted-in rules" }
            TabButton { text: "Similar / possible versions" }
            TabButton { text: "Cleanup history" }
        }
        StackLayout {
            currentIndex: tabs.currentIndex; Layout.fillWidth: true; Layout.fillHeight: true
            ScrollView {
                clip: true; contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    Label { visible: !panel.cleanup || panel.cleanup.candidates.length === 0; text: "Choose an indexed folder and scan. Hard links, linked paths and protected files are excluded. Estimates describe allocated bytes, not space freed by recycling."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Repeater {
                        model: panel.cleanup ? panel.cleanup.candidates : []
                        delegate: Frame {
                            required property var modelData
                            Layout.fillWidth: true
                            ColumnLayout {
                                width: parent.width
                                Label { text: modelData.kind === "exact" ? "Exact duplicate" : modelData.kind === "build" ? "Generated build file — review required" : "Old installer — review required"; font.bold: true }
                                Label { text: modelData.reason; textFormat: Text.PlainText; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                Label { text: "Keep: " + modelData.keep; visible: modelData.kind === "exact"; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; color: "#9dd7b1" }
                                Label { text: "Proposed recycle: " + modelData.path; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                                Label { text: "Allocated bytes: " + modelData.allocated + " · Modified: " + modelData.modified; textFormat: Text.PlainText; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                RowLayout {
                                    Button { text: "Open"; onClicked: panel.openFileRequested(modelData.fileId) }
                                    Button { text: "Review recycling"; enabled: !panel.cleanup.busy; onClicked: panel.cleanup.prepare(modelData.token, false) }
                                    Button { text: "Keep this copy instead…"; visible: modelData.kind === "exact"; enabled: !panel.cleanup.busy; onClicked: panel.cleanup.prepare(modelData.token, true) }
                                }
                            }
                        }
                    }
                }
            }
            ScrollView {
                clip: true; contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    Button { text: "Inspect selected search result"; enabled: panel.cleanup && !panel.cleanup.busy && panel.selectedFileId > 0; onClicked: panel.cleanup.inspect(panel.selectedFileId) }
                    Label { text: "Versions are inferred from names and indexed evidence. Dates are context, not proof of which file to keep. These suggestions have no recycle action."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Repeater {
                        model: panel.cleanup ? panel.cleanup.related : []
                        delegate: Frame {
                            required property var modelData
                            Layout.fillWidth: true
                            ColumnLayout {
                                width: parent.width
                                Label { text: modelData.kind + ": " + modelData.name; textFormat: Text.PlainText; font.bold: true; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                Label { text: modelData.path + "\n" + modelData.reason + "\nModified: " + modelData.modifiedLabel; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                                Label { text: modelData.snippet || "No indexed passage available."; textFormat: Text.PlainText; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                Button { text: "Open"; onClicked: panel.openFileRequested(modelData.fileId) }
                            }
                        }
                    }
                }
            }
            ScrollView {
                clip: true; contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    Button { text: "Refresh history"; enabled: panel.cleanup && !panel.cleanup.busy; onClicked: panel.cleanup.refresh() }
                    Label { text: "Newest 100 reviews. Interrupted actions are never retried automatically. Restore files using Windows Recycle Bin; there is no app-managed cleanup undo."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Repeater {
                        model: panel.cleanup ? panel.cleanup.history : []
                        delegate: Frame {
                            required property var modelData
                            Layout.fillWidth: true
                            ColumnLayout {
                                width: parent.width
                                Label { text: modelData.state + " · " + modelData.createdAt; textFormat: Text.PlainText; font.bold: true }
                                Label { text: "Proposed recycle: " + modelData.path + (modelData.keep ? "\nRetained copy: " + modelData.keep : "") + "\n" + modelData.message; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                                Button { text: "Cancel unstarted review"; visible: modelData.state === "planned"; enabled: !panel.cleanup.busy; onClicked: panel.cleanup.dismiss(modelData.id) }
                            }
                        }
                    }
                }
            }
        }
    }
    Dialog {
        id: review; objectName: "cleanupConfirmation"
        title: "Review recycling — files change only after approval"
        modal: true; anchors.centerIn: parent; width: Math.min(680,panel.width-32)
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: { standardButton(Dialog.Ok).text = "Send to Recycle Bin"; standardButton(Dialog.Cancel).forceActiveFocus() }
        onAccepted: if(panel.cleanup) panel.cleanup.approve(panel.cleanup.plan.id)
        onRejected: if(panel.cleanup && panel.cleanup.plan.id) panel.cleanup.dismiss(panel.cleanup.plan.id)
        contentHeight: Math.min(reviewContents.implicitHeight, panel.height - 160)
        contentItem: ScrollView {
            clip: true; contentWidth: availableWidth
            ColumnLayout {
                id: reviewContents; width: parent.width
                Label { text: "Keep: " + (panel.cleanup ? panel.cleanup.plan.keep || "No backup copy was verified. This is an opted-in rule; verify that this file is disposable." : ""); color: "#9dd7b1"; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                Label { text: "Send to Recycle Bin: " + (panel.cleanup ? panel.cleanup.plan.path || "" : ""); textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                Label { text: "The file and retained copy will be rechecked. Unsupported recycling stops the action. Windows Recycle Bin provides restoration; the app does not offer cleanup undo. Recycling alone does not free disk space."; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
        }
    }
}
