#ifndef LAMASSU_INTERNAL_H
#define LAMASSU_INTERNAL_H

#include "lamassu.h"

#include <string.h>

/*
 * Type of the asynchronous stop flag (JsVm.interrupt). sig_atomic_t is the
 * right choice wherever a signal handler might set it, but wasm has no signals
 * and its <signal.h> is a hard #error without -D_WASI_EMULATED_SIGNAL — and the
 * wasm builds are exactly the ones that must stay dependency-free. There the
 * flag is only ever set by the host between calls, so a plain volatile int
 * carries the same meaning.
 */
#if defined(__wasi__) || defined(__wasm__)
typedef volatile int JsInterruptFlag;
#else
#include <signal.h>
typedef volatile sig_atomic_t JsInterruptFlag;
#endif

typedef enum JsGcKind {
    JS_KIND_STRING = 1,
    JS_KIND_OBJECT = 2,
    JS_KIND_FUNCTION = 3, /* compiled prototype (bytecode + consts) */
    JS_KIND_FIBER = 4,
    JS_KIND_CLOSURE = 5,  /* callable: prototype + captured upvalues */
    JS_KIND_UPVALUE = 6,
    JS_KIND_NATIVE = 7,   /* callable host function */
    JS_KIND_PROMISE = 8,
    JS_KIND_MODULE = 9,
    JS_KIND_BYTECODE = 10, /* opaque module-bytecode buffer (js_bytecode_value) */
    JS_KIND_BIGINT = 11,   /* a BigInt (js_bigint.h); no children */
    JS_KIND_TRACE = 12,    /* the frames of a raise (JsTrace); never a script's value */
} JsGcKind;

/* Every GC allocation starts with this header; cells live on vm->cells. */
typedef struct JsGcCell JsGcCell;
struct JsGcCell {
    JsGcCell *next;
    uint8_t kind;
    uint8_t mark;
};

typedef struct JsString {
    JsGcCell gc;
    uint32_t length; /* UTF-16 code units; no terminator */
    uint32_t hash;
    bool interned;
    uint16_t units[];
} JsString;

/*
 * Open-addressed map, interned-string keys (pointer identity), linear
 * probing with tombstones. used counts live + tombstoned slots.
 * (See JsMap below for how the order of keys is kept.)
 */
typedef struct JsMapEntry {
    JsString *key; /* NULL empty, JS_MAP_TOMBSTONE deleted */
    JsValue value;
} JsMapEntry;

/*
 * Insertion-ordered: `entries` is a DENSE array in the order keys were first
 * set; `index` is the hash table beside it, holding positions into `entries`.
 * A deleted key leaves a tombstone in place, so positions stay valid until
 * the next rebuild. JavaScript enumerates array-index keys ("0", "7", ...)
 * first in ascending order and the other string keys after them in insertion
 * order; `unsorted` says an index key was appended behind a key that belongs
 * after it, and js_map_sort_keys puts the entries in that order. Every walk
 * that enumerates (rather than marks or frees) calls it first.
 */
typedef struct JsMap {
    JsMapEntry *entries; /* [0, used) written, in insertion order; NULL keys past that. The same
                          * allocation carries one JS_ATTR_* byte per entry after the last
                          * (JS_MAP_ATTRS), which moves with its entry and starts at 0. */
    uint32_t *index;     /* hash slot -> entry position, or JS_MAP_EMPTY */
    uint32_t count;      /* live entries */
    uint32_t used;       /* entries written, tombstones included */
    uint32_t capacity;   /* length of `entries`, or 0 */
    uint32_t index_cap;  /* length of `index`: power of two, or 0 */
    bool unsorted;       /* entries are not in enumeration order (see above) */
    bool by_name;        /* a module namespace's: enumerated by name (code units), as V8 does */
    bool gained;         /* a key was appended since this was last cleared. Only the close of an
                          * array's or a string's iteration clears it, on the three prototypes
                          * where a script could put a `return` (js_iter.c, close_direct). */
} JsMap;

#define JS_MAP_EMPTY UINT32_MAX

/* Property attributes, one byte per JsMap entry; 0 is an ordinary data property (enumerable,
 * writable, configurable). Setting an existing key keeps them; deleting a key drops them, so a
 * key made again is ordinary. */
#define JS_ATTR_HIDDEN 1u   /* not enumerable: left out of Object.keys, JSON, spread, assign */
#define JS_ATTR_READONLY 2u /* not writable (Object.defineProperty); its object has JS_OBJ_ATTRS */
#define JS_ATTR_FIXED 4u    /* not configurable: not deletable, attributes final; JS_OBJ_ATTRS too */
#define JS_MAP_ATTRS(m) ((uint8_t *)((m)->entries + (m)->capacity))

#define JS_MAP_TOMBSTONE ((JsString *)(uintptr_t)1)

typedef enum JsObjKind {
    JS_OBJ_PLAIN = 0,
    JS_OBJ_ARRAY = 1,
    JS_OBJ_REGEXP = 2, /* JsRegExp (js_regexp.h); only when LAMASSU_HAS_REGEX */
    JS_OBJ_DATE = 3,   /* JsDateObject (js_date.h) */
    JS_OBJ_MAP = 4,    /* JsMapObj (js_mapobj.h) */
    JS_OBJ_SET = 5,    /* JsSetObj (js_setobj.h) */
    JS_OBJ_ERROR = 6,  /* plain JsObject shape; see js_error.h */
    JS_OBJ_HOST = 7,   /* JsHostObject: an ordinary object owning a host pointer (lamassu.h) */
    JS_OBJ_ITER = 8,   /* JsIterObj (js_iter.h): a Map, Set or RegExp String Iterator */
    JS_OBJ_WEAKMAP = 9, /* JsWeakObj (js_weak.h) */
    JS_OBJ_WEAKSET = 10,
    JS_OBJ_WEAKREF = 11,
    JS_OBJ_FINREG = 12, /* a FinalizationRegistry */
} JsObjKind;

/* JsObject.obj_flags: extensibility, and whole-object integrity. Freezing or sealing sets a flag
 * that stands for every own property being read-only and not configurable (or only not
 * configurable), elements included, rather than writing each property's attributes: the same
 * thing to every observer, at no cost. Per-property attributes (Object.defineProperty) live in
 * the property map's attribute bytes, and JS_OBJ_ATTRS says the object has some, so a write to
 * an object with no flags at all still takes one branch. */
#define JS_OBJ_SEALED 1u /* not extensible; every own property not configurable */
#define JS_OBJ_FROZEN 2u /* sealed, and every own property read-only */
/* A module namespace (a compiled module's exports object): sealed, and its properties
 * read-only to scripts (only the module's own export writes change them). A let/const export
 * holds JS_SPECIAL_TDZ until its declaration runs; every read of the object checks for it. */
#define JS_OBJ_NAMESPACE 4u
#define JS_OBJ_NOEXT 16u /* not extensible (Object.preventExtensions): no new properties */
#define JS_OBJ_ATTRS 32u /* some own property is JS_ATTR_READONLY or JS_ATTR_FIXED */
/* The flags that refuse a new property */
#define JS_OBJ_NO_ADD (JS_OBJ_SEALED | JS_OBJ_FROZEN | JS_OBJ_NOEXT | JS_OBJ_NAMESPACE)
#define JS_OBJ_RAW_JSON 8u /* made by JSON.rawJSON (always frozen too): JSON.stringify writes its rawJSON */
/* Its [[Prototype]] is a function or a promise (Object.create(f), Object.setPrototypeOf, `new` of
 * a function whose prototype is one). A write to it, or through an object that inherits from
 * it, looks up the chain as a write past a prototype with attributes does; nothing else reads
 * it. Kept by js_object_set_proto. */
