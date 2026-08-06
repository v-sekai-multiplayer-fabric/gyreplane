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

## Revision

Per direct instruction, `sinew-mocap/solve`'s avatar-posing role is now
built on a QP-based IK core mirroring `mink`'s architecture (task #16,
revised), rather than staying on plain FK + LBS with `mink` deferred
wholesale. The scope split:

- **Task #16** ports the QP-assembly core `mink`'s `solve_ik.py`
  implements (task residuals + Levenberg-Marquardt damping as the QP
  objective, `mj_jac*` Jacobians from the already-vendored MuJoCo),
  applied specifically to `sinew-mocap/solve`'s avatar-posing use case.
  Still needs a vendored C QP solver (OSQP).
- **Task #17** stays deferred: the rest of `mink`'s surface (joint/
  velocity/collision-avoidance limits as QP inequalities, closed-chain
  equality constraints, the full Lie group module, multi-task weighting
  beyond one avatar-posing task) that task #16's narrower scope doesn't
  need. May turn out unnecessary once #16 is built and proven.

The "large, multi-session effort" sizing above still applies to task
#16's revised scope -- it does not become small because it is scoped
narrower, only more clearly bounded.
