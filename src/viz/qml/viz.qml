import QtQuick
import QtQuick.Window
import QtQuick.Effects

Window {
    id: root
    visible: true
    color: "#05060a"
    title: "VJVCPlus Visualizer"
    width: 960
    height: 540
    minimumWidth: 320
    minimumHeight: 240

    // ---------- Mock fallback for qmlscene preview ----------
    // When loaded from qmlscene (no C++ context), `viz` is undefined.
    // Provide a self-contained mock so the full visual layout can be
    // tweaked live without recompiling.
    // All mock-exposed properties live under `mx` so they never collide
    // with Window's built-ins (title, visible, color etc.).
    property bool _mock: (typeof viz === "undefined")
    property var _mockBins: (function(){
        var arr = new Array(64);
        for (var i = 0; i < 64; ++i) arr[i] = 0.2 + 0.3 * Math.sin(i * 0.4);
        return arr;
    })()
    property real _mockPeak: 0.5
    Timer {
        id: _mockAnim
        interval: 50; repeat: true; running: root._mock && root.visible
        onTriggered: {
            var arr = new Array(64);
            var t = Date.now() / 500;
            for (var i = 0; i < 64; ++i) {
                var env = Math.exp(-Math.pow((i - 32) / 20, 2));
                arr[i] = Math.max(0, env * (0.5 + 0.5 * Math.sin(t + i * 0.3)) * 0.8);
            }
            root._mockBins = arr;
            root._mockPeak = 0.3 + 0.4 * Math.abs(Math.sin(t * 0.5));
        }
    }
    // All downstream bindings read through `mx` — a thin indirection that
    // picks mock or real viz values. Downstream uses mx.xxx, never viz.xxx.
    QtObject {
        id: mx
        readonly property bool hasTrack: root._mock ? true : viz.hasTrack
        readonly property string titleText: root._mock ? "fortissimo -the ultimate crisis-" : viz.title
        readonly property string artistText: root._mock ? "fripSide" : viz.artist
        readonly property string albumText: root._mock ? "Decade" : viz.album
        readonly property string coverPath: root._mock ? "" : viz.coverPath
        readonly property bool tentative: root._mock ? false : viz.tentative
        readonly property real peak: root._mock ? root._mockPeak : viz.peak
        readonly property var bins: root._mock ? root._mockBins : viz.bins
        readonly property string standbyPath: root._mock ? "" : viz.standbyPath
        readonly property string bgVideoPath: root._mock ? "" : viz.bgVideoPath
        readonly property var trackColors: root._mock ? [Qt.rgba(0.87,0.89,0.84,1), Qt.rgba(0.18,0.16,0.15,1), Qt.rgba(0.83,0.79,0.69,1)] : viz.trackColors
        readonly property color trackColor: root._mock ? Qt.rgba(0.87,0.89,0.84,1) : viz.trackColor
        // Flat RGB from C++ standby/logo color sampling:
        // [r0,g0,b0, r1,g1,b1, r2,g2,b2]  (ints 0-255)
        readonly property var effectiveColors: root._mock
            ? [224, 235, 255, 255, 179, 102, 115, 230, 191]
            : viz.effectiveColors
    }

    // F toggles fullscreen on the screen the window is currently on.
    // Drag the window to any monitor first → F fullscreen there.
    Shortcut {
        sequence: "F"
        onActivated: {
            if (root.visibility === Window.FullScreen)
                root.visibility = Window.Windowed
            else
                root.visibility = Window.FullScreen
        }
    }

    // ---------- Animation tuning (all adjustable, later bound from panel) ----------
    // Easing presets — Qt Quick supports ~40 types (In/Out/InOut × Linear..Bounce).
    // Indexed list so the panel can expose a ComboBox.
    readonly property var easingPresets: [
        "Linear", "InQuad", "OutQuad", "InOutQuad",
        "InCubic", "OutCubic", "InOutCubic",
        "InQuart", "OutQuart", "InOutQuart",
        "InQuint", "OutQuint", "InOutQuint",
        "InSine", "OutSine", "InOutSine",
        "InExpo", "OutExpo", "InOutExpo",
        "InCirc", "OutCirc", "InOutCirc",
        "InElastic", "OutElastic", "InOutElastic",
        "InBack", "OutBack", "InOutBack",
        "InBounce", "OutBounce", "InOutBounce"
    ]
    // Current easing preset index — defined in Tuning section below
    readonly property string animEasingName: root.easingPresets[root.animEasingIdx]
    readonly property var animEasingType: {
        switch (root.animEasingIdx) {
        case 0:  return Easing.Linear;
        case 1:  return Easing.InQuad;
        case 2:  return Easing.OutQuad;
        case 3:  return Easing.InOutQuad;
        case 4:  return Easing.InCubic;
        case 5:  return Easing.OutCubic;
        case 6:  return Easing.InOutCubic;
        case 7:  return Easing.InQuart;
        case 8:  return Easing.OutQuart;
        case 9:  return Easing.InOutQuart;
        case 10: return Easing.InQuint;
        case 11: return Easing.OutQuint;
        case 12: return Easing.InOutQuint;
        case 13: return Easing.InSine;
        case 14: return Easing.OutSine;
        case 15: return Easing.InOutSine;
        case 16: return Easing.InExpo;
        case 17: return Easing.OutExpo;
        case 18: return Easing.InOutExpo;
        case 19: return Easing.InCirc;
        case 20: return Easing.OutCirc;
        case 21: return Easing.InOutCirc;
        case 22: return Easing.InElastic;
        case 23: return Easing.OutElastic;
        case 24: return Easing.InOutElastic;
        case 25: return Easing.InBack;
        case 26: return Easing.OutBack;
        case 27: return Easing.InOutBack;
        case 28: return Easing.InBounce;
        case 29: return Easing.OutBounce;
        case 30: return Easing.InOutBounce;
        default: return Easing.OutCubic;
        }
    }

    // ================================================================
    // VISUAL TUNING DEFAULTS — confirmed 2026-09-10 by user
    // Later bound to control-panel knobs + JSON-persisted prefs.
    // ================================================================

    // --- Standby image ---
    //   idle (no track): centered, 100% scale (image fills window)
    //   playing bottom-centered — separate tunables for landscape / portrait
    property real standbySizeStandbyRatio: 1.00
    property real standbySizePlayingLandRatio: 0.29    // landscape playing logo size
    property real standbySizePlayingPortRatio: 0.40    // portrait playing logo size (bigger)
    property real standbyYStandbyRatio: 0.50           // vertical center when idle
    property real standbyYPlayingLandRatio: 0.90       // landscape: near bottom
    property real standbyYPlayingPortRatio: 0.94       // portrait: further down

    // --- Animation durations (ms) ---
    property int standbyAnimDuration: 1100       // logo shrink/grow — OutCubic bezier
    property int fadeInDelay: 400
    property int fadeInDuration: 600

    // --- Easing (index into easingPresets[]; default 5 = OutCubic) ---
    property int animEasingIdx: 5

    // --- Cover size ---
    property real coverSizeLandRatio: 0.25        // min(window.h*0.45, window.w*0.25)
    property real coverMaxHeightLandRatio: 0.45
    property real coverSizePortRatio: 0.40

    // --- Text ---
    property real titleSizeLandVmin: 0.055
    property real artistSizeLandVmin: 0.028
    property real albumSizeLandVmin: 0.020
    property real infoHLandRatio: 0.22

    // --- Spectrum ---
    property real spectrumHLandRatio: 0.35
    property real spectrumBarHRatio: 0.77         // main bar / total spectrum height
    property real mirrorHeightRatio: 0.55         // mirror height as × main-bar height

    // --- Background video ---
    property real bgBlurAmount: 0.40               // 0 = sharp, 1 = max blur
    property bool bgBlurEnabled: mx.bgVideoPath !== ""  // auto-enables when bg video present
    property real bgVideoSaturation: 1.0           // 0 = grayscale, 1 = normal

    // --- Auto-return to standby after N seconds of silence ---
    property int silenceTimeoutSec: 15

    // ================================================================

    // ---------- Content fade-in/out + logo sequencing ----------
    // We drive BOTH contentProgress AND the two logo anim properties
    // from SequentialAnimation so we can sequence them:
    //   fade-in  : [pause fadeInDelay] → content fade-in → logo shrink (parallel)
    //   fade-out : content fade-out → pause 200ms → logo grow
    // NO property bindings drive these (bindings would short-circuit
    // animations instantly). Only the SequentialAnimation nodes.
    property real contentProgress: 0.0
    // Direct anim props — NOT bound to standbySizeRatio; SequentialAnimation
    // nodes drive them explicitly so timing is controllable.
    property real standbySizeAnim: 1.00   // start at standby size
    property real standbyYAnim: 0.50      // start at standby Y

    // Fade-in: [pause fadeInDelay] → content 0→1 (parallel with logo shrink)
    SequentialAnimation {
        id: fadeInSeq
        running: false
        PauseAnimation { duration: root.fadeInDelay }
        ParallelAnimation {
            NumberAnimation {
                target: root; property: "contentProgress"
                from: 0; to: 1
                duration: root.fadeInDuration
                easing.type: root.animEasingType
            }
            NumberAnimation {
                target: root; property: "standbySizeAnim"
                from: root.standbySizeStandbyRatio
                to: root.landscape ? root.standbySizePlayingLandRatio
                                   : root.standbySizePlayingPortRatio
                duration: root.standbyAnimDuration
                easing.type: root.animEasingType
            }
            NumberAnimation {
                target: root; property: "standbyYAnim"
                from: root.standbyYStandbyRatio
                to: root.landscape ? root.standbyYPlayingLandRatio
                                   : root.standbyYPlayingPortRatio
                duration: root.standbyAnimDuration
                easing.type: root.animEasingType
            }
        }
    }
    // Fade-out: content 1→0 → pause 200ms → logo grow
    SequentialAnimation {
        id: fadeOutSeq
        running: false
        NumberAnimation {
            target: root; property: "contentProgress"
            from: 1; to: 0
            duration: root.standbyAnimDuration
            easing.type: root.animEasingType
        }
        PauseAnimation { duration: 200 }
        ParallelAnimation {
            NumberAnimation {
                target: root; property: "standbySizeAnim"
                to: root.standbySizeStandbyRatio
                duration: root.standbyAnimDuration
                easing.type: root.animEasingType
            }
            NumberAnimation {
                target: root; property: "standbyYAnim"
                to: root.standbyYStandbyRatio
                duration: root.standbyAnimDuration
                easing.type: root.animEasingType
            }
        }
    }
    Connections {
        target: mx
        function onHasTrackChanged() {
            fadeInSeq.stop()
            fadeOutSeq.stop()
            if (mx.hasTrack) {
                // Reset to standby state before fade-in starts
                root.contentProgress = 0
                root.standbySizeAnim = root.standbySizeStandbyRatio
                root.standbyYAnim = root.standbyYStandbyRatio
                fadeInSeq.start()
            } else {
                fadeOutSeq.start()
            }
        }
    }

    // Escape returns to the control panel (session stop is owned by
    // VizController on the GUI thread).
    Shortcut {
        sequence: "Escape"
        onActivated: { if (!root._mock) viz.requestClose(); else root.close(); }
    }

    readonly property bool landscape: root.width > root.height
    readonly property real vmin: Math.min(root.width, root.height)

    // ---------- Layout metrics ----------
    // Plain x/y bindings throughout (QML anchor switching via ternary
    // `undefined` does NOT clear old anchors — see history).
    //
    // Landscape (reference-image style):
    //   Cover on the LEFT — large square, vertically centered.
    //   Text + spectrum on the RIGHT — share a content area that starts
    //   a gap away from the cover and runs to the right edge.
    readonly property real gap: root.vmin * 0.025
    // Cover: roughly 25% of window width, capped by 45% of height.
    readonly property real coverSizeLand: Math.min(root.height * 0.45, root.width * 0.25)
    readonly property real coverXLand: root.gap
    readonly property real coverYLand: (root.height - root.coverSizeLand) / 2
    // Content area (text + spectrum) sits to the right of the cover.
    readonly property real contentXLand: root.coverXLand + root.coverSizeLand + root.gap
    readonly property real contentWLand: root.width - root.contentXLand - root.gap
    // Content-area layout: text on top, spectrum below. Heights are
    // fractions of window height so the bars have room to grow.
    readonly property real infoHLand: root.height * 0.22
    readonly property real spectrumHLand: root.height * 0.35

    // Portrait: cover on top ~40%.
    readonly property real coverSizePort: Math.min(root.height * 0.40, root.width * 0.80)
    readonly property real coverYPort: root.height * 0.05
    readonly property real textYPort: root.coverYPort + root.coverSizePort + root.gap
    readonly property real spectrumYPort: root.height * 0.55
    readonly property real spectrumHPort: root.height * 0.40

    // Cover-derived hue used by background ripple / accents.
    property int trackHue: {
        var h = 0;
        for (var i = 0; i < mx.titleText.length; ++i) h = (h * 31 + mx.titleText.charCodeAt(i)) % 360;
        return mx.titleText.length > 0 ? h : 210;
    }

    // Background tint sampled from cover (dominant color). Falls back to
    // trackHue if no art. Later bound to actual cover dominant color.
    property color bgTint: {
        if (mx.coverPath !== "") return Qt.hsla(root.trackHue / 360, 0.55, 0.32, 1)
        return Qt.hsla(root.trackHue / 360, 0.45, 0.22, 1)
    }

    // ---------- Layered background (direct Window children) ----------
    // Order (back → front): solid base → ripple → dark vignette
    // Removed bgRoot wrapper — nesting behind an Item can mask rendering.

        // --- Video layer (optional, bottom) ---
        // Requires Qt Multimedia module (not installed yet).
        // When enabled, place bg_video.mp4 next to the exe and set
        // bgVideoPath via control panel.
        // TODO: uncomment + add Qt6::Multimedia dep when ready
        /*
        Item {
            id: videoLayer
            anchors.fill: parent
            visible: mx.bgVideoPath !== ""
            MultiEffect {
                anchors.fill: parent
                source: bgVideo
                blurEnabled: root.bgBlurEnabled
                blurMax: 128
                blur: root.bgBlurAmount
                saturation: root.bgVideoSaturation
            }
            MediaPlayer {
                id: bgVideoPlayer
                source: mx.bgVideoPath
                loops: MediaPlayer.Infinite
                volume: 0
                onErrorOccurred: console.log("bg video error:", errorString)
            }
            VideoOutput {
                id: bgVideo
                anchors.fill: parent
                source: bgVideoPlayer
                fillMode: VideoOutput.PreserveAspectCrop
                Component.onCompleted: bgVideoPlayer.play()
            }
        }
        */

        // --- Ripple layer (always present, on top of video) ---
        Rectangle {
            anchors.fill: parent
            color: "#0a0f1a"
        }
        Canvas {
            id: ripple
            anchors.fill: parent
            contextType: "2d"
            renderStrategy: Canvas.Immediate
            opacity: 1.0

            // Persistent lerp state — survives across onPaint calls.
            // 9-element flat RGB: [r0,g0,b0, r1,g1,b1, r2,g2,b2]
            property var curRgb: [80, 80, 120, 80, 80, 120, 80, 80, 120]

            // Lerp factor: smaller → slower color fade transition
            property real fadeLerp: 0.06

            // Timer-driven repaint (60fps). requestPaint self-trigger can
            // stall on some platforms — explicit Timer is reliable.
            Timer {
                id: rippleTimer
                interval: 16
                repeat: true
                running: true
                onTriggered: ripple.requestPaint()
            }

            onPaint: {
                var ctx = getContext("2d")
                var w = width, h = height
                ctx.reset()

                // Absolute time — keeps wave motion continuous even when
                // colors are fading. No phase jump on color changes.
                var t = (Date.now() / 1000.0)

                // --- Color lerp for fade-in transition ---
                // mx.effectiveColors is a QVariantList from C++:
                //   flat [r0,g0,b0, r1,g1,b1, r2,g2,b2]  (ints 0-255)
                var target = mx.effectiveColors
                var cur = curRgb
                if (target && target.length >= 9) {
                    var k = fadeLerp
                    for (var i = 0; i < 9; ++i) {
                        cur[i] = cur[i] + (target[i] - cur[i]) * k
                        if (cur[i] < 0) cur[i] = 0
                        if (cur[i] > 255) cur[i] = 255
                    }
                } else {
                    // Fallback hard-coded colors (used when mx is mock)
                    cur = [224, 235, 255, 255, 179, 102, 115, 230, 191]
                }
                // Persist lerp state for next frame
                curRgb = cur

                // Helper: build layer rgb array from flat list
                function layerRgb(idx) {
                    return [Math.round(cur[idx*3]),
                            Math.round(cur[idx*3+1]),
                            Math.round(cur[idx*3+2])]
                }

                // Web Canvas string format — Qt.rgba() value-type is NOT
                // recognized by Canvas 2D addColorStop/fillStyle.
                function rgbaStr(rgb, a) {
                    return "rgba(" + rgb[0] + "," + rgb[1] + "," + rgb[2] + "," + a + ")"
                }

                var waveforms = [
                    function(x, t, l) { return Math.sin(x * 0.0025 + t * 0.21 + l * 0.7) * 0.7
                                                 + Math.sin(x * 0.006 + t * 0.30) * 0.3 },
                    function(x, t, l) { return Math.sin(x * 0.004 + t * 0.30 + l * 1.1) * 0.6
                                                 + Math.sin(x * 0.009 + t * 0.42) * 0.25 },
                    function(x, t, l) { return Math.sin(x * 0.005 + t * 0.42 + l * 1.5) * 0.5
                                                 + Math.sin(x * 0.011 + t * 0.54) * 0.25
                                                 + Math.sin(x * 0.002 - t * 0.15) * 0.3 }
                ]

                var ampArr   = [h * 0.05, h * 0.04, h * 0.03]
                var yOffArr  = [h * 0.27, h * 0.42, h * 0.58]
                var alphaArr = [0.28, 0.22, 0.18]

                // PAINTER'S ALGORITHM: draw BOTTOM layer first (2), MIDDLE (1), TOP last (0).
                for (var li = 2; li >= 0; --li) {
                    ctx.beginPath()
                    ctx.moveTo(0, h)
                    ctx.lineTo(0, yOffArr[li])

                    for (var x = 0; x <= w; x += 3) {
                        var y = yOffArr[li] + waveforms[li](x, t, li) * ampArr[li]
                        ctx.lineTo(x, y)
                    }

                    ctx.lineTo(w, h)
                    ctx.closePath()

                    var lc = layerRgb(li)
                    var a = alphaArr[li]

                    // Gradient from crest (wave top) down to screen bottom:
                    //   - crest: full alpha → wave line is clearly visible
                    //   - below crest: alpha fades smoothly → soft tail
                    var crestY = yOffArr[li] - ampArr[li]
                    var grad = ctx.createLinearGradient(0, crestY, 0, h)
                    grad.addColorStop(0.00, rgbaStr(lc, a))
                    grad.addColorStop(0.15, rgbaStr(lc, a * 0.6))
                    grad.addColorStop(0.50, rgbaStr(lc, a * 0.2))
                    grad.addColorStop(1.00, rgbaStr(lc, 0.0))
                    ctx.fillStyle = grad
                    ctx.fill()
                }
            }
        }

        // --- Dark vignette (kept very subtle so ripple shows through) ---
        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#00000000" }
                GradientStop { position: 0.5; color: "#00000000" }
                GradientStop { position: 1.0; color: "#00000000" }
            }
        }

    // ---------- Spectrum (mirrored bars) ----------
    // Landscape: pushed to the right of the cover, gutter-matched.
    // Portrait: bottom-anchored strip.
    Item {
        id: spectrum
        x: root.landscape ? root.contentXLand : root.gap
        y: root.landscape ? (root.coverYLand + root.infoHLand + root.gap) : root.spectrumYPort
        width: root.landscape ? root.contentWLand : (root.width - root.gap * 2)
        height: root.landscape ? root.spectrumHLand : root.spectrumHPort
        opacity: root.contentProgress * (mx.hasTrack ? 0.95 : 0.35)
        Behavior on opacity { NumberAnimation { duration: 400 } }

        // baseline: bars grow up from here, faint mirror grows down.
        // Main-bar region [0..barH], mirror region [barH..height].
        readonly property real barH: height * 0.77
        readonly property real barBaselineY: barH
        readonly property real barMaxH: barH

        Row {
            anchors.fill: parent
            spacing: spectrum.width / 64 * 0.25
            Repeater {
                model: 64
                delegate: Item {
                    width: (spectrum.width - parent.spacing * 63) / 64
                    height: spectrum.height
                    readonly property real v: (mx.bins[index] !== undefined ? mx.bins[index] : 0)
                    property real cap: 0
                    Timer { interval: 40; repeat: true; running: true
                        onTriggered: parent.cap = Math.max(parent.v * 0.92, parent.cap * 0.965) }

                    Rectangle { // mirror (reflection below baseline), gradient fade-out
                        x: 0; width: parent.width
                        y: spectrum.barBaselineY
                        height: parent.v * spectrum.barMaxH * 0.55
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#387fa8ff" }
                            GradientStop { position: 1.0; color: "#007fa8ff" }
                        }
                    }
                    Rectangle { // main bar
                        x: 0; width: parent.width
                        y: spectrum.barBaselineY - height
                        height: parent.v * spectrum.barMaxH
                        gradient: Gradient {
                            GradientStop { position: 0.0; color: "#9fc0ff" }
                            GradientStop { position: 1.0; color: Qt.hsla(root.trackHue / 360, 0.7, 0.6, 1) }
                        }
                        Behavior on height { SmoothedAnimation { duration: 100 } }
                    }
                    Rectangle { // peak-hold cap
                        x: 0; width: parent.width; height: 3
                        y: spectrum.barBaselineY - parent.cap * spectrum.barMaxH - 5
                        color: "#ffffff"
                        opacity: parent.cap > 0.03 ? 0.9 : 0
                    }
                }
            }
        }
    }

    // ---------- Track info + circular cover ----------
    Item {
        id: info
        // NO visible binding — let opacity Behavior handle fade-out.
        // hasTrack flips false → visible=瞬断 kills the fade animation.
        opacity: root.contentProgress
        Behavior on opacity { NumberAnimation { duration: 400 } }
        anchors.fill: parent

        // circular cover. NB: plain x/y bindings only.
        Item {
            id: coverHolder
            width: root.landscape ? root.coverSizeLand : root.coverSizePort
            height: width
            x: root.landscape ? root.coverXLand : (root.width - width) / 2
            y: root.landscape ? root.coverYLand : root.coverYPort
            z: -1

            // Single-layer circular cover:
            //   - Rectangle placeholder (colored circle + ♪ glyph, shows
            //     when no art; transparent when art is present so the
            //     Image's circular mask is the only visible shape)
            //   - Real art Image rotated + masked into a perfect circle
            // Plain clip:true does NOT respect Rectangle radius in Qt 6,
            // so the art is masked via layer.effect MultiEffect.
            Rectangle {
                id: coverCircle
                anchors.fill: parent
                radius: width / 2
                color: mx.coverPath === ""
                     ? Qt.hsla(root.trackHue / 360, 0.55, 0.35, 1)
                     : "transparent"

                Text {
                    anchors.centerIn: parent
                    text: "♪"
                    font.pixelSize: coverHolder.width * 0.45
                    color: "#e8eeff"
                    visible: mx.coverPath === ""
                }
                Image {
                    id: coverImage
                    anchors.fill: parent
                    source: mx.coverPath
                    fillMode: Image.PreserveAspectCrop
                    visible: mx.coverPath !== ""
                    asynchronous: true
                    cache: false
                    layer.enabled: true
                    layer.effect: MultiEffect {
                        maskEnabled: true
                        maskSource: coverMask
                    }
                    // 0.25 rev/s = 4s/rev, matching Python default.
                    // Keep rotating during fade-out — only stops when
                    // there genuinely is no cover image at all.
                    NumberAnimation on rotation {
                        from: 0; to: 360; duration: 4000
                        loops: Animation.Infinite
                        running: mx.coverPath !== ""
                    }
                }
            }
            // Hidden white circle mask — only used as maskSource.
            Rectangle {
                id: coverMask
                anchors.fill: parent
                radius: width / 2
                color: "white"
                visible: false
                layer.enabled: true
                layer.smooth: true
            }
        }

        Column {
            id: textBlock
            x: root.landscape ? root.contentXLand : (root.width - width) / 2
            y: root.landscape ? root.coverYLand : root.textYPort
            width: root.landscape ? root.contentWLand : root.width * 0.9
            height: root.landscape ? root.infoHLand : implicitHeight
            spacing: root.vmin * 0.02

            Text {
                width: textBlock.width
                horizontalAlignment: root.landscape ? Text.AlignLeft : Text.AlignHCenter
                text: mx.titleText
                font { family: "Microsoft YaHei"; pixelSize: root.landscape ? root.vmin * 0.055 : root.vmin * 0.08; bold: true }
                color: "#ffffff"
                elide: Text.ElideRight
            }
            Text {
                width: textBlock.width
                horizontalAlignment: root.landscape ? Text.AlignLeft : Text.AlignHCenter
                text: mx.artistText !== "" ? mx.artistText : "未知艺术家"
                font { family: "Microsoft YaHei"; pixelSize: root.landscape ? root.vmin * 0.028 : root.vmin * 0.04 }
                color: "#a9b6e8"
                elide: Text.ElideRight
            }
            Text {
                width: textBlock.width
                horizontalAlignment: root.landscape ? Text.AlignLeft : Text.AlignHCenter
                text: mx.tentative ? "识别中…" : "♫ " + (mx.albumText !== "" ? mx.albumText : "")
                font { family: "Microsoft YaHei"; pixelSize: root.landscape ? root.vmin * 0.020 : root.vmin * 0.03 }
                color: mx.tentative ? "#ffd166" : "#6b76a8"
                visible: mx.tentative || mx.albumText !== ""
            }

            // tentative pulsing: fade the whole text block
            NumberAnimation on opacity {
                running: mx.hasTrack && mx.tentative
                from: 1.0; to: 0.35; duration: 650
                loops: Animation.Infinite
                easing.type: Easing.InOutSine
            }
        }
    }

    // ---------- Standby image (alpha / gif) ----------
    // Animated via ratio properties (defined above in Tuning section).
    // Window resize snaps instantly; hasTrack transitions glide smoothly.

    // Important design rule:
    //   The standby image is displayed 1:1 "as-is" — we NEVER crop,
    //   stretch, or reshape it. standbySizeAnim is a scale factor,
    //   not a shape mandate. The image's own aspect ratio decides
    //   whether we scale by width or height.

    Item {
        id: standbyItem
        visible: mx.standbyPath !== "" || root._mock

        // Aspect from the loaded image itself — we NEVER reshape/crop.
        readonly property real wSrc: Math.max(1, standbyImg.sourceSize.width)
        readonly property real hSrc: Math.max(1, standbyImg.sourceSize.height)
        readonly property real aspect: wSrc / hSrc

        // Use standbySizeAnim / standbyYAnim — these already have
        // Behavior NumberAnimation for smooth glide on hasTrack changes.
        width:  Math.min(root.width * root.standbySizeAnim,
                         (root.height * root.standbySizeAnim) * aspect)
        height: width / aspect

        x: (root.width - width) / 2
        y: root.height * root.standbyYAnim - height / 2
        z: 100

        Image {
            id: standbyImg
            anchors.fill: parent
            source: mx.standbyPath
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: false
            visible: mx.standbyPath !== ""
            opacity: 0.9
        }
    }

}
