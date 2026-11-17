// A promise rejected with nothing handling it ends the run with an error, once the run is over.
// The first one is reported; one handled later, however late in the run, is no error.
const late = Promise.reject(new Error("late"));
Promise.resolve().then(() => 0).then(() => late.catch((e) => print("caught", e.message)));
async function fails() { await 0; throw new RangeError("nobody"); }
fails();
Promise.reject(new Error("after")).then(() => print("not reached"));
print("done");
