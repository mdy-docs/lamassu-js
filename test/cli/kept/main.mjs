// A thrown value that is no error keeps the place it was thrown at for as long as its rejection
// goes on: through a finally() and a then() that let it pass, and whatever other functions
// throw (and catch) before the run is over.
function check(n) {
    if (n > 1) throw "too many";
}
async function later() { await 0; check(2); }
async function noise() {
    for (let i = 0; i < 4; i++) {
        await 0;
        try { null.x; } catch {}
    }
}
noise();
const waiting = later().finally(() => noise()).then(() => 1);
await 0; await 0; await 0; await 0; await 0; await 0;
try { undefined.y; } catch {}
print('before');
await waiting;
