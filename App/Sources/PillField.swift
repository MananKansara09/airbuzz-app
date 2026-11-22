import SwiftUI

/// The AirBuzz mark: a solid pill inside a halftone dot field. Pulses of dots run outward from the pill,
/// a few on a slow idle beat, a burst on each tap and a steady stream while a buzz is running.
/// Ported from the launch film's logo animation. Units: the mark spans -1...1 on both axes.
struct PillField: View {
    var tapTimes: [Date]
    var runStart: Date?
    var runEnd: Date?
    var reduceMotion: Bool
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        TimelineView(.animation(paused: reduceMotion)) { context in
            Canvas { gc, size in
                draw(gc, size: size, now: context.date)
            }
        }
        .accessibilityHidden(true)
    }

    private var ink: Color {
        scheme == .dark ? Color(white: 0.96) : Color(red: 0.067, green: 0.067, blue: 0.067)
    }

    private func draw(_ gc: GraphicsContext, size: CGSize, now: Date) {
        let side = min(size.width, size.height)
        let half = side / 2
        let ox = size.width / 2, oy = size.height / 2
        let field = DotField.shared
        let t = now.timeIntervalSinceReferenceDate
        let pulses = reduceMotion ? [] : activePulses(t: t)

        // Opacity is bucketed so the whole field draws in a handful of fills.
        let levels = 24
        var paths = [Path](repeating: Path(), count: levels + 1)
        let r = field.dotRadius * half
        for i in field.dots.indices {
            let d = field.dots[i]
            var p = 0.0
            for pulse in pulses { p += pulse.intensity(dist: d.dist, theta: d.theta, t: t) }
            let floor = reduceMotion ? 0.3 : DotField.floor
            let op = max(floor * d.base, min(max(p, 0), 1) * d.envelope)
            if op < 0.03 { continue }
            let level = min(levels, Int((op * Double(levels)).rounded()))
            let cx = ox + d.x * half, cy = oy - d.y * half
            paths[level].addEllipse(in: CGRect(x: cx - r, y: cy - r, width: 2 * r, height: 2 * r))
        }
        for level in 1...levels where !paths[level].isEmpty {
            gc.fill(paths[level], with: .color(ink.opacity(Double(level) / Double(levels))))
        }

        let pw = 2 * DotField.pillR * half, ph = 2 * (DotField.pillH + DotField.pillR) * half
        let pill = Path(roundedRect: CGRect(x: ox - pw / 2, y: oy - ph / 2, width: pw, height: ph),
                        cornerRadius: pw / 2, style: .continuous)
        gc.fill(pill, with: .color(ink))
    }

    private func activePulses(t: Double) -> [Pulse] {
        var out: [Pulse] = []
        // Idle beat: 3 pulses every 1.6 s (5 on every fourth), like the icon in the film.
        let period = 1.6
        let k = Int((t / period).rounded(.down))
        for beat in (k - 1)...k {
            var rng = SplitMix(seed: UInt64(bitPattern: Int64(beat)) &* 0x9E37_79B9)
            let count = beat % 4 == 0 ? 5 : 3
            for _ in 0..<count { out.append(Pulse.random(start: Double(beat) * period, rng: &rng)) }
        }
        // Tap: a ring of 9 pulses all the way round.
        for tap in tapTimes {
            let t0 = tap.timeIntervalSinceReferenceDate + 0.03
            guard t - t0 < 1.7 else { continue }
            var rng = SplitMix(seed: UInt64(bitPattern: Int64(t0 * 1000)))
            for i in 0..<9 {
                let a = Double(i) / 9 * 2 * .pi + rng.uniform(-0.2, 0.2)
                out.append(Pulse(start: t0, angle: a, spread: rng.uniform(0.5, 0.8),
                                 width: rng.uniform(0.08, 0.11), duration: rng.uniform(1.2, 1.6)))
            }
        }
        // Running: 6 pulses every 0.45 s from just after the tap until the buzz finishes.
        if let start = runStart?.timeIntervalSinceReferenceDate {
            let end = runEnd?.timeIntervalSinceReferenceDate ?? t
            let step = 0.45
            var b = start + 0.2
            while b < end {
                if t - b < 1.7 && b <= t {
                    var rng = SplitMix(seed: UInt64(bitPattern: Int64(b * 1000)) ^ 0x5DEE_CE66)
                    for _ in 0..<6 {
                        out.append(Pulse(start: b, angle: rng.uniform(-.pi, .pi), spread: rng.uniform(0.45, 0.85),
                                         width: rng.uniform(0.07, 0.10), duration: rng.uniform(1.2, 1.6)))
                    }
                }
                b += step
            }
        }
        return out.filter { t > $0.start && t < $0.start + $0.duration }
    }
}

