// No import or export: a module all the same. Its import() is beside it, and the value it ends
// with is nobody's to print.
const m = await import('./lib/m.js');
print('main', m.v);
1 + 2;
