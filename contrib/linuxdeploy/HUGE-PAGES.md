# Optional Linux Huge Pages setup

Installing a DEB/RPM or opening the portable/AppImage bundle does **not** change
memory reservations or privileges. There is no package-maintainer hook, service,
automatic elevation, or mining startup. The optional `connectcoin-hugepages`
command is a separate, explicitly invoked administrator tool. It requires your
distribution's Python 3 (3.10 or newer on the supported package platforms).
Core itself does not need Python.

Use this on the Linux host, not inside a container or a restricted memory/NUMA
namespace. Review other applications' HugeTLB requirements first. These are
unswappable, globally shared kernel pages; reserving them reduces the memory
available for normal applications. This tool does not manage transparent huge
pages (THP), mount hugetlbfs, change swappiness, alter group membership, add file
capabilities, or modify boot parameters or `/etc/sysctl.d`.

## Preview and apply

First close Core normally so its cached RandomX contexts will be recreated
after configuration. Do not run the wallet or daemon as root. Choose an existing
non-root group to which your normal Core user belongs; **all members** gain
access to the entire shared HugeTLB pool, including pre-existing reservations,
not just this budget or Core's allocations. The helper never creates a group or adds users.
`id -gn` below selects the invoking user's primary group before `sudo` runs.
If a different non-root `vm.hugetlb_shm_group` is already configured, the helper
refuses to replace it. Ask the administrator to review the existing policy.

With a native DEB/RPM installation:

```sh
connectcoin-hugepages status
connectcoin-hugepages status --group "$(id -gn)" --datasets 1 --vm-count 4
sudo /usr/bin/connectcoin-hugepages apply --group "$(id -gn)" --datasets 1 --vm-count 4 --confirm
```

The example budgets **one** shared dataset and **four** concurrent VM
scratchpads; it is not a universal recommended setting. Explicitly choose:

- `--datasets 1` for one key, or `2` for the current/next epoch transition.
  Core admits at most two prepared FAST entries/builds. A one-dataset budget
  may fall back during a key change.
- `--vm-count N` (1–1024) for the concurrent mining **and validation** VM
  scratchpads you want to budget. Distinct JIT-policy pools and in-flight work
  can need additional VMs. This is a memory budget, not a mining-thread setting.

The calculation rounds each 2080 MiB-minus-64-byte RandomX dataset, each
temporary 256 MiB initialization cache, and each 2 MiB scratchpad separately to
the kernel page size. It also budgets one additional shared 256 MiB LIGHT cache,
which can remain alive while FAST initializes; this is not a cache per worker.
The one-dataset/one-VM case therefore needs 1297 pages, not a fixed blanket
reservation for every machine. This helper supports only the default **2 MiB** HugeTLB
page size used by the Linux x86_64 packages, and refuses other defaults. It
uses existing free, unreserved pages and only increases the persistent pool
when necessary; it never reduces an existing reservation during `apply`.

It refuses new reservations exceeding 50% of total RAM as HugeTLB, and requires
at least 2 GiB and 25% of total RAM to remain available. These conservative
checks are not a guarantee against memory pressure, fragmentation, NUMA limits,
other users, cgroup limits, or allocation failure. A successful kernel reservation
is not proof that every RandomX allocation will use Huge Pages. Core's Mining
page and `getcpumininginfo.randomx_dataset` report the dataset allocator result.

Without `--confirm` no settings change. The helper does not invoke `sudo` itself.
Changes affect only the current boot and are lost at reboot; distributions may
apply their own existing boot-time policy. A normal restart of Core retries the
dataset allocation. Do not re-run `apply` while its previous backup is active.

## Portable tarball and AppImage

The unpacked portable bundle contains the same helper and this document:

```sh
./connectcoin-hugepages status
./AppRun --tool connectcoin-hugepages status --group "$(id -gn)" --datasets 1 --vm-count 4
```

For an AppImage, `./ConnectCoin-Core-<version>-x86_64.AppImage --tool
connectcoin-hugepages status` reads the kernel state without elevation. To make
changes, extract the verified AppImage **as your normal user** into an empty
directory using `--appimage-extract`, or use the unpacked tarball. Then explicitly
run only the reviewed helper with the isolated system interpreter:

```sh
sudo /usr/bin/python3 -I /absolute/path/to/bundle/usr/share/connectcoin/hugepages.py apply --group "$(id -gn)" --datasets 1 --vm-count 4 --confirm
```

For an extracted AppImage, the bundle root is `squashfs-root`. Do not run the
AppImage runtime, `AppRun`, GUI, or daemon with `sudo`. Running a user-owned
helper with `sudo` grants its code administrator authority: verify the artifact
checksum and review the file before doing so. The launcher uses `/usr/bin/python3
-I`, clears inherited loader/Python variables, and does not add bundled Qt
libraries to its environment. No interpreter is downloaded or bundled.

## Restore and failures

Stop Core and every other HugeTLB user before restoring, then run:

```sh
sudo /usr/bin/connectcoin-hugepages restore --confirm
```

For a portable/extracted bundle, use the same isolated interpreter and script
path as above, replacing `apply ...` with `restore --confirm`.

The helper writes a root-owned journal under `/var/lib/connectcoin-hugepages`
before touching the kernel. It saves the original page count and group, the boot
ID, the plan, and observed readback values. `restore` requires that its observed
settings remain unchanged and that all huge pages are free, unreserved and have
no surplus. It restores the original group and pool size, not an assumed zero.
Completed audit backups are retained; uninstalling Core does not delete them
or undo administrator changes. Restore before uninstalling, or keep a verified
copy of this helper. After a reboot, `restore` only archives the expired journal
and does not apply old settings to the new boot.

If the kernel reserves only part of the requested pool, the helper reports
failure, records the actual count, does not grant new group access, and tells
you to run `restore`. It never repeatedly allocates or drops system caches to
force success. If killed during a write, or if another administrator changes
the controls, automatic restoration is refused and the journal is retained for
administrator review. Do not delete an active journal to bypass that check.
The file lock serializes this helper only; do not concurrently modify HugeTLB
settings or start HugeTLB users while applying/restoring them.

## Implementation and tests

The runtime controls are `/proc/sys/vm/nr_hugepages` and
`/proc/sys/vm/hugetlb_shm_group`. Anonymous `MAP_HUGETLB` requires `CAP_IPC_LOCK`
or membership in that group; a hugetlbfs mount is not needed. This is distinct
from a per-process `mlock` limit. See the
[Linux kernel HugeTLB documentation](https://docs.kernel.org/admin-guide/mm/hugetlbpage.html)
and [`mmap(2)` permissions](https://man7.org/linux/man-pages/man2/mmap.2.html).

Offline tests use fake `/proc` files and injected kernel/store objects. They
never write the host's controls or request administrator privileges:

```sh
python3 -m unittest discover -s contrib/linuxdeploy -p 'test_hugepages.py' -v
```
