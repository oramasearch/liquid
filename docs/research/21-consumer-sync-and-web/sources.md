<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Consumer file sync and collaboration systems, 2024-2026

**Scope note:** Several foundational designs - Dropbox's Nucleus rewrite, Project Infinite, block architecture, and Google's client consolidation - were published before 2024, but they remain the architectural basis needed to understand the products as they operate in 2026. Product/API status below is current to **September 2026**.

## 1. Dropbox architecture

| Component or feature | Technical behavior and significance |
|---|---|
| **Nucleus sync engine** | Dropbox replaced its 2008-era "Sync Engine Classic" with **Nucleus**, rolled out to all users in 2020 and written principally in **Rust**. Most state-machine logic runs deterministically on one "control thread," using futures for concurrency; network I/O, hashing and filesystem I/O are delegated to specialized threads or pools. Rust's type system is used to encode invariants and eliminate invalid states. Nucleus also introduced a strongly consistent client-server protocol and stable global IDs for files and shared folders. [web:91] |
| **Three-tree reconciliation model** | Instead of persisting a queue of intended operations, Nucleus maintains a **Remote Tree** (cloud state), a **Local Tree** (observed disk state), and a **Synced Tree** (last mutually synchronized state). The Synced Tree is effectively a per-node merge base: comparing all three trees reveals whether a path changed locally, remotely, on both sides, or merely moved. [web:92] |
| **4 MiB block-level storage** | Files are divided into fixed, aligned blocks of up to **4 MiB**; the last block may be smaller. Every block is SHA-256 hashed, and a file version is represented by its ordered block-hash list or **blocklist**. Only blocks absent from the destination need transfer, so modifying a small region of a large file normally transfers only the affected aligned blocks - Dropbox calls this **delta sync**. [web:1][web:2][web:294] |
| **Content addressing** | A block is retrieved by hash rather than pathname. Hash verification detects corruption, lets unchanged blocks be reused between versions, and separates immutable byte storage from filesystem semantics. Authorization still includes a namespace: LAN requests use a `(namespace, block hash)` pair rather than unrestricted retrieval by hash. [web:1][web:3] |
| **Metadata server vs block server** | The **metadata service** stores users, namespaces, paths, revisions, blocklists, and a monotonically increasing journal ID per namespace. The **block service** is a key-value service from hash to encrypted block that deliberately knows nothing about filenames, users, or how blocks form files. A metadata commit makes a blocklist into a visible file version; uploading blocks alone does not. [web:2] |
| **Partial-upload safety** | Blocks are uploaded first, then a final commit containing the block list. The upload-session design separates appending bytes from the namespace commit, allowing parallel or resumable transfer without exposing a half-uploaded file. The final commit takes the namespace lock and publishes atomically. [web:15][web:320] |
| **Magic Pocket** | Dropbox's published storage system (not "Magic Pool"). It stores opaque, compressed and encrypted blocks - normally keyed by SHA-256 hash - in storage buckets. [web:12] |
| **Streaming File Sync** | Conventional sync waits for all blocks to reach Dropbox before another client downloads. Streaming Sync pipelines the transfer: as blocks arrive they become available to the receiving client, approaching a 2x improvement when upload and download bandwidth are similar. Metadata ordering remains server-controlled. [web:2][web:289] |
| **LAN sync** | Clients discover peers with UDP broadcast and transfer authorized blocks directly over HTTPS, falling back to the block service. Only content blocks travel peer-to-peer; paths, folder structure and permissions still come from Dropbox servers, preserving a single authoritative namespace order. **Not supported on macOS under File Provider.** [web:1][web:4][web:7] |
| **Server-ordered journal** | Each namespace's Server File Journal uses monotonically increasing journal IDs, giving clients a total server order for accepted metadata revisions. Nucleus checks that client and server agree on remote state before applying a mutation; for irreconcilable concurrent operations, server acceptance order determines the result rather than attempting a semantic merge. [web:2][web:91] |

### Project Infinite, Smart Sync and selective sync

