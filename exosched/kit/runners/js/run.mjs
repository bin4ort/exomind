// JS runner shim: read fn<TAB>args lines, print results.
import { readFileSync } from 'fs';
for (const line of readFileSync(0, 'utf8').split('\n')) {
  const i = line.indexOf('\t');
  if (i < 0) continue;
  const fn = line.slice(0, i), args = line.slice(i + 1);
  // dispatch here: if (fn === 'add') ... console.log(a+b)
  console.log('error: no such fn ' + fn);
}
