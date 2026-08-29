# Python runner shim: read fn<TAB>args lines, print results.
import sys
def no_such(fn):
    print(f'error: no such fn {fn}')
for line in sys.stdin:
    line = line.rstrip('\n')
    i = line.find('\t')
    if i < 0:
        continue
    fn, args = line[:i], line[i + 1:]
    # dispatch here: if fn == 'add': print(int(a) + int(b))
    no_such(fn)