| Feature | What the user sees | Architectural meaning in 2026 |
|---|---|---|
| **Project Infinite** | An entire namespace browsable even when it exceeds local disk. [web:24] | Its first macOS design used a kernel extension on `KAUTH_SCOPE_VNODE` to intercept placeholder access, delay the operation, fetch content in user space, and let access retry. [web:284] |
| **Smart Sync / online-only** | Files remain visible with real names and metadata but consume little local space. Opening one downloads it; users can mark it available offline or return it to online-only. [web:18][web:26] | Now exposed as **online-only / available offline**. Windows uses Cloud Files placeholders; macOS uses Apple File Provider rather than the old kernel extension. |
| **macOS File Provider migration** | Dropbox appears under Finder **Locations**, normally at `~/Library/CloudStorage`; the OS participates in materialization, eviction, pinning and presentation. [web:271][web:282] | Better security and OS consistency, but constrains sync-root placement; LAN sync unavailable and external-drive support limited. [web:198][web:271][web:273] |
| **Selective sync** | An unchecked folder disappears entirely from that computer's tree, remaining in the cloud and on other devices. | A **namespace-scope choice**, usually folder-granular: the client does not instantiate those paths locally. |
| **Online-only vs selective sync** | Online-only items stay visible and hydrate on open; selectively excluded folders are not visible locally. Individual files can be online-only; selective sync removes whole folders. [web:16][web:25] | Conflating these is a persistent usability problem: one controls local bytes, the other controls whether part of the namespace exists locally at all. |

### Hardest Dropbox engineering problems

| Problem | Why it is difficult, and the approach taken |
|---|---|
| **Why rewrite the engine?** | Classic's data model predated large shared workspaces, lacked stable identities across moves, represented a move as separate delete/add operations, had weak consistency guarantees, and could not scale to millions of files. Coarse threading made failures nondeterministic and testing hard. Nucleus prioritized strong invariants, deterministic simulation, stable IDs, atomic moves, testability and debuggability - not just transfer speed. [web:91][web:92] |
| **Conflicting operations** | Offline users can produce combinations each valid alone but invalid together - e.g. concurrent moves creating a directory cycle. Nucleus serializes accepted remote mutations and preserves a valid tree rather than blindly replaying both. For simultaneous content edits, both byte streams are preserved because arbitrary binary files have no safe generic merge. |
| **Filename and Unicode normalization** | Windows, macOS, Linux, Android and their filesystems disagree on case, trailing whitespace, normalization forms and lowercase mappings. Dropbox normalized paths into an OS-independent key; it published an Android bug where Java and the server's older Python lowercase behavior produced different normalized paths. [web:103] |
| **Case sensitivity** | A case-sensitive filesystem may permit `Records` and `records` in one directory where a case-insensitive target cannot. The engine must reject, rename or flag one rather than silently overwrite. [web:241] |
| **Path-length limits** | Dropbox documents a path limit below **260 characters**, with UTF-16 details such as some emoji counting as two characters. [web:303] |
| **Atomicity and crash consistency** | A file must not become visible with only some blocks downloaded, and a move must not appear as a deletion if the addition fails. Nucleus supports atomic logical moves independent of subtree size; downloads are assembled into a temporary sibling, verified, flushed, and renamed over the destination. Uploads become visible only at metadata commit. [web:91][web:15][web:320] |
| **Open or locked files** | An application may hold a file open, deny sharing, keep writing after the watcher event, or hold the destination against replacement. The client must retry with backoff and never interpret "temporarily inaccessible" as deletion. [web:304] |
| **State-space explosion** | Local edits, remote edits, offline intervals, partial downloads, filesystem failures, sharing changes and arbitrary scheduling produce an enormous state space. Nucleus's deterministic state machine enables reproducible, seeded simulation rather than relying on slow production rollout. [web:91][web:92] |

---

## 2. Google Drive architecture

