// A thrown value that is no error has no stack to say where it was made: it is reported where
// it was thrown, through the await it came out of. (Node names no place for it at all.)
function check(n) {
    if (n > 1) throw "too many";
}
async function later() { await 0; check(2); }
await later();
