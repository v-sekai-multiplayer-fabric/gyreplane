# Guest differential test: real result, not assumed

Real run, both under real interpreters, same seed (7), same objective
(`gain_watch_trust`), same five-command sequence
(look, go north, go north, talk captain "thank you for your service",
talk captain "can you recommend me to the watch?"):

1. `Source_Code/state_machine.py` (CrucibleBench_Phase1's own Python
   original, `classifier=None` so it uses `_fallback_classification()`
   directly, no network/LLM call), run under CPython.
2. `mud_guest.cpp`, cross-compiled for riscv64-musl, run under real
   `qemu-riscv64` (user-mode emulation, not `rvlinux`/`libriscv` --
   that combination is separately verified in
   `mud/orchestrator/README.md`).

Result: byte-identical narration and room-transition output at all
five turns, including the trust-tier-gated captain dialogue branches
at turns 4 and 5 (the exact wording only appears when trust crosses
specific thresholds -- an exact match there means the ChaCha20 PRNG's
NPC-trust jitter (mud_guest.cpp's own file-header-documented deviation
from CPython's Mersenne Twister) landed in the same trust bucket for
this seed, not just that the deterministic non-RNG logic matches).

This is one seed, one command sequence -- a real spot-check confirming
the port is correct for this case, not an exhaustive equivalence
proof. A future pass could fuzz more seed/command combinations; not
done here.