| Area | Technical behavior |
|---|---|
| **Client consolidation** | Google replaced consumer **Backup and Sync** and enterprise **Drive File Stream** with one **Drive for desktop**, combining File Stream's streaming model with Backup and Sync's local mirroring. [web:65] |
| **Stream mode** | "My Drive" appears as a virtual drive or OS-integrated cloud location. Directory entries and metadata are local; content downloads and caches only on access. Scales beyond local disk. [web:62][web:69] |
| **Mirror mode** | The entire My Drive corpus is stored locally and in Drive, available offline by default. Needs disk for the full dataset. [web:62] |
| **Virtual-filesystem semantics** | A directory listing does not hydrate every item. Opening a streamed file triggers retrieval; writes are staged and uploaded. On Windows, Google documents its virtual Drive as presenting a **FAT-style virtual filesystem**, so applications may observe semantics unlike the underlying NTFS volume. [web:270] |
| **Google Docs are non-file objects** | A Doc is a Drive resource with a Google-specific MIME type and collaborative server-side state, not a local blob. Drive for desktop exposes `.gdoc`/`.gsheet` shortcut files; double-clicking opens a browser. Backup clients cannot meaningfully copy these as content. [web:256][web:258][web:259][web:264] |
| **Export boundary** | Conventional bytes require an **export** (`files.export`) to DOCX/PDF/ODT/XLSX. Export creates a representation and may lose collaboration history, comments or unsupported formatting. [web:71][web:75][web:266] |
| **Shortcut permissions** | A shortcut is metadata pointing at another resource. Access to the shortcut's folder does not grant access to the target. [web:257] |

---

## 3. OS integration APIs for an on-demand filesystem in 2026

| API | Placeholders | Hydration on read | Eviction | Pinning | Status badges |
|---|---|---|---|---|---|
| **Windows Cloud Files API / Cloud Filter (`cldflt.sys`, CFAPI)** | Provider registers a sync root and creates NTFS placeholders that look like ordinary files but use roughly filesystem-header-sized storage. The driver mediates I/O between applications and the provider. [web:106][web:223] | **Native.** A read triggers a fetch-data callback; the provider supplies ranges with `TRANSFER_DATA`. [web:114][web:218] | **Native.** `CfDehydratePlaceholder` removes resident data while leaving metadata, subject to file state and open handles. | **Native.** Pin states distinguish cloud-only placeholders, reclaimable cached files, and explicitly pinned files. [web:106][web:107] | **Native Shell integration** for cloud/check/syncing/error states. [web:106] |
| **Microsoft Files On-Demand** | The user-facing OneDrive model built on CFAPI, not a separate API. "Online-only", "locally available" and "always available" map to placeholder, cached and pinned. | Yes | Yes | Yes | Yes |
| **macOS File Provider extension** | Provider exposes a domain under Finder / `~/Library/CloudStorage`. The system owns namespace enumeration, materialization and lifecycle rather than a vendor kernel module intercepting syscalls. | **Native.** Opening a dataless item asks the provider to materialize it. | **Native.** `evictItem` returns a materialized item to dataless; macOS also evicts LRU content under disk pressure. [web:115][web:210] | Supported via system/user policy; Finder's "Download Now"/"Keep Downloaded". | **Native Finder integration** via File Provider item decorations. [web:205] |
| **macOS kernel extensions (legacy)** | Earlier clients used KAUTH or FUSE-like interception. Apple has **deprecated kernel extensions** in favour of File Provider/system extensions. [web:196][web:198][web:201] | Vendor-defined | Vendor-defined | Vendor-defined | Finder Sync extensions |
| **Linux FUSE** | A user-space daemon implements an entire mounted filesystem, synthesizing entries and fetching remote data on `open`/`read`. [web:217] | **Yes**, but as remote-read behavior rather than hydration of standardized placeholders inside ext4/Btrfs. | Provider-defined cache eviction; no common state machine. | Provider-defined; no universal "keep on this device" bit understood by desktops. | **No standard cross-desktop badge API.** Nautilus, Dolphin etc. differ. |
| **Linux ordinary-directory sync** | Syncthing/Nextcloud sync real files in an ext4/Btrfs directory, usually via inotify, but cannot leave universally recognized zero-byte placeholders. A FUSE mount or app-specific placeholder is required. **There was still no general Linux equivalent to CFAPI/File Provider by 2026.** [web:213][web:215] | No | Not standardized | Not standardized | Not standardized |

**Practical consequence:** Windows and macOS dictate much of virtual-file lifecycle and UI. Linux gives more freedom through FUSE, but every provider must design its own caching, offline policy, errors and desktop integration.

---

## 4. Open-source and self-hosted comparators

