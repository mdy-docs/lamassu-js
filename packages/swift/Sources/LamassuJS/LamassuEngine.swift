/*
 * LamassuEngine — a Swift face for the lamassu-js C engine.
 *
 * One engine owns one JsVm and one persistent JsContext, so successive
 * evaluate() calls share top-level state the way a REPL session does
 * (js_compile_module_repl). The engine is single-threaded by contract:
 * use it from one thread or serial queue. The one exception, mirroring the
 * C API, is interrupt(), which may be called from any thread while an
 * evaluation is running.
 */
import CLamassu
import Foundation

/// A thrown JavaScript failure: a compile error, an uncaught runtime value,
/// or an interrupted run.
public struct JSError: Error, Equatable, CustomStringConvertible {
    public enum Kind: Equatable {
        case syntax
        case runtime
        case interrupted
    }

    public let kind: Kind
    /// The engine's message: a static diagnostic for syntax errors, the
    /// stringified thrown value for runtime errors.
    public let message: String
    /// 1-based source position, syntax errors only.
    public let line: Int?
    public let column: Int?

    public var description: String {
        switch kind {
        case .syntax:
            if let line, let column {
                return "SyntaxError at \(line):\(column): \(message)"
            }
            return "SyntaxError: \(message)"
        case .runtime:
            return "Uncaught \(message)"
        case .interrupted:
            return "Execution interrupted"
        }
    }
}

public final class LamassuEngine {
    /// Sandbox limits, both off by default — right for your own code, wrong
    /// for anyone else's (see docs/security.md in the repo).
    public struct Limits {
        /// Bytecode-instruction budget per evaluate() (0 = unlimited).
        public var fuel: UInt64
        /// Hard cap on live guest heap bytes (0 = unlimited).
        public var heapLimit: Int

        public init(fuel: UInt64 = 0, heapLimit: Int = 0) {
            self.fuel = fuel
            self.heapLimit = heapLimit
        }
    }

    /// Receives each `print(...)` line from guest code. When nil, lines go
    /// to stdout. Called synchronously on the thread running evaluate().
    public var onPrint: ((String) -> Void)?

    public let limits: Limits

    private let vm: OpaquePointer
    private let ctx: OpaquePointer

    public init(limits: Limits = Limits()) {
        self.limits = limits
        var cfg = JsVmConfig()
        cfg.heap_limit = limits.heapLimit
        guard let vm = js_vm_new(&cfg), let ctx = js_context_new(vm) else {
            preconditionFailure("lamassu: could not allocate a VM")
        }
        self.vm = vm
        self.ctx = ctx
        LamassuEngine.register(self, for: ctx)
        let name = Array("print".utf16)
        _ = name.withUnsafeBufferPointer {
            js_register_native(ctx, $0.baseAddress, $0.count, lamassuPrintThunk, nil)
        }
    }

    deinit {
        LamassuEngine.unregister(ctx)
        js_vm_free(vm)
    }

    /// Compiles and runs `source` in the persistent REPL context, returning
    /// the stringified completion value (the last expression statement).
    /// Throws JSError on a compile error, an uncaught throw, or interrupt.
    @discardableResult
    public func evaluate(_ source: String) throws -> String {
        let units = Array(source.utf16)
        guard !units.isEmpty else { return "undefined" }

        js_vm_clear_interrupt(vm)
        if limits.fuel > 0 {
            js_context_set_fuel(ctx, limits.fuel)
        }

        var errMsg: UnsafePointer<CChar>?
        var errPos: UInt32 = 0
        let fn = units.withUnsafeBufferPointer {
            js_compile_module_repl(ctx, $0.baseAddress, $0.count, &errMsg, &errPos)
        }
        guard js_is_function(fn) else {
            let msg = errMsg.map(String.init(cString:)) ?? "compile error"
            let (line, column) = Self.lineColumn(at: Int(errPos), in: units)
            throw JSError(kind: .syntax, message: msg, line: line, column: column)
        }

        return try protecting(fn) { fnSlot in
            let promise = js_run_module(self.ctx, fnSlot.pointee)
            return try self.protecting(promise) { pSlot in
                let state = js_promise_state(pSlot.pointee)
                let result = js_promise_result(pSlot.pointee)
                return try self.protecting(result) { rSlot in
                    let text = self.stringify(rSlot.pointee)
                    if state == 2 {
                        let kind: JSError.Kind =
                            js_vm_interrupted(self.vm) ? .interrupted : .runtime
                        throw JSError(kind: kind, message: text, line: nil, column: nil)
                    }
                    return text
                }
            }
        }
    }

