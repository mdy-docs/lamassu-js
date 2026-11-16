// An error nothing catches is reported at the place it was thrown, in the module that threw.
import { f } from './lib/thrower.mjs';
print('before');
f();
print('never');