| System | Architecture | Sync, conflict and virtual-file behavior | Security and collaboration |
|---|---|---|---|
| **Nextcloud** | PHP/WebDAV platform with desktop, web and mobile clients. The desktop client records expected remote ETags and local state in a journal database and reconciles local and server scans. [web:166] | If both sides changed since the last common state, it keeps the local version as a timestamped conflict file and downloads the remote under the original name. VFS implementations expose small placeholders and fetch on demand, differing by OS. [web:34][web:168] | Strong app ecosystem, user/group sharing, links, federation. Optional folder-level client-side E2EE keeps keys and plaintext from the server, but encrypted folders lose server-side indexing, previews and some integrations. [web:89] |
| **Seafile** | Self-hosted, organized into independently versioned **libraries**. The internal model is explicitly Git-like: a library has commits; each commit points to a root filesystem object; directory objects point to children; a file object stores an ordered block-ID list. [web:181][web:183] | FS-object IDs derive from content, so unchanged files and subtrees are reused between commits. Files use **content-defined, variable-length chunks averaging about 8 MB**, so changed regions generate new blocks while unchanged blocks survive offset shifts better than fixed chunking. Commit, FS-object and block stores can be backed by S3 or Ceph. [web:181][web:188] | Very efficient for large datasets and revision history. Libraries are the sharing and encryption boundary. The object store is not a normal POSIX tree, so server-side edits must go through Seafile. |
| **Syncthing** | Decentralized device-to-device replication using the **Block Exchange Protocol** rather than an authoritative cloud namespace. BEP file records include a **version vector**. [web:134] | SHA-256 blocks; a receiver fetches only missing blocks, verifies each, writes a temporary file, then replaces the destination. Concurrent versions are causally detectable; one is renamed `name.sync-conflict-date-time-device.ext`. [web:40][web:128] | **Receive Encrypted** folders let an untrusted device store and relay encrypted data and hidden metadata without learning filenames, hashes or structure - which necessarily **reduces deduplication**, since the untrusted node must not learn equality. [web:38][web:126] |
| **rclone** | Command-line data mover across many backends. `copy`/`sync` are one-way; `bisync` keeps two-way state; `mount` presents a remote through FUSE. [web:46][web:51] | The mount's VFS cache serves applications needing seekable or writable files, but it is not a collaboration engine with a conflict database. `bisync` can preserve or rename conflict losers. [web:250] | `crypt` adds client-side filename and content encryption. Sharing and key distribution remain the administrator's problem. |
| **ownCloud Infinite Scale (oCIS)** | Go redesign. Instead of one assembled user namespace it exposes independently addressable **storage spaces**: personal files, project drives and incoming shares are all spaces with owners, permissions, quota, type and root ETag. [web:49][web:57] | Clients sync spaces over WebDAV/OCS/LibreGraph. Space-level endpoints and ETags give a clearer sync and permission boundary than one large account tree. [web:167][web:171] | Self-hosted enterprise collaboration and federation; not peer-to-peer. |
| **Resilio Sync** | Proprietary, descended from BitTorrent Sync. Data flows directly between authorized peers, optionally via an encrypted relay that does not persist content. [web:244] | Full-sync and selective-sync modes; selective exposes placeholders and downloads on demand, but a content-bearing peer must be online. Name, case and version collisions produce `.Conflict` files. [web:241][web:242] | Cloudless: authoritative copies live on participant devices. X.509 peer identities; availability and backup depend on the user's fleet. |

---

## 5. Hard correctness problems in file synchronization

