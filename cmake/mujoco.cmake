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

# MuJoCo's own cmake/MujocoOptions.cmake sets -Werror unconditionally for
# GCC/Clang. The first real CI build (task #17) hit this: combined with
# our own -Wextra leaking in (fixed in the top-level CMakeLists.txt,
# include/compile-options are now target-scoped, not global), mujoco's
# own pre-existing warnings (unused-parameter, old-style-declaration,
# type-limits, all in mujoco's own upstream source, not ours) became
# fatal errors. -Wno-error here is defensive on top of that fix -- we
# do not want a third-party vendor's own strictness blocking our build
# over warnings in code we do not own and should not be patching.
target_compile_options(mujoco PRIVATE -Wno-error)
