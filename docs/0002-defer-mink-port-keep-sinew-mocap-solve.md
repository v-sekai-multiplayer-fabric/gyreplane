# Decision: defer porting kevinzakka/mink, keep sinew-mocap/solve for avatar IK

**Status:** superseded -- see "Revision" section at the end. The analysis
of what `mink` actually is (below) still holds; only the decision
changed.

## Context

`sinew-mocap/solve` (FK + LBS skinning, ported for task #8 per the
consolidation plan) was flagged as less trusted than
[`kevinzakka/mink`](https://github.com/kevinzakka/mink), a widely-used
differential inverse-kinematics library built on MuJoCo (which
task #8 already vendors for entity/prop contact physics, `thirdparty/mujoco`
pinned to 3.11.0).

## What mink actually is

Read `mink`'s core (`src/mink/solve_ik.py`) directly rather than assume
from its README. It is not a simple Jacobian pseudo-inverse solver. Each
solve builds a full quadratic program:

- Objective `(H, c)` assembled from weighted per-task residuals plus
  Levenberg-Marquardt damping (`_compute_qp_objective`).
- Inequality constraints `(G, h)` from joint/velocity/collision-avoidance
  limits (`_compute_qp_inequalities`).
- Equality constraints `(A, b)` from closed-chain kinematics
  (`_compute_qp_equalities`).

That QP is solved via `qpsolvers`, a separate general-purpose QP
dispatch library backing onto solvers such as OSQP, quadprog, or proxqp
— not something `mink` implements itself. `mink` also ships its own
C-optimized Lie group (SE3/SO3) module and roughly a dozen pluggable
task/limit types (frame, COM, posture, look-at, collision-avoidance,
velocity/configuration limits).

## Decision: defer, do not port now

A faithful port needs a vendored C QP solver (OSQP is the natural
pick — pure C, license-compatible, already a common `qpsolvers` backend)
plus `mink`'s QP-assembly loop, Lie group math, and at least the
`FrameTask` case. That is a genuinely large, multi-session effort, not
something to complete alongside the rest of this pass's work.
`sinew-mocap/solve` stays the avatar IK/posing approach for now — it is
already built and already matched to the org's actual mocap hardware
pipeline (the Sinew rebocap dongle).

## Revisit when

There is dedicated budget for a careful, verified port: vendor OSQP (or
confirm `qpsolvers`' actual default/preferred backend first), port
`mink`'s QP assembly and Lie group math, and validate against `mink`'s
own test suite or a differential test against its Python output the same
way `xr_grid_entity_packet.c` was validated against `lean-entity-packet`'s
golden vectors. Track this as its own scoped task when that budget
exists, not folded into task #8's current entity-physics scope.

## Revision 1 (superseded by Revision 2)

Per direct instruction, `sinew-mocap/solve`'s avatar-posing role is now
built on a QP-based IK core mirroring `mink`'s architecture (task #16,
revised), rather than staying on plain FK + LBS with `mink` deferred
wholesale.

## Revision 2 (current)

Revision 1 was wrong, caught by a direct correction: `sinew-mocap/solve`
follows this org's Lean-first convention (Lean spec -> Slang codegen,
same as `lean-entity-packet` and `lean-rebac-core`'s Lean -> C pattern),
and its own Lean source (`core/spec/Sinew/Align.lean`, read directly, not
assumed) is **not** a QP-based solver at all. It is Kabsch-style rotation
fitting -- `rodrigues` for one vector pair, a Newton-Schulz-orthogonalized
covariance (`ns30`) for two or more, falling back to a Jacobi-SVD Kabsch
solve (`kabsch`) when the fast path produces an invalid rotation. No QP,
no `qpsolvers`, no MuJoCo dependency for this part at all.

Task #16 now ports `Align.lean` directly (`src/gen/sinew_align.{c,h}`),
verified against the exact same known-rotation-recovery test
`core/spec/AlignTest.lean` itself uses (quaternion (0.5,0.5,0.5,0.5),
the same five source vectors, the same N=5/N=2/N=1 cases) --
`test/unit/test_sinew_align.c` reproduces it and recovers the rotation
to floating-point precision at all three. This is **done**, not
deferred -- much smaller and more tractable than either Revision 1's QP
framing or the original mink-wholesale framing, because it is the
algorithm the org actually already trusts and has proven, not a
heavier one substituted in from outside.

Task #17 (`mink` feature parity: limits, closed-chain constraints, the
Lie group module, multi-task weighting) stays deferred and unrelated to
task #16's now-completed scope -- it would only become relevant if a
genuinely different, `mink`-shaped IK need arises later, not as a
continuation of `Align.lean`'s already-sufficient algorithm.

## Revision 3 (current)

`thirdparty/mujoco` (Context's own line 13 reference, task #8's
entity/prop contact physics) is dropped, per direct instruction --
Godot's own Jolt physics already covers that role, so a second vendored
physics engine in this process was redundant. Nothing in this doc's
own analysis changes: `sinew-mocap/solve`/`Align.lean` never depended
on MuJoCo, as Revision 2 already established. Task #8 itself needs a
real replacement plan for entity/prop contact physics against Jolt,
tracked separately, not assumed solved by this doc.