| Problem | Required technical treatment |
|---|---|
| **Conflict detection without a generic merge** | Must distinguish "only local changed", "only remote changed", "both changed identically" and "both changed differently" relative to a common base. Office documents, databases, encrypted files and media cannot safely be line-merged. The conservative answer: keep one canonical path and write the other as a conspicuously named **conflicted copy**. [web:168][web:40] |
| **Causality / vector clocks** | Per-device counters form a version vector. If A dominates B, A includes B's history; incomparable vectors prove concurrent edits rather than relying on unreliable wall clocks. Vectors grow with participating identities and still do not merge byte content. [web:134] |
| **Server-ordered journals** | A central service assigns each accepted mutation an increasing sequence ID, simplifying incremental fetch and giving one remote order. But "last accepted by the server" is an ordering rule, not proof the last writer understood the previous edit. [web:2] |
| **Partial and interrupted uploads** | Upload to immutable temporary blocks or a resumable session, validate by hash, and make visible only at a final metadata commit; garbage-collect abandoned blocks. Downloads build and verify a temporary file before atomic replacement. [web:15][web:320] |
| **Rename detection** | A stateless scanner sees rename as delete plus create. Stable file IDs, inode IDs, paired watcher rename events, or matching a new path to the old blocklist allow a metadata-only move. Without stable identity, heuristic hash matching confuses duplicates and cannot distinguish copy from move. Nucleus introduced globally unique identities and atomic moves because Classic's delete-plus-add representation could lose data. [web:91] |
| **Filesystem watching at scale** | `ReadDirectoryChangesW`/USN journal on Windows, FSEvents on macOS, inotify on Linux. Events are **hints, not the source of truth**: queues overflow, events coalesce, mounts disappear, and the client may be offline. A correct engine persists a cursor/journal and periodically (or after overflow) rescans against its database. [web:337][web:344] |
| **Files still being written** | A close event is not universally available. Applications save via repeated writes, lock files, temporary names or atomic rename. Clients debounce, test size/mtime stability, open with appropriate sharing flags, and retry - and must hash a consistent snapshot or verify before commit. |
| **Locked destinations** | Windows sharing modes, antivirus and indexers can prevent replacement or deletion. The engine must retain pending state and retry rather than declaring success or spawning duplicate conflicts. On POSIX, replacing a pathname while the old inode is open is possible, but the old process keeps seeing the old inode. |
| **Atomic local publication** | Write a temporary file on the same filesystem, validate block hashes, flush where durability is required, then atomically rename. Atomic visibility differs from crash durability: durable POSIX publication may require `fsync` of both file and parent directory. |
| **Case and normalization collisions** | Compare names under both the server's canonical rules and the destination filesystem's rules before publication. Two cloud entries collapsing to one local name must be an error or a deterministic rename; silently choosing one loses data. |
| **Deletes versus edits** | "Deleted on A, edited on B while offline" is a real conflict, not automatically a delete. Policies: edit-wins/resurrection, delete-wins plus conflict preservation, or server-order wins. [web:40] |
| **Permissions and membership races** | A device may hold cached plaintext when access is revoked, or upload an edit after its role changed. Authorization must be checked **at commit**, not only at open. Cached local copies cannot be cryptographically recalled. |
| **End-to-end encryption** | E2EE prevents the server reading content but disables or complicates dedup, previews, indexing, malware scanning, server-side conversion, web editing and recovery. Multi-user sharing needs authenticated key wrapping, membership revocation and device recovery. Dropbox's encrypted team folders exclude Dropbox from folder keys but lose shared links, previews, search, API access and several governance functions. [web:226][web:228] |
| **Provider encryption vs zero knowledge** | TLS plus server-side encryption protects transport and disks but is **not** E2EE if the service can unwrap keys. Google Workspace CSE requires an administrator, identity provider and external Key Access Control List Service. [web:234][web:235] |

---

## 6. Sharing and permission models for non-technical users

| Model | User-visible behavior | Confusion or risk |
|---|---|---|
| **Share link** | A URL grants access by scope (anyone with the link / organization members / specifically added people) and role. Dropbox links can reach people without accounts; admins can prohibit external links. [web:136][web:140] | Users treat a link like an email attachment rather than a continuing capability. They do not grasp that forwarding extends access, or that "has the URL" differs from "was invited". |
| **Per-folder ACL / membership** | Users or groups become owner, editor or viewer, normally with inherited access to descendants. Dropbox editors can add, modify, delete, download and share; viewers can view, download, share and comment. [web:137] | "Editor" implicitly includes deleting and resharing. Users expect file-level exceptions to override the parent. |
| **Google Drive roles** | Owner, Editor, Commenter, Viewer. Folders propagate to existing and future descendants; shared drives add Manager, Content manager, Contributor. [web:139][web:141][web:162] | Personal ownership vs org-owned shared drives, inherited vs direct permissions, and "can edit" vs "can reorganize/delete" are hard to predict. Moving content across boundaries changes effective access. |
| **External collaborator invitation** | The owner invites an address, producing durable auditable membership. | Recipients sign in with the wrong account, or get access to a shortcut but not its target. [web:257] |
| **Download/copy vs collaboration** | A viewer may download a private copy; an exported Doc is a static file, not the collaborative object. | Users assume revoking a share recalls downloads, or that emailing an export preserves live updates. Neither is true. |
| **Selective sync vs permission** | Selective sync hides a folder on one device but does not revoke cloud access or remove it from collaborators. Online-only merely removes local bytes. | Users read "not on my computer" as deleted, private, or unshared. [web:16] |
| **Conflict copies** | Both versions are preserved rather than one silently overwritten. | The filename looks like an unwanted duplicate, so users delete it without comparing, and cannot tell which is authoritative. |
| **Permission inheritance after moves** | Content usually takes rules from its new parent, space or shared drive. [web:139][web:146] | Users think a file carries its old audience; the system treats location as the security boundary. |
| **Overall usability finding** | Non-technical users have a "muddy" model of access rights, rarely inspect effective permissions, and do not anticipate how move, delete, copy and email-link operations affect collaborators. Studies identify the "permission clash" where a link is emailed without granting recipients access. [web:151][web:163] | The safest UI shows **who can access**, **what they can do**, **whether access is inherited**, and **whether the recipient gets a live object, a link, or an independent copy** at the point of sharing - not behind a secondary dialog. |

