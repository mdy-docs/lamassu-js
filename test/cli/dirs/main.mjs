// "./" and "../" are beside the module that imports, wherever the entry is; a file reached by
// two spellings is one module.
import { x } from './sub/x.mjs';
import { y } from './sub/../sub/y.mjs';
import { y as y2 } from './sub/y.mjs';
print('main', x, y === y2);
const d = await import('./sub/deep/d.mjs');
print('d', d.d);
