# norms dry-run fixtures

Stand-in content for `exocrawl/contrib/fetch-norms.sh --dry-run <dir>`.
Each `<id>.txt` mimics the extracted-text shape the real source yields
through exocrawl's `/fetch` redirector (URLs live in the script's
URLS array; headers `# norm:`, `# source:`, `# harvested:` are written
by the script itself). The dry-run path is byte-identical to a real
run except the fetch step: same cap (MAX_NORM, default 30000), same
exomind writes, same registry maintenance — so these fixtures prove
the mechanics offline. `ecma262.txt` is deliberately oversized to
exercise the cap.