#define JS_OBJ_PROTO_CELL 64u

/* JsErrorKind (lamassu.h) indexes JsContext.error_protos (js_error.c). */

/* An error path the interpreter loop calls rarely: kept out of line, so that it does not take
 * registers or code space from the hot cases around it. */
/* JS_NOINLINE: an opcode's whole body, kept out of the loop for the same reason, but not cold. */
#if defined(__GNUC__) || defined(__clang__)
#define JS_COLD __attribute__((noinline, cold))
#define JS_NOINLINE __attribute__((noinline))
#define JS_UNLIKELY(c) __builtin_expect(!!(c), 0)
#else
#define JS_COLD
#define JS_NOINLINE
#define JS_UNLIKELY(c) (c)
#endif

typedef struct JsObject {
    JsGcCell gc;
    uint8_t obj_kind;
    uint8_t obj_flags; /* fits in the padding before props; costs nothing */
    uint8_t error_cause; /* JS_OBJ_ERROR only: a JsErrorCause (js_error.c) */
    JsMap props;
    /* JS_OBJ_ARRAY: dense elements; sparse writes fall back to props. */
    JsValue *elems;
    uint32_t elem_count, elem_cap;
    /* [[Prototype]]: undefined = none, else an object, a function or a promise (the last two
     * through js_object_set_proto, which marks the object JS_OBJ_PROTO_CELL). Never a cycle
     * (js_set_proto refuses one). */
    JsValue proto;
} JsObject;

/* A host object (js_host_object_new): an ordinary object in every way guest code can see, whose
 * cell also carries a host pointer. The class's finalizer runs when the collector frees the cell
 * or the VM is freed; external_bytes counts toward collection pacing (JsVm.external_bytes). */
/* An error (obj_kind JS_OBJ_ERROR): an ordinary object plus what its `stack` accessor reads and
 * writes (js_error_stack_accessor). The frames are taken when the error is made
 * (js_error_capture) and turned into text the first time `stack` is read, as V8 does; the
 * first line uses the name and message the error has by then, as Node's formatter does (V8's
 * own keeps the message the error was made with). The same struct, reached by no script, is
 * the record of a stack Error.captureStackTrace gave another object
 * (JsContext.stack_records). */
typedef struct JsErrorFrame {
    JsValue fn;       /* the closure or native running in the frame */
    JsValue receiver; /* `this` it was called with: names a method's type, as V8 does */
    uint32_t pos;     /* source offset of the instruction executing in the frame */
    uint8_t flags;    /* JS_EF_* */
} JsErrorFrame;
#define JS_EF_CONSTRUCT 1u /* entered through `new` */
#define JS_EF_ASYNC 2u     /* an async function waiting on the one above it */

enum { JS_STACK_PENDING, JS_STACK_FRAMES, JS_STACK_VALUE };

/* Frames a stack's text is being made from (JsContext.frame_roots, on the C stack of
 * js_error_stack_accessor): the name and message that head the text are converted by the
 * script's own toString, which may capture the stack again or assign `stack`; these stay what
 * the text is made from, as in V8, and their values GC roots until it is. */
typedef struct JsFrameRoot {
    const JsErrorFrame *frames;
    uint32_t count;
    struct JsFrameRoot *next;
} JsFrameRoot;

/* The engine's accessor functions: what V8 has as getters and setters, by number. A property that
 * is an accessor holds a marker naming two of them (js_accessor, js_bytecode.h); js_accessor_call
 * runs one. The RegExp legacy statics (input, $_, lastMatch, ..., $9: JS_REGEXP_LEGACY_COUNT of
 * them, in V8's order) have a getter and a setter each, as V8 gives each its own. */
#define JS_REGEXP_LEGACY_COUNT 19
typedef enum {
    JS_ACC_NONE,
    JS_ACC_PROTO_GET, JS_ACC_PROTO_SET,   /* Object.prototype.__proto__ */
    JS_ACC_MAP_SIZE, JS_ACC_SET_SIZE,
    JS_ACC_RE_DOTALL, JS_ACC_RE_FLAGS, JS_ACC_RE_GLOBAL, JS_ACC_RE_HASINDICES, JS_ACC_RE_IGNORECASE,
    JS_ACC_RE_MULTILINE, JS_ACC_RE_SOURCE, JS_ACC_RE_STICKY, JS_ACC_RE_UNICODE, JS_ACC_RE_UNICODESETS,
    JS_ACC_RE_LEGACY_GET,
    JS_ACC_RE_LEGACY_SET = JS_ACC_RE_LEGACY_GET + JS_REGEXP_LEGACY_COUNT,
    JS_ACC_ITER_CTOR_GET = JS_ACC_RE_LEGACY_SET + JS_REGEXP_LEGACY_COUNT, JS_ACC_ITER_CTOR_SET,
    JS_ACC_STACK_GET, JS_ACC_STACK_SET,   /* an error's `stack` */
    JS_ACC_FN_ARGUMENTS_GET, JS_ACC_FN_ARGUMENTS_SET, JS_ACC_FN_CALLER_GET, JS_ACC_FN_CALLER_SET,
    JS_ACC__COUNT
} JsAccessorId;

/*
 * The builtin properties a builtin algorithm reads from the object it works on, where V8 reads
 * them: RegExp.prototype's `exec`, `flags` and the flag getters, `constructor`, `source`;
 * Array.prototype's `constructor`; the `next` of the iterator an array or a string is iterated
 * with. A
 * script may replace any of them, and the algorithm then goes through the replacement
 * (RegExpExec calls whatever `exec` is; split constructs whatever `constructor` names), which is
 * the slow, general path. While one is still the value the engine put there ("intact"), reading
 * it has no effect a script could see, and the algorithm takes its direct path. A watch
 * remembers where the property sits in its holder's map, so asking is two compares (js_intact);
 * nothing is hooked into property writes.
 */
enum {
    JS_INTACT_RE_EXEC,
    JS_INTACT_RE_FLAGS,
    JS_INTACT_RE_HASINDICES, JS_INTACT_RE_GLOBAL, JS_INTACT_RE_IGNORECASE, JS_INTACT_RE_MULTILINE,
    JS_INTACT_RE_DOTALL, JS_INTACT_RE_UNICODE, JS_INTACT_RE_UNICODESETS, JS_INTACT_RE_STICKY,
    JS_INTACT_RE_CONSTRUCTOR,
    JS_INTACT_RE_SOURCE,
    JS_INTACT_ARRAY_CONSTRUCTOR, /* Array.prototype.constructor */
    JS_INTACT_ARRAY_ITER_NEXT,   /* the Array Iterator prototype's next */
    JS_INTACT_STRING_ITER_NEXT,  /* the String Iterator prototype's next */
    JS_INTACT_PROMISE_THEN,        /* Promise.prototype.then */
    JS_INTACT_PROMISE_CONSTRUCTOR, /* Promise.prototype.constructor */
    JS_INTACT__COUNT
};

typedef struct JsIntact {
    struct JsObject *holder; /* the builtin object that has the property (a GC root itself) */
    JsString *key;           /* interned */
    JsValue value;           /* what the engine put there */
    uint32_t pos;            /* the entry's position in holder->props when last looked up */
} JsIntact;

/* The constructors V8 gives a [Symbol.species] getter (js_species; JsContext.species_ctors) */
enum { JS_SPECIES_ARRAY, JS_SPECIES_MAP, JS_SPECIES_SET, JS_SPECIES_PROMISE, JS_SPECIES_REGEXP,
       JS_SPECIES__COUNT };

