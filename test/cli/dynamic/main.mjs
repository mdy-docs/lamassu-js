// import() of the entry, from the entry and from a module it imports: the module that is
// running, not a second one.
import { again } from './other.mjs';
export const v = 5;
globalThis.runs = (globalThis.runs ?? 0) + 1;
print('body');
again().then((ns) =>
  import('./main.mjs').then((m) => print('imported', globalThis.runs, m.v, m === ns)));
