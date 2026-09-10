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
    //   idle (no track): centered, 50% vmin
    //   playing: bottom-centered, 29% vmin, 90% height anchor
    property real standbySizeStandbyRatio: 0.50
    property real standbySizePlayingRatio: 0.29
    property real standbyYStandbyRatio: 0.50      // vertical center
    property real standbyYPlayingRatio: 0.90      // near bottom

    // --- Animation durations (ms) ---
    property int standbyAnimDuration: 640
    property int fadeInDelay: 400
    property int fadeInDuration: 500

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

    // Derived standby ratio properties (bind to idle/playing defaults above)
    property real standbySizeRatio: mx.hasTrack ? root.standbySizePlayingRatio : root.standbySizeStandbyRatio
    property real standbySizeAnim: standbySizeRatio
    Behavior on standbySizeAnim { NumberAnimation {
        duration: root.standbyAnimDuration; easing.type: root.animEasingType } }

    property real standbyYRatio: mx.hasTrack ? root.standbyYPlayingRatio : root.standbyYStandbyRatio
    property real standbyYAnim: standbyYRatio
    Behavior on standbyYAnim { NumberAnimation {
        duration: root.standbyAnimDuration; easing.type: root.animEasingType } }

    // ---------- Delayed content fade-in ----------
    property real contentProgress: 0.0
    SequentialAnimation {
        id: fadeSeq
        NumberAnimation { target: root; property: "contentProgress"; to: 0.0; duration: 80 }
        PauseAnimation { duration: mx.hasTrack ? root.fadeInDelay : 200 }
        NumberAnimation { target: root; property: "contentProgress"; to: mx.hasTrack ? 1.0 : 0.0;
            duration: root.fadeInDuration; easing.type: root.animEasingType }
    }
    Connections {
        target: mx
        function onHasTrackChanged() { fadeSeq.restart(); }
    }

    // Kick off fade-in once at startup (esp. important in qmlscene mock
    // where hasTrack never changes — the Connections handler above only
    // fires when hasTrack actually toggles).
    Component.onCompleted: fadeSeq.start()

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

                // Colors from C++ cover sampler (top-N dominant)
                // User rule: Layer 0 ← largest cover color
                //             Layer 1 ← contrast color (complement of Layer 0)
                //             Layer 2 ← second-largest cover color
                var colors = mx.trackColors
                // Mock fallback (3 HSL swatches when no C++ context)
                if (!colors || colors.length < 2)
                    colors = [Qt.hsla(0.6, 0.55, 0.50, 1), Qt.hsla(0.3, 0.55, 0.50, 1)]
                function layerColor(layer) {
                    // Complement: 255 - channel, then clamp brightness so
                    // pure white / pure black complements don't blow out
                    function complementOf(c) {
                        var r = (255 - c.r) / 255, g = (255 - c.g) / 255, b = (255 - c.b) / 255
                        var maxCh = Math.max(r, Math.max(g, b))
                        if (maxCh > 0.92) { var f = 0.92 / maxCh; r *= f; g *= f; b *= f }
                        if (maxCh < 0.12) { var f2 = 0.12 / Math.max(maxCh, 0.01); r *= f2; g *= f2; b *= f2 }
                        return Qt.rgba(r, g, b, 1)
                    }
                    if (layer === 0) return colors[0]
                    if (layer === 1) return complementOf(colors[0])
                    if (layer === 2) return (colors.length > 1) ? colors[1] : colors[0]
                    return Qt.hsla((root.trackHue + layer * 60) / 360, 0.5, 0.45, 1)
                }

                // Base: dark tint from primary cover color (brightened so
                // ripple layers on top remain visible on near-black bg)
                var c0 = layerColor(0)
                ctx.fillStyle = Qt.rgba(c0.r / 255 * 0.40, c0.g / 255 * 0.40, c0.b / 255 * 0.42, 1.0)
                ctx.fillRect(0, 0, w, h)

                // Animated ripple waves — 3 layers, distinct waveforms/speeds/freqs
                var t = (Date.now() / 1000.0)

                // Layer 0: low-freq sine + soft harmonic (top, broad)
                // Layer 1: mid-freq, two harmonics (mid)
                // Layer 2: higher-freq, three harmonics for complexity (bottom)
                var waveforms = [
                    function(x, t, l) { return Math.sin(x * 0.0025 + t * 0.35 + l * 0.7) * 0.7
                                                 + Math.sin(x * 0.006 + t * 0.5) * 0.3 },
                    function(x, t, l) { return Math.sin(x * 0.004 + t * 0.5 + l * 1.1) * 0.6
                                                 + Math.sin(x * 0.009 + t * 0.7) * 0.25 },
                    function(x, t, l) { return Math.sin(x * 0.005 + t * 0.7 + l * 1.5) * 0.5
                                                 + Math.sin(x * 0.011 + t * 0.9) * 0.25
                                                 + Math.sin(x * 0.002 - t * 0.25) * 0.3 }
                ]

                var speedArr = [0.4, 0.55, 0.75]
                var ampArr   = [h * 0.18, h * 0.14, h * 0.10]
                var yOffArr  = [h * 0.35, h * 0.55, h * 0.75]
                var alphaArr = [0.85, 0.70, 0.60]

                for (var layer = 0; layer < 3; ++layer) {
                    ctx.beginPath()
                    ctx.moveTo(0, h)
                    ctx.lineTo(0, yOffArr[layer])

                    for (var x = 0; x <= w; x += 3) {
                        var y = yOffArr[layer] + waveforms[layer](x, t, layer) * ampArr[layer]
                        ctx.lineTo(x, y)
                    }

                    ctx.lineTo(w, h)
                    ctx.closePath()

                    var grad = ctx.createLinearGradient(0, yOffArr[layer] - ampArr[layer], 0, h)
                    var lc = layerColor(layer)
                    // Boost brightness so sampled colors show up on dark bg:
                    //   dark colors → lift to at least 45% lightness
                    //   bright colors → keep as-is
                    var r = lc.r / 255, g = lc.g / 255, b = lc.b / 255
                    var maxCh = Math.max(r, Math.max(g, b))
                    if (maxCh < 0.45) {
                        var f = 0.45 / Math.max(maxCh, 0.05)
                        r = Math.min(1, r * f); g = Math.min(1, g * f); b = Math.min(1, b * f)
                    }
                    var top = Qt.rgba(r, g, b, alphaArr[layer])
                    var bot = Qt.rgba(r * 0.4, g * 0.4, b * 0.4, 0)
                    grad.addColorStop(0, top)
                    grad.addColorStop(1, bot)
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
        visible: mx.hasTrack
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
                    NumberAnimation on rotation {
                        from: 0; to: 360; duration: 4000
                        loops: Animation.Infinite
                        running: mx.hasTrack && mx.coverPath !== ""
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

    Item {
        id: standbyItem
        visible: mx.standbyPath !== "" || root._mock
        width: root.vmin * root.standbySizeAnim
        height: width
        x: (root.width - width) / 2
        y: root.height * root.standbyYAnim - height / 2
        opacity: 1.0

        // Fallback placeholder if no standby image configured.
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "#0a0e1a"
            border.color: "#3a4a7a"
            border.width: 1
            opacity: mx.standbyPath === "" ? 1.0 : 0.0
            Behavior on opacity { SmoothedAnimation { duration: 300 } }
        }
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
