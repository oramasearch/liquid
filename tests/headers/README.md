# Public-header compilation tests

Each translation unit includes one installed C or C++ header without relying on the umbrella
header. `main.cpp` supplies the executable entry point; the other files instantiate representative
public values. `abi.cpp` covers the linked-binary descriptor and compatibility report types.
`admission.cpp` keeps the public policy, evidence interfaces, options, and server entry point
independently compilable. `configuration.cpp` keeps the configuration AST, lowered records, and
task entry points independently compilable.
independently compilable.
`frontend_abi.c` compiles the frontend and liquify plugin descriptors as C11 and checks both
independent ABI version constants. It also instantiates the optional plugin event table so its C11
layout and version remain covered.
`session.cpp` compiles the speculative-session and collaboration API independently.
`projection.cpp` covers the Tier A package, OCI image, and Helm request/report API.
`tiers.cpp` compiles and links the synchronous registry-derived support matrix as an isolated
public include.
