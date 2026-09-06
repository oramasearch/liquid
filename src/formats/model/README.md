# Model and notebook readers

Jupyter notebooks expose cell sources without outputs or execution counts. Safetensors exposes
JSON-declared tensor content and ranges. Pickle scanning is non-executing and reports `GLOBAL`,
`INST`, and `STACK_GLOBAL` references plus construction-risk opcodes; it never deserialises
checkpoint data.
