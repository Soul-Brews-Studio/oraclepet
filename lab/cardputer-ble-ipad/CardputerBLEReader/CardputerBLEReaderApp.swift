import SwiftUI

@main
struct CardputerBLEReaderApp: App {
    @StateObject private var ble = BuddyBLEModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(ble)
        }
    }
}
