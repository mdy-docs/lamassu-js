import { z } from '../../z.mjs';
print('d body');
export const d = 'd' + z;
const up = await import('../y.mjs');
print('d sees', up.y.name);
