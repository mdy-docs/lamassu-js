// lamassu's own: a name without an extension is a .js file, and a bare name is beside the entry.
import { n } from './util';
import { n as n2 } from './util.js';
import { n as n3 } from 'util.js';
import { n as n4 } from './lib/m';
print('main', n === n2, n === n3, n === n4);
