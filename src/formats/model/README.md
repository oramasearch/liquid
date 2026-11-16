# Model and notebook readers

Jupyter notebooks expose cell sources without outputs or execution counts. Safetensors exposes
JSON-declared tensor content and ranges. Pickle scanning is non-executing and reports `GLOBAL`,
`INST`, and `STACK_GLOBAL` references plus construction-risk opcodes; it never deserialises
checkpoint data.

Safetensors charges every tensor range to one cumulative decoded-byte budget before copying,
including repeated and partially overlapping ranges. It stops before an entry would exceed that
budget or the entry limit and returns a truncated tree with the reason recorded (liquid #476).