## Bottom line

| Design choice | Best fit |
|---|---|
| Central service plus server journal | Dropbox, Google Drive: availability, sharing, web collaboration, deterministic ordering |
| Git-like content-addressed libraries | Seafile: efficient self-hosted revisioned sync and dedup |
| Federated WebDAV collaboration suite | Nextcloud, oCIS: organizational control and apps |
| Version-vector peer replication | Syncthing: decentralized sync with precise causal-conflict detection |
| Cloud-backend toolbox | rclone: scripted copying, mounting, encryption |
| Cloudless managed P2P | Resilio: direct device replication with an available peer |
| Native on-demand desktop experience | **CFAPI on Windows**, **File Provider on macOS**, **FUSE on Linux** - accepting that Linux pinning, placeholders and badges must be designed per desktop |

## Citations

[1] https://dropbox.tech/infrastructure/inside-lan-sync
[2] https://dropbox.tech/infrastructure/streaming-file-synchronization
[3] https://dropbox.tech/infrastructure/-broccoli--syncing-faster-by-syncing-less
[4] https://help.dropbox.com/sync/lan-sync-overview
[7] https://help.dropbox.com/sync/sync-overview
[12] https://dropbox.tech/infrastructure/inside-the-magic-pocket
[15] https://dropbox.tech/mobile/making-camera-uploads-for-android-faster-and-more-reliable
[16] https://help.dropbox.com/sync/team-selective-sync
[18] https://help.dropbox.com/sync/sync-overview
[24] https://dropbox.tech/infrastructure/going-deeper-with-project-infinite
[25] https://help.dropbox.com/sync/selective-sync-overview
[26] https://help.dropbox.com/sync/make-files-online-only
[34] https://docs.nextcloud.com/desktop/3.12/architecture.html
[38] https://docs.syncthing.net/users/untrusted.html
[40] https://docs.syncthing.net/users/syncing.html
[46] https://rclone.org/commands/rclone_mount/
[49] https://doc.owncloud.com/ocis/next/architecture/architecture.html
[51] https://rclone.org/docs/
[57] https://doc.owncloud.com/ocis/latest/admin/architecture/architecture.html
[62] https://support.google.com/drive/answer/16631477?hl=en
[65] https://blog.google/products-and-platforms/products/workspace/drive-for-desktop/
[69] https://support.google.com/drive/answer/10838124?hl=en
[71] https://developers.google.com/workspace/drive/api/guides/manage-downloads
[75] https://developers.google.com/workspace/drive/api/reference/rest/v3/files/get
[89] https://nextcloud.com/encryption/
[91] https://dropbox.tech/infrastructure/rewriting-the-heart-of-our-sync-engine
[92] https://dropbox.tech/infrastructure/-testing-our-new-sync-engine
[103] https://dropbox.tech/mobile/our-counterintuitive-fix-for-android-path-normalization
[106] https://learn.microsoft.com/en-us/windows/win32/cfapi/build-a-cloud-file-sync-engine
[107] https://learn.microsoft.com/en-us/windows/win32/api/_cloudapi/
[114] https://learn.microsoft.com/en-us/windows/win32/api/cfapi/ns-cfapi-cf_operation_parameters
[115] https://developer.apple.com/videos/play/wwdc2021/10182/
[126] https://docs.syncthing.net/users/untrusted.html
[128] https://docs.syncthing.net/v1.27.3/users/syncing.html
[134] https://docs.syncthing.net/v1.21.0/specs/bep-v1.html
[136] https://help.dropbox.com/share/manage-team-sharing
[137] https://help.dropbox.com/share/set-file-folder-permissions
[139] https://support.google.com/drive/answer/7166529
[140] https://help.dropbox.com/share/share-outside-dropbox
[141] https://support.google.com/drive/answer/2494822
[146] https://support.google.com/drive/answer/2494893
[151] https://www.nngroup.com/articles/cloud-storage/
[162] https://support.google.com/a/users/answer/9310249?hl=en
[163] https://cs.brown.edu/people/sk/Publications/Papers/Published/akf-sharing-scaring/paper.pdf
[166] https://docs.nextcloud.com/desktop/3.12/architecture.html
[167] https://doc.owncloud.com/ocis/7.1/_attachments/architecture/infinite-scale-c4-model.dsl
[168] https://docs.nextcloud.com/server/latest/user_manual/en/desktop/conflicts.html
[171] https://doc.owncloud.com/ocis/latest/admin/architecture/architecture.html
[181] https://manual.seafile.com/12.0/develop/data_model/
[183] https://manual.seafile.com/13.0/develop/data_model/
[188] https://manual.seafile.com/13.0/setup/setup_with_s3/
[196] https://developer.apple.com/support/kernel-extensions/
[198] https://developer.apple.com/la/videos/play/wwdc2021/10182/
[201] https://developer.apple.com/documentation/systemextensions
[205] https://developer.apple.com/documentation/fileprovider/synchronizing-files-using-file-provider-extensions
[210] https://developer.apple.com/documentation/fileprovider/nsfileprovidermanager/evictitem
[213] https://stackoverflow.com/questions/66467255/virtual-file-system-api-cloud-provider-synchronization-api-for-linux
[215] https://deepwiki.com/nextcloud/desktop/3.2-virtual-file-system
[217] https://docs.kernel.org/filesystems/fuse/
[218] https://github.com/MicrosoftDocs/win32/blob/docs/desktop-src/cfApi/cloud-files-functions.md
[223] https://github.com/MicrosoftDocs/win32/blob/docs/desktop-src/cfApi/cloud-files-api-portal.md
[226] https://dropbox.tech/security/end-to-end-encryption-for-dropbox-teams
[228] https://help.dropbox.com/security/encrypted-team-folders
[234] https://support.google.com/drive/answer/10519333
[235] https://developers.google.com/workspace/cse/guides/handle-cse-files
[241] https://help.resilio.com/hc/en-us/articles/204753629-Conflict-files-in-Sync
[242] https://help.resilio.com/hc/en-us/articles/205457775-Synchronization-Modes
[244] https://help.resilio.com/hc/en-us/articles/205451025-Can-others-see-my-files-How-secure-is-sharing-by-Resilio-Sync
[250] https://rclone.org/bisync/
[256] https://support.google.com/a/answer/7502016?hl=en
[257] https://support.google.com/drive/answer/9700156
[258] https://developers.google.com/workspace/drive/api/guides/mime-types
[259] https://developers.google.com/workspace/drive/api/guides/about-files
[264] https://support.google.com/docs/answer/10838124
[266] https://developers.google.com/workspace/drive/api/guides/ref-export-formats
[270] https://support.google.com/docs/answer/2375082
[271] https://help.dropbox.com/installs/macos-support-for-expected-changes
[273] https://help.dropbox.com/installs/mac-external-drive-support
[282] https://help.dropbox.com/installs/macos-for-teams
[284] https://dropbox.tech/infrastructure/going-deeper-with-project-infinite
[289] https://dropbox.tech/infrastructure/streaming-file-synchronization
[294] https://www.dropbox.com/business/trust/security/encryption
[303] https://help.dropbox.com/organize/file-names
[304] https://help.dropbox.com/sync/files-not-syncing
[320] https://developers.dropbox.com/dbx-performance-guide
[337] https://github.com/emcrisostomo/fswatch/blob/master/README.md
[344] https://emcrisostomo.github.io/fswatch/