typedef struct JsErrorObject {
    JsObject obj;
    JsValue stack;         /* JS_STACK_VALUE: the text, or whatever a script assigned */
    JsErrorFrame *frames;  /* JS_STACK_FRAMES: raw allocation, frame_count entries */
    uint32_t frame_count;
    uint8_t stack_state;   /* JS_STACK_*: PENDING = made by the interpreter for a raise,
                            * captured when the raise unwinds */
    bool has_origin;       /* a syntax error a module's source gave: where in it, which V8
                            * keeps with the error too, and whatever becomes of `stack` */
    uint32_t origin_pos;
    JsString *origin_spec; /* the module's specifier, or NULL */
} JsErrorObject;

/* Whether enumerating O's own properties leaves out the one at position I of its map (a
 * non-enumerable one: an error's `message`, a prototype's `constructor`, a builtin's methods) */
static inline bool js_prop_hidden_at(const JsObject *o, uint32_t i) {
    return o->props.entries && (JS_MAP_ATTRS(&o->props)[i] & JS_ATTR_HIDDEN);
}

/* A property's attributes A (its map byte) with O's integrity applied: what it is to a script */
static inline uint8_t js_obj_attrs(const JsObject *o, uint8_t a) {
    if (o->obj_flags & JS_OBJ_SEALED)
        a |= JS_ATTR_FIXED;
    if (o->obj_flags & JS_OBJ_FROZEN)
        a |= JS_ATTR_READONLY | JS_ATTR_FIXED;
    return a;
}

typedef struct JsHostObject {
    JsObject obj; /* obj_kind == JS_OBJ_HOST */
    const JsHostClass *cls;
    void *ptr;
    size_t external_bytes;
} JsHostObject;
size_t js_host_object_release(JsVm *vm, JsObject *o); /* js_object.c; finalizes, returns the cell size */

/* Compiled function. code/consts/lines are raw allocations owned by the
 * cell; consts are traced by the GC. */
typedef struct JsLineEntry {
    uint32_t off; /* code offset */
    uint32_t pos; /* source offset */
} JsLineEntry;

/* A call or `new` site's callee as source-like text ("s.fillet", "foo", "a[0]"), so a failed call
 * can say what it tried to call. off is the CALL/NEW instruction's code offset; the text is
 * callee_text[start .. start + len). Entries are in increasing off order. */
typedef struct JsCalleeName {
    uint32_t off;
    uint32_t start;
    uint32_t len;
} JsCalleeName;

typedef struct JsUpvalDesc {
    uint16_t idx;      /* parent local slot, or parent upvalue index */
    uint8_t from_local;
} JsUpvalDesc;

#define JS_FN_ARROW 0x01
#define JS_FN_HAS_REST 0x02
#define JS_FN_ASYNC 0x04
/* The body (or a nested arrow) reads `arguments`: frame_setup materialises a
 * plain array of the actual args into local slot n_params. Never set on an
 * arrow (arrows see the enclosing function's `arguments` as an upvalue). */
#define JS_FN_USES_ARGUMENTS 0x08
/* An object literal's method (`{ m() {} }`, async or not). With ARROW and ASYNC, one of the kinds
 * of function that is no constructor and has no `prototype` (js_is_constructor). */
#define JS_FN_METHOD 0x10
#define JS_FN_NEW_TARGET 0x20 /* uses new.target: frame_setup puts it in the local after `arguments` */

typedef struct JsModule JsModule;

typedef struct JsFunctionCell {
    JsGcCell gc;
    uint8_t *code;
    uint32_t code_len, code_cap;
    JsValue *consts;
    uint32_t const_count, const_cap;
    JsLineEntry *lines;
    uint32_t line_count, line_cap;
    JsUpvalDesc *upvals;
    uint16_t n_upvals, upval_cap;
    uint16_t n_params;
    uint16_t length;    /* `length`: the parameters before the first default or rest */
    uint8_t fn_flags;
    uint32_t n_locals, max_stack;
    JsString *name;     /* may be NULL */
    JsModule *module;   /* module this fn belongs to (exports/imports); may be NULL */
    /* Retained per-code-byte bitmap (see JSBC_B_* below), populated by the
     * bytecode verifier and kept ONLY for functions loaded from an untrusted
     * cache. NULL for compiler-emitted functions, whose RET_SUB return
     * addresses cannot be forged. Lets RET_SUB reject any return address that
     * is not a real GOSUB resume point. */
    uint8_t *insn_boundary;
    /* (GOSUB resume offset, verified operand depth) pairs, also verifier-built
     * and loaded-functions-only. RET_SUB requires its popped return address and
     * the depth it arrives at to match one of these. NULL/0 when the function
     * has no subroutines, or for compiler-emitted code. */
    uint32_t *gosub_rets;
    uint32_t gosub_ret_count;
    /* Callee text per call site (JsCalleeName), read only when a call fails. */
    JsCalleeName *callees;
    uint32_t callee_count, callee_cap;
    uint16_t *callee_text;
    uint32_t callee_text_len, callee_text_cap;
    /* The text Function.prototype.toString gives: units [src_start, src_end) of SOURCE, the
     * whole compilation unit's source, which every function compiled from it shares. The root
     * function holds it too (src_end 0) so the bytecode writer has it. NULL when no function in
     * the unit has text. Cold: last. */
    JsString *source;
    uint32_t src_start, src_end;
    /* For a stack trace: the name V8 infers for an anonymous function from where it is put
     * (`x.y` for `x.y = function () {}`, `F.m` for `F.prototype.m = ...`), when `name` is "". */
    JsString *debug_name;
} JsFunctionCell;

/* JsFunctionCell.insn_boundary bits. */
#define JSBC_B_INSN 1u      /* an instruction starts at this offset */
#define JSBC_B_GOSUB_RET 2u /* a GOSUB resumes here — the only legal RET_SUB target */

typedef struct JsFiber JsFiber;

/* One native call in progress, on the C stack of whoever called it: where it was called from,
 * so a stack trace can put it between the frames of the fibers around it (V8's
 * `at Array.map (<anonymous>)`). */
#define JS_NATIVE_KEEP_MAX 8
typedef struct JsNativeAct JsNativeAct;
struct JsNativeAct {
    JsNativeAct *outer;
    struct JsNative *nf;
    JsValue this_val;
    /* its arguments, GC roots while it runs: the caller's own copy may be the only reference left
     * (Function.prototype.apply's list, emptied by an argument's valueOf) */
    const JsValue *args;
    uint32_t argc;
    JsFiber *fiber;       /* the fiber whose top frame called it, or NULL (the host) */
    uint32_t frame_count; /* that fiber's frame count at the call */
    bool is_new;
    /* values the native made and must keep across guest code it runs (a string converted from its
     * receiver while an argument's valueOf runs): js_native_keep; GC roots */
    uint8_t nkeep;
    JsValue keep[JS_NATIVE_KEEP_MAX];
};

/*
 * Captured variable. While open it aliases fiber->stack[slot] (offset, not
 * pointer — the stack may be reallocated); closing copies the value into
 * `closed`.
 */
typedef struct JsUpvalue JsUpvalue;
struct JsUpvalue {
    JsGcCell gc;
    JsFiber *fiber;
    uint32_t slot;
    bool open;
    JsValue closed;
    JsUpvalue *next_open; /* fiber's open-upvalue list */
};

