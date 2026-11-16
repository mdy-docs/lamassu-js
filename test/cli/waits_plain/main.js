// The same for a file with no import or export.
print('body');
await new Promise(() => {});
print('never');
