import XCTest
@testable import LamassuJS

final class LamassuEngineTests: XCTestCase {
    func testEvaluatesExpressions() throws {
        let engine = LamassuEngine()
        XCTAssertEqual(try engine.evaluate("1 + 1"), "2")
        XCTAssertEqual(try engine.evaluate("'a' + 'b'"), "ab")
    }

    func testStatePersistsAcrossEvaluations() throws {
        let engine = LamassuEngine()
        try engine.evaluate("let x = 21")
        XCTAssertEqual(try engine.evaluate("x * 2"), "42")
        try engine.evaluate("function twice(n) { return n * 2 }")
        XCTAssertEqual(try engine.evaluate("twice(x)"), "42")
    }

    func testSyntaxErrorCarriesPosition() {
        let engine = LamassuEngine()
        XCTAssertThrowsError(try engine.evaluate("let = 1")) { error in
            guard let err = error as? JSError else { return XCTFail("not a JSError") }
            XCTAssertEqual(err.kind, .syntax)
            XCTAssertEqual(err.line, 1)
            XCTAssertNotNil(err.column)
        }
    }

    func testUncaughtThrowIsRuntimeError() {
        let engine = LamassuEngine()
        XCTAssertThrowsError(try engine.evaluate("noSuchFunction()")) { error in
            guard let err = error as? JSError else { return XCTFail("not a JSError") }
            XCTAssertEqual(err.kind, .runtime)
            XCTAssertFalse(err.message.isEmpty)
        }
        // The context survives an uncaught throw.
        XCTAssertEqual(try? engine.evaluate("2 + 2"), "4")
    }

    func testPrintReachesTheHost() throws {
        let engine = LamassuEngine()
        var lines: [String] = []
        engine.onPrint = { lines.append($0) }
        try engine.evaluate("print('hi', 1, true)")
        try engine.evaluate("print('bye')")
        XCTAssertEqual(lines, ["hi 1 true", "bye"])
    }

    func testFuelBoundsARunawayLoop() {
        let engine = LamassuEngine(limits: .init(fuel: 100_000))
        XCTAssertThrowsError(try engine.evaluate("while (true) {}")) { error in
            XCTAssertEqual((error as? JSError)?.kind, .runtime)
        }
        // Fuel re-arms per evaluation, so the next line runs normally.
        XCTAssertEqual(try? engine.evaluate("1 + 2"), "3")
    }

    func testHeapLimitIsEnforced() {
        let engine = LamassuEngine(limits: .init(heapLimit: 1 << 20))
        XCTAssertThrowsError(
            try engine.evaluate("let a = []; while (true) { a.push('x'.repeat(1024)) }"))
    }

    func testInterruptFromAnotherThread() throws {
        let engine = LamassuEngine()
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.2) {
            engine.interrupt()
        }
        XCTAssertThrowsError(try engine.evaluate("while (true) {}")) { error in
            XCTAssertEqual((error as? JSError)?.kind, .interrupted)
        }
        // The next evaluate clears the flag.
        XCTAssertEqual(try engine.evaluate("7"), "7")
    }
}
