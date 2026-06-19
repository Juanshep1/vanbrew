# Vanta parity test suite

A battery of `.va` programs used to measure how close the native toolchain
(`vself` interpreter + `vc` compiler) is to the reference Python interpreter.

Run from a built `vanta/` dev tree with `bash run.sh`: each test runs through
all three backends (python `vanta`, native `vself`, native `vc`) and the runner
reports where they disagree — a measurable parity score.

Current: ~10/27 full parity. The remaining gaps are language features the native
interpreter predates — anonymous functions (`make x give x*2`), inline
conditionals (`A if C otherwise B`), higher-order `map`/`reduce`/`keep`, `import`,
and floating-point — plus a long tail of stdlib builtins.