typedef struct JsClosure {
    JsGcCell gc;
    JsFunctionCell *fn;
    JsValue this_val; /* arrows: lexically captured `this` */
    bool has_this;
    bool length_deleted; /* `delete f.length` ran: it reads 0 (Function.prototype's) */
    bool proto_made;     /* `prototype` holds the function's prototype (see js_is_constructor) */
    bool proto_readonly; /* Object.defineProperty made `prototype` read-only (fits the padding) */
    uint16_t n_upvals;
    bool name_deleted;   /* `delete f.name` ran: no own name (it reads Function.prototype's "") */
    /* A constructor's `prototype`: any value a script gives it. Made (an object whose hidden
     * `constructor` is the function) the first time it is read or `new` needs it. */
    JsValue prototype;
    /* Properties a script gives the function (statics on a constructor: F.create = ...); a plain
     * object, created on the first write. NULL until then. */
    JsObject *props;
    /* The name a computed key gave it (JS_OP_SET_FUNCTION_NAME); NULL: fn->name. */
    JsString *name;
    /* Its [[Prototype]] once Object.setPrototypeOf has set one (proto_set): an object, function
     * or promise, or undefined for null. Until then Function.prototype. */
    JsValue own_proto;
    bool proto_set;
    JsUpvalue *upvals[];
} JsClosure;

/* Bound native: receives a captured value (used for promise resolve/reject). */
typedef bool (*JsBoundFn)(JsContext *ctx, JsValue bound, JsValue this_val,
                          const JsValue *args, int argc, JsValue *result);

typedef struct JsNative {
    JsGcCell gc;
    JsNativeFn fn;       /* used when !is_bound */
    JsBoundFn bound_fn;  /* used when is_bound */
    void *ud;
    JsString *name;      /* may be NULL */
    JsObject *statics;   /* namespace statics (String.fromCharCode, ...); may be NULL */
    /* A constructor's `prototype` (JsClosure's, likewise made when first needed). A builtin
     * constructor's is set when it is made and is read-only: instances of builtin types (RegExp,
     * Date, Map, Set, ...) point their [[Prototype]] at it. */
    JsValue prototype;
    JsValue bound;       /* captured value for bound natives */
    JsObject *props;     /* properties a script gives it (like JsClosure.props); NULL until written */
    bool is_bound;
    bool builtin;        /* made by the engine (js_builtins_init): shown in a stack trace */
    bool name_deleted;   /* `delete f.name`: reads "", while toString still gives the name */
    uint8_t length;      /* `length`: V8's for a builtin (js_builtin_lengths_apply), else 0 */
    uint8_t ctor;        /* JS_CTOR_*: whether and how `new` may call it */
    bool proto_made;     /* `prototype` holds it */
    bool proto_readonly; /* Object.defineProperty made `prototype` read-only */
    bool length_deleted; /* `delete f.length` ran: no own length (it reads 0) */
    struct JsNative *parent; /* its [[Prototype]] when that is another constructor (TypeError's
                              * is Error), whose statics it inherits; NULL: Function.prototype */
    JsValue own_proto;       /* its [[Prototype]] once a script has set one (proto_set), as
                              * JsClosure's; replaces parent and Function.prototype */
    bool proto_set;
} JsNative;

/* JsNative.ctor. A function from bind() is a constructor when its target is (js_is_constructor). */
enum {
    JS_CTOR_NONE,     /* not a constructor, no `prototype`: builtin functions, promise resolvers */
    JS_CTOR_FACTORY,  /* a builtin constructor (Map, Error, ...): makes its own object, and its
                       * `prototype` is read-only */
    JS_CTOR_ORDINARY, /* a host's native (V8's default for one): `new` makes `this` from its
                       * `prototype`, as for a script's function, and gives it unless the native
                       * returns an object */
};

/* ---- promises & microtasks ---- */

typedef enum JsPromiseState {
    JS_PROMISE_PENDING = 0,
    JS_PROMISE_FULFILLED = 1,
    JS_PROMISE_REJECTED = 2,
} JsPromiseState;

/*
 * A pending promise's subscribers. `then` reactions call handlers and settle
 * a derived promise; `await` reactions resume a suspended async fiber.
 */
typedef struct JsReaction JsReaction;
struct JsReaction {
    JsReaction *next;
    uint8_t kind; /* 0 = then, 1 = await */
    JsValue on_fulfilled; /* then: callable or undefined */
    JsValue on_rejected;  /* then: callable or undefined */
    struct JsPromise *result; /* then: derived promise (may be NULL) */
    struct JsFiber *fiber;    /* await: fiber to resume */
};

/*
 * The frames of one raise, innermost first, as the context's trace had them when a promise was
 * rejected by it. The context has one trace and the next error to leave a function replaces it;
 * a rejection is read later (after the jobs, or when nothing turned out to handle it), so what
 * it needs it keeps. Shared by the promises one raise rejects; never changed once made.
 */
typedef struct JsTrace {
    JsGcCell gc;
    uint32_t count;
    JsStackFrame frames[];
} JsTrace;

typedef struct JsPromise {
    JsGcCell gc;
    uint8_t state;
    JsValue value;     /* fulfillment value or rejection reason */
    JsReaction *reactions; /* non-empty only while PENDING */
    JsObject *props;       /* properties a script gives it (like JsClosure.props); NULL until written */
    JsValue own_proto;     /* its [[Prototype]] once a script has set one (proto_set), as
                            * JsClosure's; until then Promise.prototype */
    bool proto_set;
    bool handled;          /* [[PromiseIsHandled]]: something has reacted to it */
    struct JsTrace *trace; /* rejected by an error leaving a fiber: where that raise was thrown
                            * (js_promise_error_stack); else NULL */
} JsPromise;

/* A queued microtask. Raw allocation; the VM's queue is a GC root. */
typedef struct JsJob JsJob;
struct JsJob {
    JsJob *next;
    uint8_t kind;   /* 0 = then, 1 = await */
    bool fulfilled; /* settlement branch */
    JsValue value;  /* settlement value */
    /* then */
    JsValue on_fulfilled, on_rejected;
    struct JsPromise *result;
    /* await */
    struct JsFiber *fiber;
    bool is_throw;
    struct JsTrace *trace; /* a rejection's: the promise's (JsPromise.trace) */
};

typedef enum JsRunStatus {
    JS_RUN_DONE = 0,
    JS_RUN_ERROR = 1,
    JS_RUN_SUSPENDED = 2, /* parked at await */
    JS_RUN_READY = 3,     /* a module body parked at JS_OP_MODULE_READY (stop_at_ready) */
} JsRunStatus;

/* ---- ES modules ---- */

typedef enum JsImportKind {
    JS_IMPORT_NAMED = 0,   /* import { x } / { x as y } */
    JS_IMPORT_DEFAULT = 1, /* import d from */
    JS_IMPORT_NS = 2,      /* import * as ns */
    JS_IMPORT_SIDE = 3,    /* import 'm' (no bindings) */
    JS_IMPORT_META = 4,    /* import.meta: IMPORT_NS of it pushes the module's meta object, which
                            * linking makes (the namespace of a synthetic module) */
} JsImportKind;

typedef struct JsModuleImport {
    JsString *specifier;     /* module to import from */
    JsString *imported_name; /* export name in source (NULL for ns/side); after linking, the
                              * name in `source` that owns the binding, or NULL where the name
                              * turned out to be a module's namespace (`requested` is still set) */
    uint8_t kind;            /* JsImportKind */
    JsModule *source;        /* resolved at link */
    JsString *requested;     /* the name as this module imports it (link may follow re-exports
                              * to another): for the missing-export error */
    uint32_t pos;            /* where a link error about it points, as V8 has it: the name asked
                              * for (`a` of `{ a as b }`; the binding of a default import) */
} JsModuleImport;

