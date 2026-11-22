import SwiftUI

@main
struct AirBuzzApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
                .task { _ = AirLink.shared }
        }
    }
}
