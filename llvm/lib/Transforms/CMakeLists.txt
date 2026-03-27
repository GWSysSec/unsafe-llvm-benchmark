add_subdirectory(Utils)
add_subdirectory(Instrumentation)
add_subdirectory(AggressiveInstCombine)
add_subdirectory(InstCombine)
add_subdirectory(Scalar)
add_subdirectory(IPO)
add_subdirectory(Vectorize)
add_subdirectory(Hello)
add_subdirectory(ObjCARC)
add_subdirectory(Coroutines)
add_subdirectory(CFGuard)
add_subdirectory(HipStdPar)
# UNSAFE-RUST BEGIN
# add_subdirectory(DebugInfoPreserve)
add_subdirectory(InstMarker)
add_subdirectory(DynamicAnalysis)
add_subdirectory(UnsafeRustDummy)
# UNSAFE-RUST END
# UNSAFE-SVF BEGIN
add_subdirectory(SVFAnalysis)
# UNSAFE-SVF END