    /// Asks a running evaluation to stop as soon as it can. Safe to call from
    /// any thread — it only sets a flag. The victim evaluate() throws
    /// JSError(kind: .interrupted); the next evaluate() clears the flag.
    public func interrupt() {
        js_vm_interrupt(vm)
    }

    /// Live bytes on the guest heap.
    public var allocatedBytes: Int {
        js_vm_allocated_bytes(vm)
    }

    // MARK: - internals

    fileprivate func handlePrint(_ line: String) {
        if let onPrint {
            onPrint(line)
        } else {
            print(line)
        }
    }

    /// Runs `body` with `value` held in a GC-protected slot. The engine's GC
    /// roots *slots* (addresses), so the value lives in stable storage for
    /// exactly the protected extent.
    private func protecting<R>(
        _ value: JsValue, _ body: (UnsafeMutablePointer<JsValue>) throws -> R
    ) rethrows -> R {
        var slot = value
        return try withUnsafeMutablePointer(to: &slot) { p in
            js_gc_protect(vm, p)
            defer { js_gc_unprotect(vm, p) }
            return try body(p)
        }
    }

    private func stringify(_ v: JsValue) -> String {
        let s = js_to_string(ctx, v)
        var len = 0
        guard let u = js_string_units(s, &len) else { return "undefined" }
        return String(decoding: UnsafeBufferPointer(start: u, count: len), as: UTF16.self)
    }

    private static func lineColumn(at offset: Int, in units: [UInt16]) -> (Int, Int) {
        var line = 1, column = 1
        for i in 0..<min(offset, units.count) {
            if units[i] == 0x0A {
                line += 1
                column = 1
            } else {
                column += 1
            }
        }
        return (line, column)
    }

    // MARK: - context -> engine registry
    //
    // js_register_native stores a userdata but JsNativeFn never receives it,
    // so the print thunk routes back to its engine by JsContext identity.

    private final class WeakBox {
        weak var engine: LamassuEngine?
        init(_ engine: LamassuEngine) { self.engine = engine }
    }

    private static let registryLock = NSLock()
    private static var registry: [OpaquePointer: WeakBox] = [:]

    private static func register(_ engine: LamassuEngine, for ctx: OpaquePointer) {
        registryLock.lock()
        defer { registryLock.unlock() }
        registry[ctx] = WeakBox(engine)
    }

    private static func unregister(_ ctx: OpaquePointer) {
        registryLock.lock()
        defer { registryLock.unlock() }
        registry[ctx] = nil
    }

    fileprivate static func engine(for ctx: OpaquePointer) -> LamassuEngine? {
        registryLock.lock()
        defer { registryLock.unlock() }
        return registry[ctx]?.engine
    }
}

/// The `print(...args)` native: space-separated, one line per call.
private func lamassuPrintThunk(
    _ ctx: OpaquePointer?, _ thisVal: JsValue, _ args: UnsafePointer<JsValue>?,
    _ argc: Int32, _ result: UnsafeMutablePointer<JsValue>?
) -> Bool {
    result?.pointee = js_undefined()
    guard let ctx else { return true }
    var pieces: [String] = []
    if let args, argc > 0 {
        for i in 0..<Int(argc) {
            // args live on the fiber stack (rooted for the call), and each
            // js_to_string result is copied out before the next safe point.
            let s = js_to_string(ctx, args[i])
            var len = 0
            if let u = js_string_units(s, &len) {
                pieces.append(
                    String(decoding: UnsafeBufferPointer(start: u, count: len), as: UTF16.self))
            }
        }
    }
    let line = pieces.joined(separator: " ")
    if let engine = LamassuEngine.engine(for: ctx) {
        engine.handlePrint(line)
    } else {
        print(line)
    }
    return true
}
