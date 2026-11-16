// A module that waits for itself never finishes: exit status 13, as Node has it.
export {};
print('body');
await import('./main.mjs');
print('never');
