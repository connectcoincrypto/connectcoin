# File and socket limits

ConnectCoin raises limits inside its own process at startup. It does not edit
system configuration, acquire administrator privileges, raise a POSIX hard
limit, change P2C concurrency, or reserve thousands of sockets in advance.
These limits are operating-system/runtime policies, not measurements of how
many connections the machine can sustain.

## Platforms with build guides in this repository

| Platform | Process/runtime limit | Automatic behavior |
| --- | --- | --- |
| Linux (including Ubuntu and Mint) | `RLIMIT_NOFILE` soft and hard limits; kernel ceilings such as `fs.nr_open` also apply | Try soft = inherited hard; use a bounded finite fallback if rejected; query the resulting limit. |
| macOS | `RLIMIT_NOFILE`, plus `kern.maxfilesperproc` | Same soft-limit raise; query the kernel ceiling read-only and cap the connection budget accordingly. An unlimited resource limit does not imply unlimited descriptors. |
| FreeBSD, OpenBSD, NetBSD | `RLIMIT_NOFILE`, with kernel/login-class ceilings | Same soft-limit raise and observed-limit check; accept kernel clamping instead of assuming the request succeeded unchanged. |
| Windows with UCRT (MSVC or UCRT MinGW) | CRT `FILE*` streams default to 512, maximum 8192 | Try `_setmaxstdio(8192)`, then log the observed value. |
| Windows with legacy MSVCRT MinGW | CRT streams default to 512, maximum 2048 | Try `_setmaxstdio(2048)`, then log the observed value. |

The POSIX fallback searches only for an accepted soft limit, no higher than the
inherited hard limit or `INT_MAX`. It never requests a lower soft limit. Failure
to read the initial resource limit fails the node's startup capacity check;
failure to verify a change uses the previously observed value conservatively.
On macOS, a failed kernel-ceiling query is logged and the resource-limit budget
is retained. The budget is not a guarantee that this many descriptors are free.

## Socket readiness is a separate limit

On POSIX systems, ConnectCoin uses `poll()` for socket readiness. Unlike a fixed
`fd_set`, this accepts descriptor numbers above `FD_SETSIZE`. Each descriptor
appears only once, with its requested events combined; errors, hangups and
invalid descriptors are reported to the caller. Darwin also registers hangup
interest for waits that request only error notification.

Windows retains Winsock `select()` and its existing 1024-entry set capacity.
An oversized `WaitMany()` request fails explicitly instead of silently dropping
sockets from a full set. This is a per-wait-set limit, not a 1024-socket limit
for the entire process. P2C HTTPS waits monitor each connection separately.
The legacy value returned for Windows connection budgeting remains 2048 and is
not a queried kernel limit. Raising CRT streams does not raise Winsock capacity.

## IPC child processes

IPC child processes close inherited descriptors other than standard streams
and their communication socket before executing another program. Linux uses
`close_range` when available, and BSD systems use `closefrom` for the upper
range, avoiding a syscall for every possible descriptor under a large limit.
The portable fallback includes the highest valid descriptor and uses a finite
hard-limit bound, with the macOS kernel ceiling when available. If neither a
range API nor a finite bound is available, the child fails closed instead of
leaking descriptors or iterating over an unbounded range.
The fallback assumes inherited descriptors are below the known bound; it does
not enumerate descriptors opened before an external launcher lowered that
bound. This cleanup is not a sandbox boundary.

Socket ownership is protected when argument preparation or process creation
fails. The post-fork child uses fixed diagnostics and exits without throwing
exceptions or invoking stdio error reporting.

## Other systems with the same interface

Android uses Linux resource limits through Bionic. DragonFly BSD also provides
`RLIMIT_NOFILE`. Solaris and illumos expose the same interface, with resource
controls such as `process.max-file-descriptor`. The generic non-Windows path
applies where ConnectCoin can be built with these POSIX interfaces. This does
not establish full project support or native test coverage for those systems.

## Limits the application does not override

- POSIX hard limits and administrator-configured kernel, service, container or
  login-session restrictions. A process can raise its soft limit only within
  what the operating system permits.
- Windows kernel handle capacity, memory availability and the CRT's built-in
  maximum. `SetHandleCount()` is not a way to raise modern Windows limits.
- Memory, worker threads, ephemeral TCP ports, connection rate and remote-server
  limits. A larger descriptor allowance does not prevent exhaustion of these.

If the inherited hard limit is too small, an administrator must adjust the
relevant launcher/service policy and restart the application. ConnectCoin does
not make that machine-wide change automatically. These changes do not require
deleting wallet files, lock files or blockchain data.

## Verification

Startup logs show `File descriptor limits` on POSIX and `CRT stream limit` on
Windows; macOS additionally logs its kernel ceiling when available.
`feature_file_descriptor_limit.py` tests constrained child-process startup on
POSIX, without changing the test runner's limits. Socket unit tests cover
readiness, combined events, timeout, disconnects and high descriptor numbers
when the test process permits them. Windows unit tests open more than 512
temporary null-device streams and restore the test process's stream limit.
The IPC `mptest` target runs a separate helper to check highest-descriptor
cleanup without changing the test runner's limits, and also tests callback
failure cleanup and a missing executable.

## Primary references

- [Linux getrlimit/setrlimit](https://man7.org/linux/man-pages/man2/getrlimit.2.html)
  and [Linux select limits](https://man7.org/linux/man-pages/man2/select.2.html).
- [Apple resource-limit implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_resource.c)
  and [Apple poll implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/sys_generic.c).
- [FreeBSD resource-limit implementation](https://github.com/freebsd/freebsd-src/blob/main/sys/kern/kern_resource.c),
  [OpenBSD getrlimit](https://man.openbsd.org/getrlimit.2),
  [NetBSD getrlimit](https://man.netbsd.org/getrlimit.2).
- [Microsoft UCRT stream limits](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/setmaxstdio?view=msvc-170),
  [legacy CRT stream limits](https://learn.microsoft.com/en-us/previous-versions/visualstudio/visual-studio-2013/6e3b887c(v=vs.120)),
  [Winsock select](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-select)
  and [SetHandleCount](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-sethandlecount).
- [Android Bionic resource interface](https://android.googlesource.com/platform/bionic/+/eb04ed5/libc/include/sys/resource.h),
  [DragonFly resource-limit implementation](https://github.com/DragonFlyBSD/DragonFlyBSD/blob/master/sys/kern/kern_plimit.c),
  [Solaris setrlimit](https://docs.oracle.com/cd/E88353_01/html/E37841/setrlimit-2.html)
  and [illumos resource-limit implementation](https://github.com/illumos/illumos-gate/blob/master/usr/src/uts/common/syscall/rlimit.c).
