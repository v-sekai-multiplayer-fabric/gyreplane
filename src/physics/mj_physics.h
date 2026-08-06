#pragma once

#include <mujoco/mujoco.h>

/*
 * MuJoCo-backed entity/prop contact physics (task #8) -- collisions,
 * joints, forces between bodies in a zone. NOT the same job as
 * sinew-mocap/solve (avatar IK/posing, ported separately): checked
 * MuJoCo's public C API (mujoco.h) directly and confirmed there is no
 * first-party IK solver (no mj_ik or equivalent) -- only Jacobian
 * primitives (mj_jac, mj_jacBody, mj_jacSite, ...) that a caller would
 * build an IK solver on top of. Reusing sinew-mocap/solve (already
 * built, already matched to the org's Sinew mocap hardware pipeline)
 * instead of reinventing IK on raw Jacobians is the point.
 *
 * API grounded against the exact vendored commit (thirdparty/mujoco,
 * pinned to release 3.11.0): mj_defaultVFS, mj_addBufferVFS,
 * mj_loadXML, mj_makeData, mj_step, mj_deleteData, mj_deleteModel,
 * mj_deleteVFS all confirmed present in include/mujoco/mujoco.h at that
 * commit, and mj_physics.c syntax-checks clean against the real
 * vendored headers (clang -fsyntax-only, no h2o/FDB-style missing-header
 * cascade -- this one really does compile against what it includes).
 * mjtNum is `double` by default (mjtype.h's own typedef, confirmed by
 * reading it directly, not assumed from documentation) unless MuJoCo is
 * built with its single-precision flag, which this repo does not set.
 *
 * STATUS: this loads an in-memory MJCF scene and steps it -- it does
 * NOT yet feed FDB-backed entity state in or physics results back out
 * (that wiring, and a real per-entity MJCF scene generator instead of
 * the fixed single-body test scene below, is follow-up work).
 */

typedef struct {
    mjModel *model;
    mjData *data;
} mj_physics_t;

/* Loads `mjcf_xml` (an in-memory MJCF scene, via mj_addBufferVFS -- no
 * disk file needed) and creates its mjData. Returns 0 on success,
 * writes a human-readable reason into error[error_len] on failure
 * (mj_loadXML's own error-reporting convention). */
int mj_physics_init(mj_physics_t *phys, const char *mjcf_xml, char *error, int error_len);

/* Advances the simulation by one MuJoCo timestep (mj_step). */
void mj_physics_step(mj_physics_t *phys);

void mj_physics_close(mj_physics_t *phys);
