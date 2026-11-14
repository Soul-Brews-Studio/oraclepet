import XCTest

final class CardputerBLEReaderUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    func testCaptureCardputerBLEScreen() throws {
        let app = XCUIApplication()
        addUIInterruptionMonitor(withDescription: "Bluetooth permission") { alert in
            for title in ["Allow", "OK", "Continue"] where alert.buttons[title].exists {
                alert.buttons[title].tap()
                return true
            }
            return false
        }

        XCUIDevice.shared.orientation = .portrait
        app.launch()
        XCUIDevice.shared.orientation = .portrait
        let buddyFullTab = app.buttons["Buddy Full"].firstMatch
        if buddyFullTab.waitForExistence(timeout: 8) {
            buddyFullTab.tap()
        } else {
            app.coordinate(withNormalizedOffset: CGVector(dx: 0.30, dy: 0.045)).tap()
        }

        let fullPage = app.descendants(matching: .any)["BuddyFullPage"]
        XCTAssertTrue(fullPage.waitForExistence(timeout: 8), "Buddy Full page should exist after selecting the new tab")
        Thread.sleep(forTimeInterval: 3)

        let screenshot = XCUIScreen.main.screenshot()
        let attachment = XCTAttachment(screenshot: screenshot)
        attachment.name = "Claude Buddy full-page iPad capture"
        attachment.lifetime = .keepAlways
        add(attachment)

        XCTAssertTrue(
            fullPage.exists,
            "Claude Buddy full-page tab should be visible before capture"
        )
        if buddyFullTab.exists {
            XCTAssertTrue(buddyFullTab.isSelected, "Buddy Full tab should be selected for the screenshot")
        }
    }
}