/*
 * A re-export: `export * from`, `export * as ns from`, or
 * `export { x as y } from`. Linking binds each name to the binding it stands for
 * (JsExportBinding below); none is a copy.
 */
typedef enum JsReExpKind {
    JS_REEXP_ALL = 0,   /* export * from 'm'  -> every name of m but `default` */
    JS_REEXP_NS = 1,    /* export * as ns from 'm' -> exports[ns] = m namespace */
    JS_REEXP_NAMED = 2, /* export { x as y } from 'm' -> y is m's binding x */
    JS_REEXP_LOCAL = 3, /* export { x as a, x as b }: `exported` (b) is one more name of this
                         * module's own export `imported` (a); no specifier */
} JsReExpKind;

typedef struct JsStarExport {
    JsString *specifier; /* NULL for LOCAL */
    JsModule *source;
    uint8_t kind;
    JsString *imported; /* NAMED: source export name; LOCAL: the binding's first export name */
    JsString *exported; /* NS: binding name; NAMED, LOCAL: exported name */
    uint32_t pos;       /* where a link error about it points, as V8 has it. ALL: the `*`;
                         * NAMED: the name asked for (`x` of `{ x as y }`), and for an imported
                         * name exported again, the import's */
} JsStarExport;

/*
 * A name a module exports without holding the binding under that name: the binding it stands
 * for, as the module and the export name that own it (V8's cell in the module's exports table).
 * Linking adds one for every `export { x } from`, every name an `export *` brings and every
 * further name of a local export; two names are the same binding when both fields are equal.
 * A NULL origin_name is the namespace of `origin`: what `export * as ns from` and `import * as
 * ns from; export { ns }` export. A module has one, so two modules re-exporting the namespace
 * of a third export one binding (V8's namespace cell), not two that conflict under `export *`.
 */
typedef struct JsExportBinding {
    JsString *name;
    JsModule *origin;
    JsString *origin_name;
    bool alias;  /* LOCAL: the compiler writes this slot with the binding's other names */
    bool hidden; /* resolved after the namespace was made: V8's namespace object keeps the
                  * names it was made with, so no slot */
} JsExportBinding;

/*
 * The other end of a JsExportBinding, on the module that owns the binding: a slot of another
 * module's namespace that shows it, rewritten at every write of the export (JS_OP_SET_EXPORT).
 * The namespace is held weakly: the collector drops the mirrors of one that died
 * (js_module_mirrors_prune), so an evicted re-exporter does not stay alive through them.
 */
typedef struct JsExportMirror {
    JsObject *exports; /* the namespace object written (not marked) */
    JsString *name;    /* its key there */
    JsString *key;     /* the export mirrored */
    uint32_t next;     /* index + 1 of the next mirror of the same export; 0 ends the chain */
} JsExportMirror;

typedef enum JsModuleStatus {
    JS_MOD_FETCHING = 0, /* loader promise in flight; body/exports unknown */
    JS_MOD_UNLINKED = 1,
    JS_MOD_LINKING = 2,
    JS_MOD_LINKED = 3,
    JS_MOD_EVALUATING = 4,       /* on the path, or in an open component, of an evaluation */
    JS_MOD_EVALUATING_ASYNC = 5, /* waiting for a top-level await, its own or a dependency's */
    JS_MOD_EVALUATED = 6,
    JS_MOD_ERRORED = 7,          /* its evaluation threw eval_error (V8's kErrored): it loads and
                                    links as any other module, and throws again where it is run */
    JS_MOD_LOAD_FAILED = 8,      /* the loader or the compiler refused it, with eval_error */
} JsModuleStatus;

struct JsModule {
    JsGcCell gc;
    JsString *specifier;      /* canonical specifier (cache/identity key) */
    JsFunctionCell *body;     /* compiled module body (NULL until compiled) */
    JsObject *exports;        /* namespace/exports object (live bindings) */
    JsModuleImport *imports;  /* import descriptors (indexed by GET_IMPORT) */
    uint32_t import_count;
    JsStarExport *stars;      /* export * from sources */
    uint32_t star_count;
    JsString **dep_specs;     /* all distinct dependency specifiers (raw) */
    uint32_t dep_spec_count;
    JsModule **deps;          /* resolved dependency modules (parallel to dep_specs) */
    uint32_t dep_count;
    uint8_t status;
    bool synthetic;           /* exports adopted from a loader-resolved object */
    bool entry;               /* the root of js_eval_entry: may `return` at top level */
    JsValue eval_error;
    JsValue returned;         /* entry only: what the body returned (undefined off the end) */
    /* Settles once this module's source/bytecode/synthetic exports have
     * arrived and been compiled/adopted (dep_specs known; deps NOT yet
     * fetched) — a shallow per-module signal the graph loader composes.
     * Fulfills with the module itself (disguised as a JsValue), rejects with
     * the load error. Rooted through the registry for the module's lifetime. */
    JsPromise *fetch_promise;
    /* The body's fiber between instantiation (its functions made, parked at JS_OP_MODULE_READY)
     * and evaluation, which resumes it; NULL before and after. */
    JsFiber *fiber;
    /* Linking, as V8's FinishInstantiate does it (js_module.c): 0 until the names this module
     * imports and re-exports are resolved, 1 while its strongly connected component is being
     * resolved, 2 once that is done and its body's functions are made */
    uint8_t link_state;
    bool ns_ready;            /* the names its `export *` bring are in its namespace */
    bool ns_wanted;           /* somebody takes its namespace (import *, export * as, import()) */
    bool fetch_mark;          /* visited by the walk that is gathering them */
    uint32_t dfs_index, dfs_ancestor;
    /* Evaluation, as V8's SourceTextModule does it (js_module.c, "evaluate") */
    bool exec_mark;             /* a dependency's end made it ready to run, and it has not yet */
    uint32_t eval_pass;         /* the evaluation whose stack it is on; 0: none */
    uint32_t pending_async_deps; /* the dependencies it waits for */
    uint64_t async_ordinal;     /* 0: nothing of it waits; 1: what waited is done; from 2: its
                                   turn among the modules that wait (V8's async evaluation ordinal) */
    JsPromise *top_capability;  /* what evaluating it from the top came to (kept on its cycle
                                   root, for a module of a cycle); NULL: never asked */
    JsModule *cycle_root;       /* the module its strongly connected component closed at; NULL
                                   until it has */
    JsModule **async_parents;   /* the modules that wait for it, an entry for each time they do */
    uint32_t async_parent_count, async_parent_cap;
    JsExportBinding *bindings; /* the exported names that are another binding */
    uint32_t binding_count, binding_cap;
    uint32_t binding_committed; /* [0, this) are in `exports` and mirrored */
    JsMap bound;               /* name -> index in `bindings` */
    JsExportMirror *mirrors;   /* the namespaces showing this module's exports */
    uint32_t mirror_count, mirror_cap;
    JsMap mirrored;            /* export name -> index + 1 of its first mirror */
    JsModule *mirror_next;     /* vm->mirror_modules: every module with mirrors */
};

/* Opaque host-provided module bytecode; bytes owned by the cell. */
typedef struct JsBytecode {
    JsGcCell gc;
    size_t length;
    uint8_t bytes[];
} JsBytecode;

typedef struct JsFrame {
    JsClosure *closure;
    uint32_t ip;
    uint32_t base; /* stack offset: locals at base..base+n_locals;
                      callee at base-2, this at base-1 */
    bool is_construct; /* frame entered via `new`: RETURN substitutes `this`
                           for a non-object return value */
} JsFrame;

