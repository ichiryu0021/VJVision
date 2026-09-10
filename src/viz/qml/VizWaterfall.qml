// Mirrored Bars — existing mode: bars grow up from baseline,
// with a faint mirror reflection below. Kept as the default.
import QtQuick 6.0

Item {
    id: root
    anchors.fill: parent

    // Inherit shared data from parent Loader
    readonly property var binsData: parent.bins
    readonly property real peakData: parent.peak
    readonly property int trackHue: parent.trackHue

    // baseline: bars grow up from here, faint mirror grows down.
    readonly property real barH: height * 0.77
    readonly property real barBaselineY: barH
    readonly property real barMaxH: barH

    Row {
        id: row
        anchors.fill: parent
        spacing: width / 64 * 0.25
        Repeater {
            model: 64
            delegate: Item {
                width: (row.width - row.spacing * 63) / 64
                height: row.height
                readonly property real v: (root.binsData !== undefined
                    && root.binsData[index] !== undefined
                    ? root.binsData[index] : 0)
                property real cap: 0
                Timer { interval: 40; repeat: true; running: true
                    onTriggered: parent.cap = Math.max(parent.v * 0.92, parent.cap * 0.965) }

                Rectangle { // mirror (reflection below baseline), gradient fade-out
                    x: 0; width: parent.width
                    y: root.barBaselineY
                    height: parent.v * root.barMaxH * 0.55
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "#387fa8ff" }
                        GradientStop { position: 1.0; color: "#007fa8ff" }
                    }
                }
                Rectangle { // main bar
                    x: 0; width: parent.width
                    y: root.barBaselineY - height
                    height: parent.v * root.barMaxH
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "#9fc0ff" }
                        GradientStop { position: 1.0; color: Qt.hsla(root.trackHue / 360, 0.7, 0.6, 1) }
                    }
                    Behavior on height { SmoothedAnimation { duration: 100 } }
                }
                Rectangle { // peak-hold cap
                    x: 0; width: parent.width; height: 3
                    y: root.barBaselineY - parent.cap * root.barMaxH - 5
                    color: "#ffffff"
                    opacity: parent.cap > 0.03 ? 0.9 : 0
                }
            }
        }
    }
}
