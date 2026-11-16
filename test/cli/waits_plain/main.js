// The same for a file run as it is: it has no value to print, it never got to one.
print('body');
await new Promise(() => {});
print('never');
