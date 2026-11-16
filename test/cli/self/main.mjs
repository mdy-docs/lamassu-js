// The entry imports itself: it is one module, and runs once.
import * as ns from './main.mjs';
import { k as again } from './../self/main.mjs';
export const k = 1;
print('body', typeof ns, Object.keys(ns).join());
export function check() { return ns.k === k && again === k; }
print('one module', check());
