// A file with no import or export that imports itself: it is the module that is running, and
// runs once.
globalThis.runs = (globalThis.runs ?? 0) + 1;
print('body');
import('./main.mjs').then((ns) => print('imported', globalThis.runs, Object.keys(ns).length));