private struct Pulse {
    var start: Double, angle: Double, spread: Double, width: Double, duration: Double

    static func random(start: Double, rng: inout SplitMix) -> Pulse {
        Pulse(start: start, angle: rng.uniform(-.pi, .pi), spread: rng.uniform(0.35, 0.8),
              width: rng.uniform(0.06, 0.10), duration: rng.uniform(1.3, 1.8))
    }

    func intensity(dist: Double, theta: Double, t: Double) -> Double {
        let u = (t - start) / duration
        guard u > 0, u < 1 else { return 0 }
        let c = DotField.reach * (1 - pow(1 - u, 1.5))
        let fade = 1.4 * pow(1 - u, 0.9) * min(1, u * 20)
        let da = remainder(theta - angle, 2 * .pi)
        let radial = (dist - c) / width, angular = da / spread
        return fade * exp(-radial * radial) * exp(-angular * angular)
    }
}

private struct Dot {
    var x: Double, y: Double, dist: Double, theta: Double, base: Double, envelope: Double
}

private final class DotField: Sendable {
    static let shared = DotField()
    static let pillH = 0.22, pillR = 0.12
    static let spacing = 0.030, gap = 1.75, falloff = 0.45
    static let reach = 0.45, floor = 0.10
    let dotRadius = 0.36 * DotField.spacing
    let dots: [Dot]

    private init() {
        let sp = Self.spacing, extent = 1.05
        let n = Int(extent / sp) + 3
        var dots: [Dot] = []
        for j in -n...n {
            for i in (-2 * n)...(2 * n) {
                let x = (Double(i) + 0.5 * Double(((j % 2) + 2) % 2)) * sp
                let y = Double(j) * sp * 3.0.squareRoot() / 2
                guard abs(x) < extent, abs(y) < extent else { continue }
                let dist = Self.capsuleDistance(x: x, y: y)
                let lin = min(max(1 - dist / Self.falloff, 0), 1)
                let base = pow(min(max(1 - pow(1 - lin, 1.25), 0), 1), 1.6)
                guard dist > 0.36 * sp + Self.gap * sp, base > 0.05 else { continue }
                let v = min(max((dist - 0.15) / 0.30, 0), 1)
                dots.append(Dot(x: x, y: y, dist: dist, theta: atan2(y * 0.55, x),
                                base: base, envelope: 1 - v * v * (3 - 2 * v)))
            }
        }
        self.dots = dots
    }

    static func capsuleDistance(x: Double, y: Double) -> Double {
        let yy = min(max(y, -pillH), pillH)
        return (x * x + (y - yy) * (y - yy)).squareRoot() - pillR
    }
}

/// Small deterministic generator so pulses come out the same on every redraw.
private struct SplitMix {
    var state: UInt64
    init(seed: UInt64) { state = seed }
    mutating func next() -> UInt64 {
        state &+= 0x9E37_79B9_7F4A_7C15
        var z = state
        z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
        z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
        return z ^ (z >> 31)
    }
    mutating func uniform(_ lo: Double, _ hi: Double) -> Double {
        lo + (hi - lo) * Double(next() >> 11) / Double(1 << 53)
    }
}