typedef struct JsTryEntry {
    uint32_t frame_count; /* unwind to this many frames */
    uint32_t sp;
    uint32_t target;      /* handler ip in that frame's function */
} JsTryEntry;

/* Suspendable execution state; call frames never live on the C stack. */
struct JsFiber {
    JsGcCell gc;
    JsValue *stack;
    uint32_t sp, stack_cap;
    JsFrame *frames;
    uint32_t frame_count, frame_cap;
    JsTryEntry *trys;
    uint32_t try_count, try_cap;
    JsUpvalue *open_upvals;
    JsFiber *caller; /* fiber that invoked a native that re-entered; GC root */
    JsPromise *result_promise; /* async fiber: promise to settle on completion */
    JsValue error;
    bool failed;
    bool is_async;   /* body may suspend at await */
    bool stop_at_ready; /* a module body being instantiated: park at JS_OP_MODULE_READY */
    uint32_t err_pos;
    /* frames at the last raise, innermost first (js_context_error_stack) */
    JsStackFrame trace[JS_TRACE_MAX];
    uint32_t trace_count;
    uint64_t fuel; /* 0 = unlimited */
};

/* Weak set of interned strings, content-keyed. Sweep removes dead entries. */
typedef struct JsAtomTable {
    JsString **slots; /* NULL empty, JS_MAP_TOMBSTONE deleted */
    uint32_t count;
    uint32_t used;
    uint32_t capacity;
} JsAtomTable;

/* ---- frontend <-> runtime seam: compiling a module from SOURCE ----
 *
 * The module pipeline is runtime code, but one branch of it — a loader that
 * fulfills with a source string — needs a parser. Rather than let that one
 * branch drag the whole frontend into every build, it goes through a hook the
 * frontend installs (js_enable_source_modules, in lamassu_compile.h).
 *
 * NULL is the normal, supported state for a runtime-only process: source
 * specifiers fail with a clear error instead of compiling. That makes "this
 * process cannot turn source into code" a property enforced by what was
 * LINKED, not by a policy someone has to remember to apply — which is the
 * point of the split for a fleet running precompiled bytecode.
 */
typedef struct JsModError {
    const char *msg;
    uint32_t pos;
    bool oom;
} JsModError;

typedef bool (*JsModuleCompileFn)(JsContext *ctx, JsModule *m, const uint16_t *source,
                                  size_t source_len, JsModError *err);

/* JsContext.iter_protos: one prototype per iterator family (js_iter.h, JS_ITER_PROTO_*) */
#define JS_ITER_PROTO_COUNT 7

struct JsContext {
    /* The host's own pointer — see js_context_set_userdata. Never read,
     * freed or copied by the engine. */
    void *host_ud;
    JsVm *vm;
    JsContext *next;
    JsContext **prev_link;
    JsObject *globals;
    /* String/Number/Promise are primitives or a distinct GC kind (no
     * [[Prototype]] slot to hang a real chain off yet), so their methods
     * still live in a hidden per-context table that property lookup falls
     * back to directly (prototype-lite: invisible and immutable from
     * scripts). Array/RegExp/Date/Map/Set instances are JS_KIND_OBJECT
     * cells with a real `proto` field, so those use genuine, script-visible
     * X.prototype objects instead — see array_proto etc. below. */
    JsObject *string_methods;
    JsObject *number_methods;
    JsObject *bigint_methods; /* BigInt.prototype; GC root */
    JsObject *boolean_methods; /* Boolean.prototype: toString, valueOf */
    JsObject *function_methods; /* call, for every function value (closures and natives) */
    struct JsNative *function_proto; /* Function.prototype itself, as V8 has it: a builtin
                                      * function (callable, returns undefined, not a constructor)
                                      * whose own properties are function_methods and whose
                                      * [[Prototype]] (own_proto) is Object.prototype */
    JsObject *async_function_proto; /* AsyncFunction.prototype: an async function's [[Prototype]],
                                     * a plain object inheriting from Function.prototype */
    JsObject *promise_methods;
    /* Real, script-visible prototype objects — Ctor.prototype is this same
     * object, and instances' [[Prototype]] is set to it at allocation, so
     * property lookup reaches methods purely via the normal own-prop-miss
     * -> proto-chain walk in get_property, with no per-kind special case.
     * Cached here so allocation sites (js_array_new_cell, alloc_mapobj, ...)
     * don't need to look "Ctor.prototype" up through a property read on
     * every single instance they create. */
    struct JsNative *accessor_fns[JS_ACC__COUNT]; /* the accessor functions as a descriptor shows
                             * them (`get __proto__`, `get size`, ...), each made when first asked
                             * for (js_accessor_function). GC roots. */
    JsObject *object_proto; /* root of the chain; its own ->proto is undefined
                             * (no [[Prototype]]), matching real JS's `null`.
                             * Created first in js_builtins_init, before
                             * anything below — see js_object_new's bootstrap
                             * comment in js_object.c. */
    JsObject *array_proto;
    JsObject *regexp_proto; /* NULL unless built with LAMASSU_HAS_REGEX */
    JsObject *date_proto;
    JsObject *map_proto;
    JsObject *set_proto;
    /* %IteratorPrototype% (Iterator.prototype) and the Map, Set, RegExp String, Array and String
     * Iterator, Iterator Helper and %WrapForValidIteratorPrototype% prototypes that chain to it
     * (js_iter.h; indexed by JS_ITER_PROTO_*) */
    JsObject *iterator_proto;
    struct JsNative *iterator_ctor; /* `Iterator`: what Iterator.prototype.constructor's getter
                                     * gives. GC root. */
    JsObject *iter_protos[JS_ITER_PROTO_COUNT];
    JsObject *error_protos[JS_ERROR_KIND_COUNT]; /* Error.prototype and subclasses */
    /* persistent REPL lexical scope: top-level let/const/function bindings
     * that survive across evaluations. repl_scope holds values (TDZ sentinel
     * for uninitialized); repl_const marks the const names. Both lazily
     * created and only used by REPL-mode compilation. */
    JsObject *repl_scope;
    JsObject *repl_const;
    /* module registry (cache by canonical specifier; all GC roots). The array
     * is the source of truth for iteration/marking; module_index is a lookup
     * accelerator (interned specifier -> module value) so resolving an import
     * is O(1) rather than a linear scan. Its keys/values are kept alive via the
     * array, so it needs no separate marking. */
    JsModule **modules;
    uint32_t module_count, module_cap;
    JsMap module_index;
    JsModuleLoader loader;          /* async module loader; may be NULL */
    JsModuleCanonicalizer canon;    /* sync specifier canonicalizer; may be NULL */
    void *loader_ud;                /* shared userdata for loader + canon */
    /* Source->code for module specifiers the loader answers with a string.
     * NULL unless the frontend is linked AND js_enable_source_modules was
     * called; see the seam comment in js_bytecode.h. */
    JsModuleCompileFn compile_source;
    /* While js_eval_entry starts its graph: the canonical root specifier, so the root's record is
     * made as an entry (JsModule.entry). NULL otherwise. */
    JsString *entry_spec;
    uint64_t async_evaluations; /* the modules that have waited so far (JsModule.async_ordinal) */
    uint32_t eval_passes;       /* the evaluations so far (JsModule.eval_pass) */
    JsFiber *fiber;     /* current/last fiber; GC root */
    uint64_t fuel;      /* budget armed by js_context_set_fuel; 0 = unlimited */
    /* What remains of `fuel` for the current turn — the top-level run PLUS the
     * microtask drain it feeds. Every top-level fiber draws from this and
     * writes its remainder back, so a script that keeps re-queueing microtasks
     * cannot mint a fresh budget per job. Re-armed by js_context_set_fuel. */
    uint64_t fuel_left;
    uint32_t error_pos; /* source offset of last runtime error */
    JsNative *running_native; /* the native executing right now (js_native_userdata); NULL outside one */
    JsStackFrame error_trace[JS_TRACE_MAX]; /* its frames, innermost first */
    uint32_t error_trace_count;
    JsValue error_trace_value; /* the error those frames belong to: a rethrow of the same value
                                * (an await resuming with a rejection) appends its frames */
    /* Whether the raise the trace is of is still on its way out: set when an error leaves a
     * fiber, and when a job hands on a rejection that has a trace (the trace is put back
     * first); cleared as soon as script code runs again (a fiber is entered, a handler catches,
     * a native returns). A promise rejected meanwhile with the same value was rejected by that
     * raise, and keeps the trace as a cell of its own (raise_trace: the one made for the trace
     * as it stands, NULL when none was or the trace has changed since). */
    bool raise_fresh;
    struct JsTrace *raise_trace;
    JsRejectionTracker rejection_fn; /* js_set_rejection_tracker */
    void *rejection_ud;
    uint32_t reentry_depth; /* nested interpreter entries (native re-entry);
                             * bounds C-stack recursion the per-fiber frame
                             * limit can't see */
    JsNativeAct *native_act;  /* the natives running now, innermost first (on the C stack) */
    JsNative *error_ctor;     /* `Error`, for Error.stackTraceLimit; GC root */
    bool builtins_ready;      /* natives made from here on are the host's, not builtins */
    JsObject *math_obj, *json_obj; /* Math and JSON, whose toString tags are theirs; GC roots */
    JsObject *weakmap_proto, *weakset_proto, *weakref_proto, *finreg_proto; /* GC roots */
    JsObject *weak_kept; /* WeakRef targets kept alive to the end of the job (an array); GC root */
    /* What Error.captureStackTrace gave an object that is not an error: a weak map of the
     * runtime's own (js_weak_side_set) from the object to a record, a JsErrorObject no script
     * sees, holding the frames and then the text (V8 keeps them under a private symbol). NULL
     * until the first one; GC root. */
    JsObject *stack_records;
    JsFrameRoot *frame_roots; /* GC roots */
    /* RegExp's legacy statics (RegExp.$1, lastMatch, input, ...): the last successful match's
     * subject and groups 0-9 ([start, end], -1 for none), its last group, and RegExp.input
     * (which a script may set): what the constructor's accessors read. GC roots: re_subject,
     * re_input. */
    struct JsNative *regexp_ctor;
    JsString *re_subject;
    JsValue re_input;
    int32_t re_caps[20];
    int32_t re_last_paren[2];
    /* What the last failed conversion threw (js_op_to_string and friends), until js_take_error
     * hands it on; undefined otherwise. GC root. */
    JsValue pending_error;
    /* The arrays Array.prototype.join is joining now, outermost first: one met again is cyclic
     * and joins as "", as in V8. On the C stack's behalf; its entries are rooted by their callers. */
    JsObject **join_stack;
    uint32_t join_len, join_cap;
    /* Last, so that what the interpreter reads on every call stays where it was */
    JsIntact intact[JS_INTACT__COUNT]; /* js_intact's watches. GC roots (key, value). */
    struct JsNative *species_ctors[JS_SPECIES__COUNT]; /* Array, Map, Set, Promise, RegExp: the
                             * constructors with a [Symbol.species] in V8 (js_species). GC roots. */
};

