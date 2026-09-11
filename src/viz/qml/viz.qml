import QtQuick
import QtQuick.Window
import QtQuick.Effects
import QtMultimedia       // MediaPlayer + VideoOutput (MP4/MOV bg)
// AnimatedImage lives in QtQuick core since Qt 6.2 — no extra import needed

Window {
    id: root
    visible: true
    color: "#05060a"
    title: "VJVision Visualizer"
    width: 960
    height: 540
    minimumWidth: 320
    minimumHeight: 240

    Component.onCompleted: {
        console.log("[viz.qml] mx.bgColor =", mx.bgColor,
                    "mx.bgOverlayDepth =", mx.bgOverlayDepth,
                    "mx.bgVideoPath =", mx.bgVideoPath,
                    "root.baseColor =", root.baseColor);
    }

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
        readonly property real bgOverlayDepth: root._mock ? 0.5 : viz.bgOverlayDepth
        readonly property string bgColor: root._mock ? "#000000" : viz.bgColor
        readonly property var trackColors: root._mock ? [Qt.rgba(0.87,0.89,0.84,1), Qt.rgba(0.18,0.16,0.15,1), Qt.rgba(0.83,0.79,0.69,1)] : viz.trackColors
        readonly property color trackColor: root._mock ? Qt.rgba(0.87,0.89,0.84,1) : viz.trackColor
        // Flat RGB from C++ standby/logo color sampling:
        // [r0,g0,b0, r1,g1,b1, r2,g2,b2]  (ints 0-255)
        readonly property var effectiveColors: root._mock
            ? [224, 235, 255, 255, 179, 102, 115, 230, 191]
            : viz.effectiveColors
        readonly property int vizMode: root._mock ? 0 : viz.vizMode
        readonly property int bgMode: root._mock ? 0 : viz.bgMode
        readonly property int fxTexture: root._mock ? 0 : viz.fxTexture
        readonly property int performanceMode: root._mock ? 0 : viz.performanceMode
        readonly property real logoSizeStandby: root._mock ? 1.0 : viz.logoSizeStandby
        readonly property real logoSizePlaying: root._mock ? 0.29 : viz.logoSizePlaying
    }

    // ---------- v2.0.4: Rhythm feature extractor (QML-side) ----------
    // Derives bass/mid/treble/energy/beat/onset from mx.bins (48 Mel bands).
    // Runs at 30fps via rhythmTimer. onset triggers fire when low-freq energy
    // exceeds EMA baseline + k*sqrt(var) with min 120ms refractory. beat is
    // an exp(-dt/tau) decay envelope (tau=180ms). Silence-protected.
    QtObject {
        id: rhythm
        property real bass: 0
        property real mid: 0
        property real treble: 0
        property real energy: 0
        property real beat: 0
        property bool onset: false

        // EMA state for onset detection
        property real bassMean: 0.01
        property real bassVar: 0.001
        property real lastOnsetMs: 0

        // Resolved performance mode (auto → heuristic, otherwise user pick)
        readonly property int resolvedPerf: {
            var p = mx.performanceMode
            if (p === 1) return 1   // high
            if (p === 2) return 2   // mid
            if (p === 3) return 3   // low
            // auto (p===0): coarse heuristic — 8+ cores → high, else mid
            // QML has no std::thread::hardware_concurrency, so use a fixed
            // mid default (v2.1.0 will move detection to C++).
            return 2
        }

        // Tunables
        readonly property real onsetK: 1.8       // sensitivity
        readonly property real onsetRefractoryMs: 120
        readonly property real beatTauMs: 180.0
        readonly property real silenceThresh: 1e-4

        property real lastTickMs: 0

        Timer {
            id: rhythmTimer
            interval: 33   // 30fps
            repeat: true
            running: true
            onTriggered: rhythm.update()
        }

        function update() {
            var bins = mx.bins
            var n = bins ? bins.length : 0
            if (n < 6) { bass = 0; mid = 0; treble = 0; energy = 0; beat = Math.max(0, beat - 0.02); onset = false; return }

            // Band aggregation (48 Mel bins: bass 0-5, mid 6-23, treble 24-47)
            var bSum = 0, mSum = 0, tSum = 0, allSum = 0
            for (var i = 0; i < 6; ++i) { var v = bins[i]; bSum += v; allSum += v }
            for (var i2 = 6; i2 < 24 && i2 < n; ++i2) { var v2 = bins[i2]; mSum += v2; allSum += v2 }
            for (var i3 = 24; i3 < n; ++i3) { var v3 = bins[i3]; tSum += v3; allSum += v3 }
            var bassVal = bSum / 6
            var midVal = mSum / Math.max(1, (Math.min(24, n) - 6))
            var trebleVal = tSum / Math.max(1, (n - 24))
            var energyVal = allSum / n
            bass = bassVal; mid = midVal; treble = trebleVal; energy = energyVal

            var now = Date.now()
            var dt = lastTickMs > 0 ? (now - lastTickMs) / 1000.0 : 0.033
            lastTickMs = now

            // Beat decay
            var decay = Math.exp(-dt * 1000.0 / beatTauMs)
            beat = beat * decay

            // Onset detection (EMA mean/var on bass)
            var alpha = 0.05
            var delta = bassVal - bassMean
            bassMean = bassMean + alpha * delta
            bassVar = (1 - alpha) * (bassVar + alpha * delta * delta)

            onset = false
            if (bassVal > silenceThresh && bassVal > bassMean + onsetK * Math.sqrt(Math.max(0, bassVar))) {
                if (now - lastOnsetMs > onsetRefractoryMs) {
                    onset = true
                    beat = 1.0
                    lastOnsetMs = now
                }
            }

            // Silence: beat fades to 0
            if (bassVal < silenceThresh) { beat = beat * 0.9 }
        }
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
    property real standbySizeStandbyRatio: mx.logoSizeStandby
        onStandbySizeStandbyRatioChanged: {
            // Slider changed — instantly apply to standby mode, but don't
            // clobber fade-in / fade-out animation that's currently running.
            if (!mx.hasTrack && !fadeInSeq.running && !fadeOutSeq.running)
                standbySizeAnim = standbySizeStandbyRatio
        }
    property real standbySizePlayingLandRatio: mx.logoSizePlaying
        onStandbySizePlayingLandRatioChanged: {
            if (mx.hasTrack && !fadeInSeq.running && !fadeOutSeq.running)
                standbySizeAnim = standbySizePlayingLandRatio
        }
    property real standbySizePlayingPortRatio: mx.logoSizePlaying * 1.38
        onStandbySizePlayingPortRatioChanged: {
            if (mx.hasTrack && !fadeInSeq.running && !fadeOutSeq.running)
                standbySizeAnim = standbySizePlayingPortRatio
        }
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

    // --- Background media (GIF/WEBP animated or MP4/MOV video) ---
    // bgMediaType: 0 = none, 1 = GIF/WEBP/PNG/Image (AnimatedImage), 2 = MP4/MOV (MediaPlayer)
    // QML picks the right component automatically — no Multimedia needed for GIF!
    property real bgOverlayDepth: 0.5              // 0 = no dim, 1 = fully black
    readonly property int bgMediaType: {
        var p = mx.bgVideoPath.toLowerCase();
        if (p === "") return 0;
        if (p.endsWith(".gif") || p.endsWith(".webp") || p.endsWith(".png") || p.endsWith(".jpg") || p.endsWith(".jpeg")) return 1;
        return 2;  // MP4/MOV/MKV → needs Multimedia
    }
    readonly property string bgImagePath: (bgMediaType === 1) ? mx.bgVideoPath : ""
    readonly property string bgMediaPath: (bgMediaType === 2) ? mx.bgVideoPath : ""

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

    // Default bg (no custom media): user-chosen color from panel, or pure black fallback.
    readonly property color baseColor: (mx.bgColor && mx.bgColor.length > 0)
        ? mx.bgColor : "#000000"

    // Background tint sampled from cover (dominant color). Falls back to
    // trackHue if no art. Later bound to actual cover dominant color.
    property color bgTint: {
        if (mx.coverPath !== "") return Qt.hsla(root.trackHue / 360, 0.55, 0.32, 1)
        return Qt.hsla(root.trackHue / 360, 0.45, 0.22, 1)
    }

    // ---------- Layered background (ALL direct Window children — no Item wrappers!) ----------
    // Qt Quick: EARLIER declaration = LOWER z = painted FIRST (back).
    //           LATER declaration = HIGHER z = painted LAST (front).
    // Order (back → front):
    //   1. Solid base color "#0a0f1a" (deep blue fallback, bottom-most)
    //   2. Ripple Canvas (semi-transparent animated color wash — OVERPAINTS bg slightly)
    //   3. Custom background media (AnimatedImage or VideoOutput — on top of ripple IF present)
    //   4. Dark vignette (gradient overlay — always top-most background effect)
    //   5. Spectrum bars / track info / logo ... (content, not background)

        // [1] Solid base — bottom-most. Always visible, even with custom bg (custom bg covers it).
        // Uses complement of track hue, darkened to near-black so foreground is legible.
        Rectangle {
            anchors.fill: parent
            color: root.baseColor
        }

        // [2] Ripple Canvas — semi-transparent color wash. ON TOP of base.
        //     IF custom bg present, ripple blends on top of video (may dim it slightly).
        Canvas {
            id: ripple
            anchors.fill: parent
            contextType: "2d"
            renderStrategy: Canvas.Immediate
            opacity: mx.bgVideoPath === "" ? 1.0 : 0.6

            // Persistent lerp state — survives across onPaint calls.
            // 9-element flat RGB: [r0,g0,b0, r1,g1,b1, r2,g2,b2]
            property var curRgb: [80, 80, 120, 80, 80, 120, 80, 80, 120]

            // Lerp factor: smaller → slower color fade transition
            property real fadeLerp: 0.06

            // Timer-driven repaint (30fps). requestPaint self-trigger can
            // stall on some platforms — explicit Timer is reliable.
            // Ambient waves barely differ at 30fps; halves CPU load.
            Timer {
                id: rippleTimer
                interval: 33
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

                    for (var x = 0; x <= w; x += 5) {
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
        }   // ← end of ripple Canvas

        // [2b] v2.0.4: FX texture Canvas — shown only when bgMode==2 (rhythm texture).
        //     Sits at the SAME z as ripple (between base color and custom media).
        //     3 textures: pulse / ripple / particles — selected by mx.fxTexture.
        //     Performance mode (mx.performanceMode) scales params.
        //     Hidden in bgMode 0/1 → zero CPU cost.
        Canvas {
            id: fxCanvas
            anchors.fill: parent
            contextType: "2d"
            renderStrategy: Canvas.Immediate
            visible: mx.bgMode === 2
            opacity: 1.0

            // --- Performance scaling (high/mid/low) ---
            readonly property int perf: rhythm.resolvedPerf   // 1/2/3
            readonly property int fpsInterval: perf === 3 ? 66 : 33   // low=15fps, else 30fps
            readonly property int particleCap: perf === 1 ? 200 : (perf === 2 ? 100 : 0)
            readonly property int ringCap: perf === 1 ? 12 : 6
            readonly property real ampScale: perf === 1 ? 1.0 : (perf === 2 ? 0.7 : 0.5)
            // low mode forces pulse-only rendering (ripple/particles degrade to pulse)
            readonly property bool lowMode: perf === 3
            readonly property int effectiveTexture: lowMode ? 0 : mx.fxTexture

            // --- Cross-frame persistent state ---
            // particles: [{x,y,vx,vy,life,r}]
            property var particles: []
            // ripple rings: [{x,y,r,vr,alpha,w}]
            property var rings: []
            // pulse drift phase accumulator
            property real pulseT: 0

            Timer {
                id: fxTimer
                interval: fxCanvas.fpsInterval
                repeat: true
                running: fxCanvas.visible
                onTriggered: fxCanvas.requestPaint()
            }

            onPaint: {
                var ctx = getContext("2d")
                var w = width, h = height
                ctx.reset()
                ctx.globalCompositeOperation = "source-over"

                // Color palette from effectiveColors (flat [r0,g0,b0, r1,g1,b1, r2,g2,b2])
                var ec = mx.effectiveColors
                var c0 = (ec && ec.length >= 9) ? [ec[0],ec[1],ec[2]] : [80,80,120]
                var c1 = (ec && ec.length >= 9) ? [ec[3],ec[4],ec[5]] : [80,80,120]
                var c2 = (ec && ec.length >= 9) ? [ec[6],ec[7],ec[8]] : [80,80,120]

                function rgba(c, a) { return "rgba(" + Math.round(c[0]) + "," + Math.round(c[1]) + "," + Math.round(c[2]) + "," + a + ")" }

                var t = Date.now() / 1000.0
                var b = rhythm.beat
                var energy = rhythm.energy
                var bass = rhythm.bass
                var treble = rhythm.treble
                var onsetFlag = rhythm.onset
                var amp = ampScale

                // --- Texture dispatch ---
                var tex = effectiveTexture
                if (tex === 1 && !lowMode) {
                    paintRipple(ctx, w, h, t, b, bass, treble, onsetFlag, amp, [c0,c1,c2], rgba)
                } else if (tex === 2 && !lowMode) {
                    paintParticles(ctx, w, h, t, b, energy, bass, onsetFlag, amp, [c0,c1,c2], rgba)
                } else {
                    paintPulse(ctx, w, h, t, b, energy, onsetFlag, amp, [c0,c1,c2], rgba)
                }
            }

            // --- pulse: rhythm-modulated wave field (evolution of default ripple) ---
            function paintPulse(ctx, w, h, t, beat, energy, onset, amp, cols, rgba) {
                pulseT += 0.033
                var cx = w / 2, cy = h / 2
                var layers = 3
                var waves = [
                    function(x, t2, l) { return Math.sin(x * 0.0025 + t2 * 0.21 + l * 0.7) * 0.7
                                               + Math.sin(x * 0.006 + t2 * 0.30) * 0.3 },
                    function(x, t2, l) { return Math.sin(x * 0.004 + t2 * 0.30 + l * 1.1) * 0.6
                                               + Math.sin(x * 0.009 + t2 * 0.42) * 0.25 },
                    function(x, t2, l) { return Math.sin(x * 0.005 + t2 * 0.42 + l * 1.5) * 0.5
                                               + Math.sin(x * 0.011 + t2 * 0.54) * 0.25
                                               + Math.sin(x * 0.002 - t2 * 0.15) * 0.3 }
                ]
                var beatAmp = (0.3 + beat * 0.7) * amp
                if (onset) beatAmp *= 1.5
                var ampArr = [h * 0.05 * beatAmp, h * 0.04 * beatAmp, h * 0.03 * beatAmp]
                var yOffArr = [h * 0.27, h * 0.42, h * 0.58]
                var alphaArr = [0.28, 0.22, 0.18]

                for (var li = 2; li >= 0; --li) {
                    ctx.beginPath()
                    ctx.moveTo(0, h)
                    ctx.lineTo(0, yOffArr[li])
                    for (var x = 0; x <= w; x += 5) {
                        var y = yOffArr[li] + waves[li](x, pulseT, li) * ampArr[li]
                        ctx.lineTo(x, y)
                    }
                    ctx.lineTo(w, h)
                    ctx.closePath()
                    var lc = cols[li]
                    var a = alphaArr[li] * (0.5 + energy * 0.5)
                    var crestY = yOffArr[li] - ampArr[li]
                    var grad = ctx.createLinearGradient(0, crestY, 0, h)
                    grad.addColorStop(0.00, rgba(lc, a))
                    grad.addColorStop(0.15, rgba(lc, a * 0.6))
                    grad.addColorStop(0.50, rgba(lc, a * 0.2))
                    grad.addColorStop(1.00, rgba(lc, 0.0))
                    ctx.fillStyle = grad
                    ctx.fill()
                }

                // Radial push on onset
                if (onset) {
                    ctx.beginPath()
                    var r = Math.max(w, h) * 0.4 * beat
                    var grad2 = ctx.createRadialGradient(cx, cy, 0, cx, cy, r)
                    grad2.addColorStop(0, rgba(cols[0], 0.15))
                    grad2.addColorStop(1, rgba(cols[0], 0))
                    ctx.fillStyle = grad2
                    ctx.fillRect(0, 0, w, h)
                }
            }

            // --- ripple: concentric rings spawned on onset ---
            function paintRipple(ctx, w, h, t, beat, bass, treble, onset, amp, cols, rgba) {
                var cx = w / 2, cy = h / 2
                // Spawn new ring on onset
                if (onset) {
                    var isBass = bass > treble
                    var vr = isBass ? (2.0 + bass * 6.0) : (4.0 + treble * 8.0)
                    var w2 = isBass ? 3.0 : 1.5
                    var col = isBass ? cols[0] : cols[2]
                    rings.push({x: cx, y: cy, r: 1, vr: vr * amp, alpha: 0.6, w: w2, c: col})
                    // Cap rings
                    while (rings.length > ringCap) rings.shift()
                }
                // Update + draw rings
                ctx.globalCompositeOperation = "lighter"
                for (var i = rings.length - 1; i >= 0; --i) {
                    var ring = rings[i]
                    ring.r += ring.vr
                    ring.alpha *= 0.97
                    if (ring.alpha < 0.01 || ring.r > Math.max(w, h)) {
                        rings.splice(i, 1)
                        continue
                    }
                    ctx.beginPath()
                    ctx.arc(ring.x, ring.y, ring.r, 0, 2 * Math.PI)
                    ctx.strokeStyle = rgba(ring.c, ring.alpha)
                    ctx.lineWidth = ring.w
                    ctx.stroke()
                }
                ctx.globalCompositeOperation = "source-over"

                // Faint ambient pulse even without onset (don't freeze)
                if (rings.length === 0) {
                    var r2 = Math.max(w, h) * 0.3 * (0.5 + beat * 0.5)
                    var grad = ctx.createRadialGradient(cx, cy, 0, cx, cy, r2)
                    grad.addColorStop(0, rgba(cols[1], 0.08))
                    grad.addColorStop(1, rgba(cols[1], 0))
                    ctx.fillStyle = grad
                    ctx.fillRect(0, 0, w, h)
                }
            }

            // --- particles: burst on onset, physics integration, trail ---
            function paintParticles(ctx, w, h, t, beat, energy, bass, onset, amp, cols, rgba) {
                var cx = w / 2, cy = h / 2
                // Spawn on onset
                if (onset && particleCap > 0) {
                    var n = Math.min(particleCap - particles.length, Math.round(20 + bass * 80))
                    for (var i = 0; i < n; ++i) {
                        var ang = Math.random() * 2 * Math.PI
                        var speed = (1.0 + bass * 5.0) * amp * (0.5 + Math.random())
                        particles.push({
                            x: cx, y: cy,
                            vx: Math.cos(ang) * speed,
                            vy: Math.sin(ang) * speed,
                            life: 1.0,
                            r: 2 + Math.random() * 3,
                            c: cols[Math.floor(Math.random() * 3)]
                        })
                    }
                    while (particles.length > particleCap) particles.shift()
                }

                // Fade canvas (trail effect) — draw semi-transparent black rect
                ctx.globalCompositeOperation = "source-over"
                ctx.fillStyle = "rgba(5, 6, 10, 0.12)"
                ctx.fillRect(0, 0, w, h)

                // Integrate + draw particles
                ctx.globalCompositeOperation = "lighter"
                for (var j = particles.length - 1; j >= 0; --j) {
                    var p = particles[j]
                    p.x += p.vx
                    p.y += p.vy
                    p.vx *= 0.98   // damping
                    p.vy *= 0.98
                    p.life *= 0.96
                    if (p.life < 0.02 || p.x < -20 || p.x > w + 20 || p.y < -20 || p.y > h + 20) {
                        particles.splice(j, 1)
                        continue
                    }
                    ctx.beginPath()
                    ctx.arc(p.x, p.y, p.r * p.life, 0, 2 * Math.PI)
                    ctx.fillStyle = rgba(p.c, p.life * 0.8)
                    ctx.fill()
                }
                ctx.globalCompositeOperation = "source-over"
            }
        }   // ← end of fxCanvas

        // [3] Custom background — ON TOP of ripple, BEHIND dark-dim overlay.
        // ALL components here are direct Window children (no Item wrapper).
        //
        // AnimatedImage path (GIF/WEBP/PNG/JPG):
        //   - Plain AnimatedImage when NO blur needed
        //   - Wrapped in MultiEffect when blur is enabled
        //   VideoOutput CANNOT be source of MultiEffect (native GL surface),
        //   so MP4 path is always plain (blur limitation for MP4 only).
        AnimatedImage {
            id: bgAnimated
            anchors.fill: parent
            // ALWAYS visible — MultiEffect needs it as source even when blur is ON.
            // When blur ON, MultiEffect renders on TOP (higher z) so user sees the blurred version.
            visible: mx.bgVideoPath !== "" && root.bgMediaType === 1
            source: root.bgImagePath
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            cache: false
        }
        MediaPlayer {
            id: bgVideoPlayer
            source: root.bgMediaPath
            loops: MediaPlayer.Infinite
            audioOutput: AudioOutput { volume: 0 }
            videoOutput: bgVideoOut
            onErrorOccurred: console.log("[viz] bg video error:", errorString)
            // play() when source changes (covers both initial and post-push timing)
            onSourceChanged: { if (source !== "") { console.log("[viz] bg video source set:", source); play() } }
            // Also play when status becomes LoadedMedia (defensive — some formats need this)
            onPlaybackStateChanged: {
                if (playbackState === MediaPlayer.LoadedMedia || playbackState === MediaPlayer.Stopped) {
                    if (source !== "") play()
                }
            }
        }
        VideoOutput {
            id: bgVideoOut
            anchors.fill: parent
            visible: mx.bgVideoPath !== "" && root.bgMediaType === 2
            fillMode: VideoOutput.PreserveAspectCrop
        }

        // [4] Dark overlay — ONLY shown when Custom bg media is present.
        // Depth driven by mx.bgOverlayDepth (0..1). 0 = no dim, 1 = fully black.
        // Default ripple background should NOT be dimmed — it is its own visual.
        Rectangle {
            anchors.fill: parent
            visible: mx.bgVideoPath !== ""
            color: {
                var d = (mx.bgOverlayDepth !== undefined) ? mx.bgOverlayDepth : 0.5;
                d = Math.max(0, Math.min(1, d));
                return Qt.rgba(0, 0, 0, d);
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
            visible: mx.vizMode === 0
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

        // ===== Mode 1: Radial — starburst emanating from center =====
        Item {
            visible: mx.vizMode === 1
            anchors.fill: parent

            Repeater {
                model: 64
                delegate: Rectangle {
                    readonly property real v: (mx.bins !== undefined && mx.bins[index] !== undefined ? mx.bins[index] : 0)
                    readonly property real maxR: Math.min(spectrum.width * 0.55, spectrum.height * 0.85)

                    // Bar's LEFT edge is at the EXACT center of spectrum
                    // transformOrigin: Item.Left → rotation pivot IS at center
                    width: Math.max(v * maxR, 2)
                    height: Math.max(maxR * 0.03, 3)
                    x: spectrum.width / 2
                    y: spectrum.height / 2 - height / 2
                    transformOrigin: Item.Left
                    rotation: (index / 64) * 360

                    color: Qt.hsla(root.trackHue / 360, 0.55, 0.35 + v * 0.4, 1)
                    opacity: Math.min(v + 0.15, 1)
                    Behavior on width { SmoothedAnimation { duration: 90 } }
                }
            }

            // Central hub covers the messy center overlap
            Rectangle {
                width: 30; height: 30
                x: spectrum.width / 2 - 15
                y: spectrum.height / 2 - 15
                radius: 15
                color: Qt.hsla(root.trackHue / 360, 0.4, 0.18, 1)
            }
        }

        // ===== Mode 2: Waterfall — bars hang from top =====
        Row {
            visible: mx.vizMode === 2
            anchors.fill: parent
            spacing: spectrum.width / 64 * 0.2
            Repeater {
                model: 64
                delegate: Rectangle {
                    readonly property real v: (mx.bins[index] !== undefined ? mx.bins[index] : 0)
                    width: (parent.width - parent.spacing * 63) / 64
                    height: Math.max(v * spectrum.height * 1.2, 2)
                    x: index * (width + parent.spacing)
                    y: 0
                    color: Qt.hsla(root.trackHue / 360, 0.45 + v * 0.3, 0.55 - v * 0.25, 0.9)
                    Behavior on height { SmoothedAnimation { duration: 100 } }
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
                text: "♫ " + (mx.albumText !== "" ? mx.albumText : "")
                font { family: "Microsoft YaHei"; pixelSize: root.landscape ? root.vmin * 0.020 : root.vmin * 0.03 }
                color: "#6b76a8"
                visible: mx.albumText !== ""
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

        // AnimatedImage auto-plays GIF/WEBP. No extra props needed.
        // Loop count comes from the GIF file header (0 = infinite loop).
        AnimatedImage {
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
