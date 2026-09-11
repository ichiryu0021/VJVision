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
        readonly property real beat: root._mock ? 0 : viz.beat
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

    // ---------- v2.0.4: Rhythm feature bridge (QML side) ----------
    // beat comes from the C++ BeatTracker (src/audio/beat_tracker.*),
    // an energy-only onset detector on PRE-AGC raw Mel bins: kick-focused
    // low-band flux (30..308 Hz) weighted by broad-band coincidence, gated
    // by a fixed 0.25 threshold plus a transient-jump path, with a 110 ms
    // refractory. There is no BPM/tempo/phase estimation by design — every
    // pulse is a real energy attack. Detection must NOT use these renderer bins post-AGC: the AGC
    // fast-attack / slow-release gain ramp fakes broadband energy rises
    // after every kick. This Item only aggregates band energies for
    // rendering and derives the per-frame onset edge from the C++ pulse.
    // NOTE: must use Item (not QtObject) — QtObject has no default property.
    Item {
        id: rhythm
        visible: false
        property real bass: 0
        property real mid: 0
        property real treble: 0
        property real energy: 0
        property real beat: mx.beat
        property bool onset: false
        property real prevBeat: 0
        // Latched in onBeatChanged (fires the instant the C++ beat envelope
        // crosses the edge) rather than polled in the 30fps update(): the
        // spectrum push and fxTimer are independent asynchronous 30fps
        // sources, so timer-side edge polling could miss the 1-2 frame
        // >0.85 plateau, and Canvas.requestPaint runs asynchronously after
        // the next update() had already cleared onset → kick impulse never
        // fired. The latch is consumed by onPaint, decoupling capture from
        // render timing.
        property bool onsetLatched: false

        onBeatChanged: {
            // Rising-edge via JUMP, not absolute levels: with rapid rolls
            // (triplets ≈156 ms apart) the 280 ms envelope only decays to
            // ~0.57 before the next reset, so a prevBeat<0.5 level test
            // would swallow every hit after the first. A >0.15 upward jump
            // reliably marks each C++ onset reset (beat_ is set to 1.0).
            if (beat > 0.7 && (beat - prevBeat) > 0.15) {
                onsetLatched = true
                // Paint in THIS event-loop iteration — do not wait for the
                // 33 ms fxTimer tick, which made kicks look late by 0-33 ms.
                if (fxCanvas.visible) fxCanvas.requestPaint()
            }
            prevBeat = beat
        }

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

        function update() {
            var bins = mx.bins
            var n = bins ? bins.length : 0
            if (n < 24) {
                bass = 0; mid = 0; treble = 0; energy = 0
                return
            }

            // Band aggregation (Mel bins: bass 0-5, mid 6-23, treble 24+)
            var bSum = 0, mSum = 0, tSum = 0, allSum = 0
            for (var i = 0; i < 6; ++i) { var v = bins[i]; bSum += v; allSum += v }
            for (var i2 = 6; i2 < 24 && i2 < n; ++i2) { var v2 = bins[i2]; mSum += v2; allSum += v2 }
            for (var i3 = 24; i3 < n; ++i3) { var v3 = bins[i3]; tSum += v3; allSum += v3 }
            bass = bSum / 6
            mid = mSum / Math.max(1, (Math.min(24, n) - 6))
            treble = tSum / Math.max(1, (n - 24))
            energy = allSum / n
            // Onset edge is captured in onBeatChanged → onsetLatched.
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
    //   2b. FX texture Canvas (fx mode only — reactive strokes ON TOP of ripple)
    //   3. Custom background media (AnimatedImage or VideoOutput — on top of ripple/fx IF present)
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
            // Always visible: in fx (rhythm texture) mode the fxCanvas draws
            // ON TOP of this ambient wash — the texture is strokes on a
            // transparent canvas, and the default ripple stays as the base.
            visible: true
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
                running: ripple.visible
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

                // Normalized cycles-per-screen (same scheme as fx pulse):
                // ~2 crests at any resolution. The 3 layers differ only by
                // phase / drift speed / opacity — no dense pixel-k ripples.
                var TAU = 2 * Math.PI
                var waveforms = [
                    function(x, t, l) { return Math.sin((x / w) * TAU * 1.90 + t * 0.21 + l * 0.7) * 0.95
                                                 + Math.sin((x / w) * TAU * 3.80 + t * 0.30) * 0.05 },
                    function(x, t, l) { return Math.sin((x / w) * TAU * 1.75 + t * 0.30 + l * 1.1) * 0.95
                                                 + Math.sin((x / w) * TAU * 3.50 + t * 0.42) * 0.05 },
                    function(x, t, l) { return Math.sin((x / w) * TAU * 2.05 + t * 0.42 + l * 1.5) * 0.95
                                                 + Math.sin((x / w) * TAU * 4.10 + t * 0.54) * 0.05 }
                ]

                var ampArr   = [h * 0.05, h * 0.04, h * 0.03]
                var yOffArr  = [h * 0.27, h * 0.42, h * 0.58]
                var alphaArr = [0.28, 0.22, 0.18]

                // PAINTER'S ALGORITHM: draw BOTTOM layer first (2), MIDDLE (1), TOP last (0).
                for (var li = 2; li >= 0; --li) {
                    ctx.beginPath()
                    ctx.moveTo(0, h)
                    ctx.lineTo(0, yOffArr[li])

                    // Fixed ~160 path points regardless of resolution.
                    var rstep = Math.max(8, Math.floor(w / 160))
                    for (var x = 0; x <= w; x += rstep) {
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

        // [2b] v2.1.0: FX texture Canvas — shown only when bgMode==2 (rhythm
        //     texture). Sits ABOVE the default ripple wash and BELOW custom
        //     media / dark overlay / song content. 3 textures:
        //     pulse / breath / horizon — selected by mx.fxTexture.
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
            readonly property real ampScale: perf === 1 ? 1.0 : (perf === 2 ? 0.7 : 0.5)
            // low mode forces pulse-only rendering (breath/horizon degrade to pulse)
            readonly property bool lowMode: perf === 3
            readonly property int effectiveTexture: lowMode ? 0 : mx.fxTexture

            // --- Cross-frame persistent state ---
            property real pulseT: 0
            property real kickV: 0   // beat impact velocity (fast glow/width punch)
            // Travelling local bumps: each onset spawns a Gaussian hump at a
            // random x with random width/speed; it drifts left and fades out.
            // Replaces the old "whole wave translated up" motion which made
            // every kick look like a copy-paste of the same shape.
            property var pulses: []

            // KEY: update rhythm BEFORE requestPaint so beat is fresh even
            // if onPaint is slow. Previously rhythmTimer ran independently but
            // got starved when onPaint blocked the GUI thread → beat stuck at 0.
            Timer {
                id: fxTimer
                interval: fxCanvas.fpsInterval
                repeat: true
                running: fxCanvas.visible
                onTriggered: {
                    rhythm.update()    // update beat first
                    fxCanvas.requestPaint()
                }
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
                // Consume the latched onset (set in rhythm.onBeatChanged,
                // independent of this timer/paint scheduling).
                var onsetFlag = rhythm.onsetLatched
                rhythm.onsetLatched = false
                var amp = ampScale

                // Central impulse injection (shared by every texture):
                // onset injects velocity, ~200 ms exponential decay.
                if (onsetFlag) {
                    kickV = Math.min(1.4, kickV + 1.0)
                    // Spawn a travelling local bump (pulse texture): random
                    // birth position / width / speed so consecutive kicks
                    // never look like copies of the same shape.
                    pulses.push({
                        x: w * (0.15 + Math.random() * 0.7),
                        w: 0.7 + Math.random() * 0.7,     // sigma multiplier
                        vx: 0.10 + Math.random() * 0.14,  // drift as fraction of w/s
                        life: 1.0
                    })
                    if (pulses.length > 12) pulses.shift()
                }
                kickV *= Math.exp(-0.033 / 0.20)

                // Advance/decay travelling bumps (use real frame interval).
                var fdt = fxCanvas.fpsInterval / 1000.0
                for (var pi = pulses.length - 1; pi >= 0; --pi) {
                    var pu = pulses[pi]
                    pu.x -= w * pu.vx * fdt
                    pu.life *= Math.exp(-fdt / 0.50)   // ~0.5 s visible travel
                    if (pu.life < 0.03 || pu.x < -w * 0.2) pulses.splice(pi, 1)
                }

                // --- Texture dispatch ---
                // 0=pulse, 1=breath, 2=horizon (low mode forces pulse)
                var tex = effectiveTexture
                if (tex === 1 && !lowMode) {
                    paintBreath(ctx, w, h, t, b, energy, bass, onsetFlag, amp, [c0,c1,c2], rgba)
                } else if (tex === 2 && !lowMode) {
                    paintHorizon(ctx, w, h, t, b, energy, treble, onsetFlag, amp, [c0,c1,c2], rgba)
                } else {
                    paintPulse(ctx, w, h, t, b, energy, onsetFlag, amp, [c0,c1,c2], rgba)
                }
            }

            // --- pulse: rhythm-modulated wave field (stroke-only, no fullscreen fill) ---
            // PERFORMANCE: uses ONLY stroke (wide lines) — no fill of large
            // areas. Pixel ops: ~20K/layer vs 1.5M/layer with fill = 75x less.
            // Beat modulates amplitude AND spatial frequency.
            function paintPulse(ctx, w, h, t, beat, energy, onset, amp, cols, rgba) {
                var step = Math.max(10, Math.floor(w / 120))  // ~120 pts regardless of resolution

                // Continuous amplitude: ENERGY sets the baseline (smooth,
                // always-moving level between quiet/loud passages) while the
                // beat envelope adds a transient punch on top. Driving amp
                // from beat alone made the wave binary (onset=max, otherwise
                // almost flat). 0.10 floor keeps gentle idle motion.
                var ampVis = Math.min(1.2, 0.10 + energy * 0.55 + beat * 0.45)
                // Very mild frequency punch only: a strong multiplier stacks
                // extra crests on top of the harmonic + kick bumps and the
                // screen ends up showing 3-4 peaks instead of ~2.
                var freqMod = 1.0 + beat * 0.2

                // Continuous gentle flow even between kicks (idle drift),
                // with a small forward surge on impact.
                pulseT += 0.033 * (0.7 + kickV * 2.0)

                // Local travelling bumps from the pulses array — each is a
                // Gaussian hump drifting left, so kicks deform the wave at
                // DIFFERENT places instead of translating the identical
                // shape up and down.
                var sigma = w * 0.085
                function perturb(x) {
                    var dy = 0
                    for (var k = 0; k < pulses.length; ++k) {
                        var p = pulses[k]
                        var u = (x - p.x) / (sigma * p.w)
                        dy += Math.exp(-u * u) * p.life
                    }
                    return dy
                }

                // Spatial frequency in CYCLES PER SCREEN (x/w normalized):
                // keeps ~2 crests at every resolution — absolute pixel k made
                // 2 crests at 1080p but 4+ at 4K. Harmonic is very weak.
                var TAU = 2 * Math.PI
                var waves = [
                    function(x, t2) { return Math.sin((x / w) * TAU * 1.85 * freqMod + t2 * 0.25) * 0.94
                                               + Math.sin((x / w) * TAU * 3.70 * freqMod + t2 * 0.35) * 0.04 },
                    function(x, t2) { return Math.sin((x / w) * TAU * 1.50 * freqMod + t2 * 0.40) * 0.94
                                               + Math.sin((x / w) * TAU * 3.00 * freqMod + t2 * 0.55) * 0.04 }
                ]
                var ampArr = [h * 0.075 * ampVis * amp, h * 0.052 * ampVis * amp]
                var yOffArr = [h * 0.35, h * 0.55]
                // Back layer responds less to bumps → depth/parallax.
                var bumpGain = [h * 0.10 * amp, h * 0.065 * amp]

                for (var li = 1; li >= 0; --li) {
                    ctx.beginPath()
                    for (var x = 0; x <= w; x += step) {
                        var y = yOffArr[li]
                              + waves[li](x, pulseT) * ampArr[li]
                              - perturb(x) * bumpGain[li]
                        if (x === 0) ctx.moveTo(x, y)
                        else ctx.lineTo(x, y)
                    }
                    var lc = cols[li]
                    var a = (0.5 + energy * 0.5) * (li === 0 ? 0.7 : 0.5)
                    ctx.strokeStyle = rgba(lc, a)
                    // Thin waveform: 4..9 px, follows continuous amplitude.
                    ctx.lineWidth = (4 + ampVis * 4) * amp
                    ctx.lineCap = "round"
                    ctx.lineJoin = "round"
                    ctx.stroke()
                }
            }

            // --- breath: central glow disc scaling with beat ---
            // PERFORMANCE: arc fill (circular region only, not fullscreen).
            // No fillRect(0,0,w,h). Pixel ops ~450K vs 2M = 4x less.
            function paintBreath(ctx, w, h, t, beat, energy, bass, onset, amp, cols, rgba) {
                var cx = w / 2, cy = h / 2
                var maxR = Math.min(w, h) * 0.35
                var beatVis = 0.3 + beat * 0.7
                if (onset) beatVis = Math.min(1.0, beatVis * 1.3)
                var discR = maxR * beatVis * amp
                var ambR = maxR * (1.0 + energy * 0.3)

                // Ambient glow (arc fill, NOT fillRect)
                var grad0 = ctx.createRadialGradient(cx, cy, 0, cx, cy, ambR)
                grad0.addColorStop(0, rgba(cols[1], 0.12 + energy * 0.1))
                grad0.addColorStop(1, rgba(cols[1], 0))
                ctx.fillStyle = grad0
                ctx.beginPath()
                ctx.arc(cx, cy, ambR, 0, 2 * Math.PI)
                ctx.fill()

                // Main disc (beat-driven)
                var grad1 = ctx.createRadialGradient(cx, cy, 0, cx, cy, discR)
                grad1.addColorStop(0, rgba(cols[0], 0.6 * beatVis))
                grad1.addColorStop(0.5, rgba(cols[0], 0.25 * beatVis))
                grad1.addColorStop(1, rgba(cols[0], 0))
                ctx.fillStyle = grad1
                ctx.beginPath()
                ctx.arc(cx, cy, discR, 0, 2 * Math.PI)
                ctx.fill()

                // Inner hot core (onset flash)
                if (beat > 0.1) {
                    var coreR = discR * 0.35
                    var grad2 = ctx.createRadialGradient(cx, cy, 0, cx, cy, coreR)
                    grad2.addColorStop(0, rgba(cols[2], 0.5 * beat))
                    grad2.addColorStop(1, rgba(cols[2], 0))
                    ctx.fillStyle = grad2
                    ctx.beginPath()
                    ctx.arc(cx, cy, coreR, 0, 2 * Math.PI)
                    ctx.fill()
                }

                // Rotating ring (stroke only)
                var ringR = maxR * (0.7 + beat * 0.2)
                var rot = t * 0.3
                ctx.strokeStyle = rgba(cols[2], 0.2 + beat * 0.4)
                ctx.lineWidth = (2 + beat * 3) * amp
                ctx.beginPath()
                for (var i = 0; i <= 48; ++i) {
                    var ang = (i / 48) * 2 * Math.PI + rot
                    var px = cx + Math.cos(ang) * ringR
                    var py = cy + Math.sin(ang) * ringR
                    if (i === 0) ctx.moveTo(px, py)
                    else ctx.lineTo(px, py)
                }
                ctx.closePath()
                ctx.stroke()
            }

            // --- horizon: glowing line swaying with beat (stroke + narrow fill) ---
            // PERFORMANCE: stroke lines + narrow rect fills (bandH only, not
            // fullscreen). Pixel ops ~310K vs 2M+ = 6x less.
            function paintHorizon(ctx, w, h, t, beat, energy, treble, onset, amp, cols, rgba) {
                var midY = h * 0.5 - kickV * h * 0.10 * amp   // kicks upward, falls back
                // Continuous energy baseline + beat punch (same as pulse).
                var hAmp = Math.min(1.2, 0.10 + energy * 0.55 + beat * 0.45)
                var sway = h * 0.055 * hAmp * amp
                var bandH = h * 0.12
                var step = Math.max(10, Math.floor(w / 100))
                var phase = t * 0.5 + kickV * 3.5   // surge on kick

                function waveY(x, off) {
                    // Cycles per screen (resolution-independent).
                    var xn = x / w * 2 * Math.PI
                    return midY + off + Math.sin(xn * 1.3 + phase) * sway * 0.5
                                        + Math.sin(xn * 2.6 + phase * 1.3) * sway * 0.25
                }

                // Sample the main line once; fills and stroke share these exact
                // points so the glow bands are perfectly contiguous with the line.
                var pts = []
                for (var xp = 0; xp <= w; xp += step) {
                    pts.push([xp, waveY(xp, 0)])
                }

                // --- Upper glow band: inner edge = main line, extends bandH up ---
                ctx.beginPath()
                ctx.moveTo(pts[0][0], pts[0][1] - bandH)
                for (var pu = 0; pu < pts.length; pu++) {
                    ctx.lineTo(pts[pu][0], pts[pu][1] - bandH)
                }
                for (var pu2 = pts.length - 1; pu2 >= 0; pu2--) {
                    ctx.lineTo(pts[pu2][0], pts[pu2][1])
                }
                ctx.closePath()
                // Opaque stop extends past midY so the wavy inner edge stays bright.
                var gradU = ctx.createLinearGradient(0, midY - bandH, 0, midY + sway)
                gradU.addColorStop(0, rgba(cols[0], 0))
                gradU.addColorStop(1, rgba(cols[0], 0.35 * Math.min(1, hAmp)))
                ctx.fillStyle = gradU
                ctx.fill()

                // --- Lower glow band: inner edge = main line, extends bandH down ---
                ctx.beginPath()
                ctx.moveTo(pts[0][0], pts[0][1] + bandH)
                for (var pl = 0; pl < pts.length; pl++) {
                    ctx.lineTo(pts[pl][0], pts[pl][1] + bandH)
                }
                for (var pl2 = pts.length - 1; pl2 >= 0; pl2--) {
                    ctx.lineTo(pts[pl2][0], pts[pl2][1])
                }
                ctx.closePath()
                var gradL = ctx.createLinearGradient(0, midY - sway, 0, midY + bandH)
                gradL.addColorStop(0, rgba(cols[2], 0.35 * Math.min(1, hAmp)))
                gradL.addColorStop(1, rgba(cols[2], 0))
                ctx.fillStyle = gradL
                ctx.fill()

                // --- Horizon line glow (stroke, brightest at beat) ---
                ctx.beginPath()
                for (var x3 = 0; x3 < pts.length; x3++) {
                    if (x3 === 0) ctx.moveTo(pts[x3][0], pts[x3][1])
                    else ctx.lineTo(pts[x3][0], pts[x3][1])
                }
                ctx.strokeStyle = rgba(cols[1], 0.5 + Math.min(1, hAmp) * 0.5)
                ctx.lineWidth = (3 + Math.min(1, hAmp) * 5) * amp
                ctx.lineCap = "round"
                ctx.lineJoin = "round"
                ctx.stroke()
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
