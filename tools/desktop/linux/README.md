# Linux drive badges

The Linux drive mount publishes `user.liquid.state` and `user.liquid.pin`. The integrations in
this directory turn those values into file-manager badges without opening the repository or
contacting the peer. They intentionally contain no synchronization behavior.

| Drive state | Extended attribute | Badge meaning |
|---|---|---|
| Online-only | `online-only` | Cloud content hydrates when read. |
| Available offline | `available-offline` | Content is pinned locally. |
| Not synced here | `not-synced-here` | The path is excluded on this device. |

`nautilus/` is a Python `InfoProvider` loaded by GNOME Files. `dolphin/` installs a session D-Bus
overlay provider and its desktop activation entry. Both installers default to the current user's
XDG data directory and accept `--prefix <path>` for a staged or system package installation.

## Screenshot procedure

1. Build liquid with FUSE and synchronize a drive containing three test files.
2. Mount the repository with `liquid mount <mountpoint>`.
3. Set one state on each path with `setfattr -n user.liquid.state -v <state> <path>`.
4. Install the matching integration and restart the file manager with `nautilus -q` or
   `kquitapp6 dolphin`.
5. Open the mountpoint in icon view, place all three files in one row, and include the location bar
   in the capture so the mounted path is visible.
6. Save the image outside the source tree. Verify the online-only cloud, available-offline check,
   and excluded warning badges are distinct before attaching it to a release or pull request.

The excluded file disappears after the kernel invalidates the entry. Capture its warning badge
immediately after setting the attribute, or use a directory rule and show its parent while
validating the exclusion behavior separately with `getfattr`.
