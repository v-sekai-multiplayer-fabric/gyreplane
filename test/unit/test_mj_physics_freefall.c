/*
 * Free-fall correctness check for the MuJoCo integration (task #8) --
 * a single body under gravity only, checked against the closed-form
 * projectile motion equation z(t) = z0 - 0.5*g*t^2, the same kind of
 * "known-good numeric answer" check as test_xr_grid_entity_packet.c's
 * golden vectors, just derived from physics instead of a Lean proof.
 *
 * STATUS: this file syntax-checks clean against the vendored MuJoCo
 * headers (see mj_physics.h's header comment) but has NOT been built
 * and run -- linking MuJoCo's full C++ core is a substantial build this
 * pass did not attempt to complete in-sandbox. Unlike every other test
 * in test/unit/, which was actually executed with a real pass/fail
 * result, this one is grounded-but-unverified. Run it for real (`cmake
 * --build build && ctest` or a direct clang/g++ link against the built
 * `mujoco` target) before trusting it the way the others have been
 * trusted this session.
 */

#include "mj_physics.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Minimal MJCF scene: one free body, no joints, no other geometry --
 * gravity is MuJoCo's own default (0, 0, -9.81), not set explicitly, so
 * this also implicitly checks that default is what's documented. */
static const char *FREEFALL_MJCF =
    "<mujoco>"
    "  <option timestep=\"0.001\"/>"
    "  <worldbody>"
    "    <body pos=\"0 0 10\">"
    "      <freejoint/>"
    "      <geom type=\"sphere\" size=\"0.1\" mass=\"1\"/>"
    "    </body>"
    "  </worldbody>"
    "</mujoco>";

int main(void)
{
    mj_physics_t phys;
    char error[512] = {0};

    if (mj_physics_init(&phys, FREEFALL_MJCF, error, sizeof(error)) != 0) {
        fprintf(stderr, "mj_physics_init failed: %s\n", error);
        return 1;
    }

    const double z0 = 10.0;
    const double g = 9.81; /* MuJoCo's documented default gravity magnitude */
    const double dt = 0.001;
    const int steps = 500; /* 0.5 simulated seconds */

    for (int i = 0; i < steps; i++) {
        mj_physics_step(&phys);
    }

    double t = dt * steps;
    double z_expected = z0 - 0.5 * g * t * t;
    double z_actual = phys.data->qpos[2]; /* free joint: qpos[0..2] = position, [3..6] = quat */

    double err = fabs(z_actual - z_expected);
    /* Loose tolerance: MuJoCo's integrator is not exact forward-Euler
     * free-fall, and contact/constraint solving overhead exists even
     * with nothing to contact -- this checks "closed enough to be
     * gravity, not zero-g or some other constant," not bit-exactness. */
    if (err > 0.05) {
        fprintf(stderr, "free-fall mismatch: expected z~=%.4f, got z=%.4f (err=%.4f)\n",
                z_expected, z_actual, err);
        mj_physics_close(&phys);
        return 1;
    }

    printf("mj_physics free-fall: after %d steps (%.2fs), z=%.4f (expected ~%.4f, err=%.4f) -- pass\n",
           steps, t, z_actual, z_expected, err);

    mj_physics_close(&phys);
    return 0;
}
