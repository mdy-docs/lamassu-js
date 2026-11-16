// A cycle through the entry: b.mjs imports back from the module that imported it.
import { b, viaB } from './b.mjs';
export const a = 'a';
print('main body', b, viaB());
