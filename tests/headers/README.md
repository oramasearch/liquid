# Public-header compilation tests

Each translation unit includes one installed C or C++ header without relying on the umbrella
header. `main.cpp` supplies the executable entry point; the other files instantiate representative
public values. `abi.cpp` covers the linked-binary descriptor and compatibility report types.
`admission.cpp` keeps the public policy, evidence interfaces, options, and server entry point
independently compilable. `configuration.cpp` keeps the configuration AST, lowered records, and
task entry points independently compilable.
`import.cpp` keeps the Spack report values, options, report codecs, and import task independently
compilable.
`frontend_abi.c` compiles the frontend and liquify plugin descriptors as C11 and checks both
independent ABI version constants.
`session.cpp` compiles the speculative-session and collaboration API independently.
`projection.cpp` covers the Tier A package, OCI image, and Helm request/report API.
