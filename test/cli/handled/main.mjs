// Rejections that are all handled, some of them late: nothing to report. An import that fails
// and is caught leaves nothing behind either, the first time or the second.
const late = Promise.reject(new Error("late"));
Promise.resolve().then(() => 0).then(() => late.catch((e) => print("caught", e.message)));
async function fails() { await 0; throw new RangeError("mine"); }
const going = fails();
for (let i = 0; i < 2; i++) {
    try {
        await import("./lib/thrower.mjs");
    } catch (e) {
        print("import", e.message);
    }
}
try {
    await import("./lib/nowhere.mjs");
} catch (e) {
    print("no module");
}
Promise.all([Promise.reject(1), going]).catch((e) => print("all", e));
await going.catch((e) => print("caught", e.message));
print("done");
