/*
 * MuJoCo-backed physics wiring. See mj_physics.h for scope and
 * provenance -- every mj_* call here is checked against
 * thirdparty/mujoco's include/mujoco/mujoco.h at the vendored commit
 * (release 3.11.0), not guessed.
 */

#include "mj_physics.h"

#include <stdio.h>
#include <string.h>

int mj_physics_init(mj_physics_t *phys, const char *mjcf_xml, char *error, int error_len)
{
    memset(phys, 0, sizeof(*phys));

    mjVFS vfs;
    mj_defaultVFS(&vfs);

    int rc = mj_addBufferVFS(&vfs, "zone_scene.xml", mjcf_xml, (int)strlen(mjcf_xml));
    if (rc != 0) {
        if (error != NULL && error_len > 0) {
            snprintf(error, (size_t)error_len, "mj_addBufferVFS failed: %d", rc);
        }
        mj_deleteVFS(&vfs);
        return -1;
    }

    phys->model = mj_loadXML("zone_scene.xml", &vfs, error, error_len);
    mj_deleteVFS(&vfs); /* mj_loadXML copies what it needs; the VFS itself is not retained */

    if (phys->model == NULL) {
        return -1; /* error[] already holds mj_loadXML's message */
    }

    phys->data = mj_makeData(phys->model);
    if (phys->data == NULL) {
        if (error != NULL && error_len > 0) {
            snprintf(error, (size_t)error_len, "mj_makeData failed (out of memory)");
        }
        mj_deleteModel(phys->model);
        phys->model = NULL;
        return -1;
    }

    return 0;
}

void mj_physics_step(mj_physics_t *phys)
{
    mj_step(phys->model, phys->data);
}

void mj_physics_close(mj_physics_t *phys)
{
    if (phys->data != NULL) {
        mj_deleteData(phys->data);
        phys->data = NULL;
    }
    if (phys->model != NULL) {
        mj_deleteModel(phys->model);
        phys->model = NULL;
    }
}
