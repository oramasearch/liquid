# Public-header compilation tests

Each translation unit includes one installed C or C++ header without relying on the umbrella
header. `main.cpp` supplies the executable entry point; the other files instantiate representative
public values. `abi.cpp` covers the linked-binary descriptor and compatibility report types.
`admission.cpp` keeps the public policy, evidence interfaces, options, and server entry point
independently compilable.
