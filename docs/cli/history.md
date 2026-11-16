# `liquid history`

`liquid history <path>` lists snapshots for the path's stable node identity, newest first. Each
line contains its change digest, timestamp, actor, `version` or `deleted`, historical path, and
change message. A rename therefore continues the earlier path's history. The actor is the change's
author only when an identity chain verifies it; otherwise it is `signed by an unverified key`
(liquid#508).

`liquid history <path> --restore <change>` reads the value from that change and writes it at the
requested current path as a new drip. It does not change a ref or rewrite an earlier change. Cut
the restored working value normally after inspection or further edits.

See [drive synchronization](../library/drive.md#history-and-restore) for the library contract.
