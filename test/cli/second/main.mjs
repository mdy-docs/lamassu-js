// An error is reported at its own place, whatever is thrown after it: a function this module
// started fails a job later, and that is not where the module failed.
async function later() { await 0; throw new Error("second"); }
later().catch((e) => print("caught", e.message));
function make() { return new Error("first"); }
const e = make();
print("thrown");
throw e;
