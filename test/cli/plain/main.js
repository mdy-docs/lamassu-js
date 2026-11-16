// No import or export: run as it is, and the value it completes with is printed. Its import()
// is beside it.
const m = await import('./lib/m.js');
print('main', m.v);
1 + 2;
