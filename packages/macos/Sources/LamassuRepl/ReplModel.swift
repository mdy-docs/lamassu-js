import Foundation
import LamassuJS

struct ReplLine: Identifiable {
    enum Role {
        case printed  // a print(...) call from guest code
        case value    // the completion value of the evaluation
        case error    // syntax error, uncaught throw, or interrupt
    }

    let id = UUID()
    let role: Role
    let text: String
}

struct ReplEntry: Identifiable {
    let id = UUID()
    let input: String
    var lines: [ReplLine] = []
    var isRunning = true
}

/*
 * The engine is single-threaded by contract, so every evaluate() runs on one
 * serial queue; the UI thread only ever touches the engine through
 * interrupt(), which the C API documents as safe from any thread. All
 * @Published mutation hops back to the main queue.
 */
final class ReplModel: ObservableObject {
    @Published var entries: [ReplEntry] = []
    @Published var input = ""
    @Published var isBusy = false
    /// Bumped on every appended line; drives the auto-scroll.
    @Published private(set) var revision = 0

    private let engine = LamassuEngine()
    private let engineQueue = DispatchQueue(label: "lamassu.repl.engine")

    init() {
        engine.onPrint = { [weak self] line in
            // Fires on engineQueue, mid-evaluation.
            DispatchQueue.main.async {
                self?.appendLine(ReplLine(role: .printed, text: line))
            }
        }
    }

    func submit() {
        let source = input.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !source.isEmpty, !isBusy else { return }
        input = ""
        entries.append(ReplEntry(input: source))
        isBusy = true
        revision += 1

        let engine = self.engine
        engineQueue.async { [weak self] in
            let outcome: ReplLine
            do {
                let value = try engine.evaluate(source)
                outcome = ReplLine(role: .value, text: value)
            } catch let error as JSError {
                outcome = ReplLine(role: .error, text: error.description)
            } catch {
                outcome = ReplLine(role: .error, text: "\(error)")
            }
            DispatchQueue.main.async {
                self?.appendLine(outcome)
                self?.finishCurrentEntry()
            }
        }
    }

    /// Stops a runaway evaluation (e.g. `while (true) {}`).
    func interrupt() {
        engine.interrupt()
    }

    private func appendLine(_ line: ReplLine) {
        guard !entries.isEmpty else { return }
        entries[entries.count - 1].lines.append(line)
        revision += 1
    }

    private func finishCurrentEntry() {
        guard !entries.isEmpty else { return }
        entries[entries.count - 1].isRunning = false
        isBusy = false
        revision += 1
    }
}
