import SwiftUI

struct ContentView: View {
    @StateObject private var model = ReplModel()
    @FocusState private var inputFocused: Bool

    private static let bottomID = "bottom"

    var body: some View {
        VStack(spacing: 0) {
            transcript
            Divider()
            inputBar
        }
        .font(.system(size: 13, design: .monospaced))
        .frame(minWidth: 480, minHeight: 320)
        .onAppear { inputFocused = true }
    }

    private var transcript: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 12) {
                    Text("lamassu REPL — top-level let/const/function persist; print(...) writes here.")
                        .foregroundStyle(.secondary)
                    ForEach(model.entries) { entry in
                        EntryView(entry: entry)
                    }
                    Color.clear.frame(height: 1).id(Self.bottomID)
                }
                .padding(12)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .onChange(of: model.revision) { _ in
                proxy.scrollTo(Self.bottomID, anchor: .bottom)
            }
        }
    }

    private var inputBar: some View {
        HStack(spacing: 8) {
            Text("›")
                .foregroundStyle(.secondary)
            TextField("JavaScript — return to run", text: $model.input)
                .textFieldStyle(.plain)
                .autocorrectionDisabled()
                .focused($inputFocused)
                .onSubmit {
                    model.submit()
                    inputFocused = true
                }
            if model.isBusy {
                ProgressView()
                    .controlSize(.small)
                Button("Stop") { model.interrupt() }
            } else {
                Button("Run") { model.submit() }
                    .disabled(model.input.trimmingCharacters(in: .whitespaces).isEmpty)
            }
        }
        .padding(10)
    }
}

private struct EntryView: View {
    let entry: ReplEntry

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(alignment: .top, spacing: 6) {
                Text("›")
                    .foregroundStyle(.tertiary)
                Text(entry.input)
                    .textSelection(.enabled)
            }
            ForEach(entry.lines) { line in
                Text(line.text)
                    .foregroundStyle(color(for: line.role))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    private func color(for role: ReplLine.Role) -> Color {
        switch role {
        case .printed: return .primary
        case .value: return .teal
        case .error: return .red
        }
    }
}
