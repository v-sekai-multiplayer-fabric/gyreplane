# MuJoCo 3.11.0, vendored (task #8): entity/prop contact physics
# (collisions, joints, forces). NOT the same job as sinew-mocap/solve
# (avatar IK/posing) -- see src/physics/mj_physics.h's header comment
# for why both exist, and docs/0002-mujoco-vs-sinew-mocap-solve.md.
#
# Disables MuJoCo's own tests/samples/simulate-GUI/Python bindings --
# zone-server-h2o only needs the `mujoco` core library target this
# upstream CMakeLists.txt already produces (add_library(mujoco ...)).
set(MUJOCO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MUJOCO_TEST_PYTHON_UTIL OFF CACHE BOOL "" FORCE)
set(MUJOCO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MUJOCO_BUILD_SIMULATE OFF CACHE BOOL "" FORCE)
set(MUJOCO_ENABLE_PLUGINS OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_SOURCE_DIR}/thirdparty/mujoco ${CMAKE_BINARY_DIR}/mujoco-build)