struct JsVm {
    JsReallocFn realloc_fn;
    void *alloc_ud;
    size_t bytes_live; /* all bytes obtained through js_realloc_raw */
    size_t external_bytes; /* host memory owned by live host objects: paces the
                            * collector (not charged against heap_limit) */
    size_t heap_limit; /* 0 = unlimited; enforced at cell allocation */
    uint64_t rng_state; /* Math.random xorshift state */
    uint32_t hash_seed; /* per-VM string-hash seed; 0 unless the embedder set a
                         * seed, in which case it randomizes atom/property hashing
                         * to defeat HashDoS. Fixed for the VM's lifetime (unlike
                         * rng_state, which Math.random mutates). */
    uint32_t regexp_live; /* live compiled patterns (each ~sizeof(Program)) */
    /* Asynchronous stop request. Written by js_vm_interrupt, which may be
     * called from another thread or a signal handler while this VM is running,
     * and only ever read. It is a one-way request flag, not synchronization:
     * the reader may observe it an instruction or two late, which is all the
     * precision "stop soon" needs. */
    JsInterruptFlag interrupt;

    /* GC */
    JsGcCell *cells;
    size_t cell_count;
    size_t gc_threshold;
    size_t gc_threshold_init;
    bool gc_stress;
    bool gc_running;
    /* Set when the root array could not grow (true allocator failure, not the
     * heap limit — roots are exempt from it). A live value is unrooted
     * somewhere up the C stack, so collecting would free something still in
     * use; the collector stays off for the VM's lifetime. See js_gc_protect. */
    bool gc_suspended;
    JsGcCell **mark_stack;
    size_t mark_len, mark_cap;
    bool mark_overflow;
    JsValue **roots;
    size_t roots_len, roots_cap;
    /* Map and Set iterators not yet done, held weakly: a delete or clear moves their positions
     * (js_iter.h), and the collector drops the unmarked ones before it sweeps */
    struct JsIterObj *live_iters;
    JsModule *mirror_modules;    /* every module with export mirrors (js_module_mirrors_prune) */
    struct JsWeakObj *weak_list; /* every WeakMap, WeakSet, WeakRef, FinalizationRegistry (js_weak.h) */
    bool weak_cleanup_pending;   /* a registry has held values waiting for its callback */

    JsAtomTable atoms;
    JsContext *contexts;
    /* Pre-built "out of memory" error, created while memory was still
     * available. The one error the engine cannot afford to build on demand is
     * the one that says allocation failed: fiber_throw's own string allocation
     * fails too, and the run then rejected with `undefined` — a host could see
     * that something went wrong but not what. */
    JsString *oom_error;
    /* A compile error whose text is built (V8's "Unexpected token ';'"): the message a
     * compile entry point hands back points here, valid until the next compile on this VM. */
    char compile_error[256];

    /* microtask FIFO queue (raw allocations; a GC root) */
    JsJob *jobs_head, *jobs_tail;
    JsJob *jobs_free;    /* recycled job nodes */
    JsJob *running_job;  /* dequeued job currently executing; a GC root */
};

/* ---- value <-> cell ---- */

static inline JsValue js_value_from_cell(JsGcCell *c) {
    JsValue v;
    uint64_t tag;
    switch (c->kind) {
    case JS_KIND_STRING: tag = JS_TAG_STRING; break;
    case JS_KIND_FUNCTION:
    case JS_KIND_CLOSURE:
    case JS_KIND_NATIVE: tag = JS_TAG_FUNCTION; break;
    case JS_KIND_PROMISE: tag = JS_TAG_PROMISE; break;
    case JS_KIND_BIGINT: tag = JS_TAG_BIGINT; break;
    default: tag = JS_TAG_OBJECT; break;
    }
    v.bits = tag | ((uint64_t)(uintptr_t)c & JS_PAYLOAD_MASK);
    return v;
}

static inline JsGcCell *js_value_cell(JsValue v) {
    return (JsGcCell *)(uintptr_t)(v.bits & JS_PAYLOAD_MASK);
}

