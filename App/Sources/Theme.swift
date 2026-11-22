import SwiftUI

extension Color {
    /// AirBuzz brand ink (#111111) and the lighter top of its button gradient (#2b2b2b).
    static let ink = Color(red: 0.067, green: 0.067, blue: 0.067)
    static let inkTop = Color(red: 0.169, green: 0.169, blue: 0.169)
}

/// Press feedback for any tappable surface: subtle scale with a snappy spring.
/// Emil: buttons must feel like the UI is listening. Keep the scale subtle (0.95-0.98).
/// With Reduce Motion on, a press dims instead of scaling.
struct PressableStyle: ButtonStyle {
    var scale: CGFloat = 0.96
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed && !reduceMotion ? scale : 1)
            .opacity(configuration.isPressed && reduceMotion ? 0.7 : 1)
            .animation(reduceMotion ? .easeOut(duration: 0.15) : .spring(duration: 0.3, bounce: 0.3),
                       value: configuration.isPressed)
    }
}
