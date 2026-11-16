// Specifiers that name no file are refused, each as a rejected import(); a roundabout one that
// names a file finds it.
export {};
const tries = {
  empty: '',
  directory: './',
  long: './' + 'a/'.repeat(3000) + 'x.mjs',
  nul: './sub/ok\0.mjs',
  half: './sub/ok\ud800.mjs',
  above: '/../../no-such-lamassu-file.mjs',
  roundabout: './sub/../.././edges//sub/./ok.mjs',
};
for (const name of Object.keys(tries))
  await import(tries[name]).then((m) => print(name, m.ok), () => print(name, 'refused'));