static inline bool js_value_is_cell(JsValue v) {
    uint64_t tag = v.bits & JS_TAG_MASK;
    return tag == JS_TAG_STRING || tag == JS_TAG_OBJECT || tag == JS_TAG_FUNCTION ||
           tag == JS_TAG_PROMISE || tag == JS_TAG_BIGINT;
}

static inline JsString *js_value_string(JsValue v) { return (JsString *)js_value_cell(v); }
static inline JsObject *js_value_object(JsValue v) { return (JsObject *)js_value_cell(v); }
static inline JsFunctionCell *js_value_function(JsValue v) {
    return (JsFunctionCell *)js_value_cell(v);
}
static inline JsPromise *js_value_promise(JsValue v) { return (JsPromise *)js_value_cell(v); }

/* Largest one-shot growth (by index-assignment gap, or a bare `Array(n)`
 * length/constructor call) a script may force before hitting a RangeError —
 * arrays have no holes here, so every slot up to the requested length is
 * eagerly materialized; this bounds the memory an untrusted script can make
 * one call allocate. Shared by js_interp.c (property-set path) and
 * js_builtins.c (the Array constructor). */
#define JS_MAX_ARRAY_GAP 4096u

/* Longest string, in UTF-16 code units, any operation may produce. Bounds what
 * one call from an untrusted script can be made to allocate AND — just as
 * importantly — how long it can spend filling it, since natives run without
 * consuming fuel. Shared by concat (js_interp.c) and repeat/pad
 * (js_builtins.c) so the ceiling is one number, not several. */
#define JS_MAX_STRING_UNITS (64u * 1024 * 1024)

/* js_object.c: array helpers used by the interpreter. new_cell takes the
 * context (not just the VM) so it can set the new array's [[Prototype]] to
 * ctx->array_proto — real arrays, not the "no [[Prototype]] at all" that a
 * bare vm-only allocator would leave them with. */
JsObject *js_array_new_cell(JsContext *ctx, uint32_t reserve);
bool js_array_append(JsVm *vm, JsObject *arr, JsValue v);
/* A new array of what `for (const x of V)` visits (an array's elements, a string's code points,
 * a Map's entries, a Set's values); NULL with *ERR set (V8's TypeError, or out of memory). */
JsObject *js_iterable_to_array(JsContext *ctx, JsValue v, JsValue *err);
/* split's ToUint32(limit) (undefined: 2^32 - 1); false thrown, the error pending */
bool js_split_limit(JsContext *ctx, JsValue limit, uint32_t *lim);
/* The tag Object.prototype.toString gives V ("Array" for "[object Array]"), as V8's */
const char *js_to_string_tag(const JsContext *ctx, JsValue v);
const char *js_message_tag(const JsContext *ctx, JsValue v);
/* V is the Object.prototype.toString the engine made (whatever a script has put in its place) */
bool js_is_object_to_string(JsValue v);
bool js_array_set_index(JsVm *vm, JsObject *arr, uint32_t idx, JsValue v);

/* ---- allocator (js_vm.c) ---- */

void *js_realloc_raw(JsVm *vm, void *ptr, size_t old_size, size_t new_size);
/* Counted but exempt from heap_limit; only for the collector's own root array
 * and mark stack. See the rationale on the definition in js_vm.c. */
void *js_realloc_internal(JsVm *vm, void *ptr, size_t old_size, size_t new_size);

/* Root slots reserved up front, while memory is still plentiful, so ordinary
 * nesting never has to grow the array mid-operation. */
#define JS_ROOTS_INIT_CAP 64

/* ---- GC (js_gc.c) ---- */

/* Safe point + allocate: may collect before allocating. */
JsGcCell *js_gc_new_cell(JsVm *vm, JsGcKind kind, size_t size);
JsGcCell *js_gc_new_cell_quiet(JsVm *vm, JsGcKind kind, size_t size); /* never collects */
struct JsTrace *js_raise_trace(JsContext *ctx);
void js_gc_maybe(JsVm *vm);
void js_gc_mark_value(JsVm *vm, JsValue v);
void js_gc_mark_cell(JsVm *vm, JsGcCell *c);
void js_gc_free_cell(JsVm *vm, JsGcCell *c, bool remove_atoms);

/* ---- map (js_map.c) ---- */

void    js_map_init(JsMap *m);
bool    js_map_set(JsVm *vm, JsMap *m, JsString *key, JsValue value);
JsValue js_map_get(const JsMap *m, const JsString *key, bool *found);
/* Where KEY (interned) sits in the entries, or JS_MAP_EMPTY: it stays there until the map is
 * rebuilt (a grow, a sort) or the key is deleted */
uint32_t js_map_position(const JsMap *m, const JsString *key);

/* SpeciesConstructor(O, DFLT) for a builtin DFLT: 0 construct the default, 1 *out is the
 * constructor to use (unrooted), -1 thrown with *out the error. O rooted by the caller. */
int js_species(JsContext *ctx, JsValue o, const struct JsNative *dflt, JsValue *out);
int js_array_species(JsContext *ctx, JsValue o, JsValue *out); /* ArraySpeciesCreate's */

/* Watches HOLDER's own property NAME (which it has) as intact-id ID. False on OOM. */
bool js_intact_watch(JsContext *ctx, unsigned id, JsObject *holder, const char *name);
bool js_intact_moved(JsContext *ctx, unsigned id);
/* Whether the watched property still holds what the engine put there */
static inline bool js_intact(JsContext *ctx, unsigned id) {
    const JsIntact *w = &ctx->intact[id];
    const JsMap *m = &w->holder->props;
    if (JS_UNLIKELY(w->pos >= m->used || m->entries[w->pos].key != w->key))
        return js_intact_moved(ctx, id);
    return m->entries[w->pos].value.bits == w->value.bits;
}
/* js_map_get, but *found is false for a JS_ATTR_READONLY entry too */
JsValue js_map_get_writable(const JsMap *m, const JsString *key, bool *found);
bool    js_map_delete(JsMap *m, const JsString *key);
void    js_map_free(JsVm *vm, JsMap *m);
void    js_map_sort_keys(JsMap *m); /* enumeration order; no allocation, so safe anywhere */
/* Adds ATTRS (JS_ATTR_*) to KEY's entry; false when KEY is not in M */
bool    js_map_add_attrs(JsMap *m, const JsString *key, uint8_t attrs);
/* KEY's JS_ATTR_* byte; 0 when KEY is not in M */
uint8_t js_map_get_attrs(const JsMap *m, const JsString *key);
/* Sets KEY's JS_ATTR_* byte to ATTRS; false when KEY is not in M */
bool    js_map_put_attrs(JsMap *m, const JsString *key, uint8_t attrs);

/* ---- normalization (js_normalize.c) ---- */

enum { JS_NF_NFC, JS_NF_NFD, JS_NF_NFKC, JS_NF_NFKD };
/* S in normalization FORM (JS_NF_*): S itself when nothing changes, NULL on OOM */
JsString *js_normalize(JsVm *vm, JsString *s, int form);

/* ---- strings / atoms (js_string.c) ---- */

uint32_t  js_units_hash(const uint16_t *units, size_t len, uint32_t seed);
uint32_t  js_units_hash_from(uint32_t h, const uint16_t *units, size_t len);
JsString *js_string_cell_new(JsVm *vm, const uint16_t *units, size_t len);
JsString *js_atoms_find(JsVm *vm, const uint16_t *units, size_t len, uint32_t hash);
JsString *js_intern_cell(JsVm *vm, JsString *s); /* canonical string, or NULL on OOM */
void      js_atoms_remove(JsVm *vm, JsString *s);
void      js_atoms_free(JsVm *vm);

#endif /* LAMASSU_INTERNAL_H */
