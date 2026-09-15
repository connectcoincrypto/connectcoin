This directory contains integration tests that test connectcoind and its
utilities in their entirety. It does not contain unit tests, which
can be found in [/src/test](/src/test), [/src/wallet/test](/src/wallet/test),
etc.

This directory contains the following sets of tests:

- [fuzz](/test/fuzz) A runner to execute all fuzz targets from
  [/src/test/fuzz](/src/test/fuzz).
- [functional](/test/functional) which test the functionality of
connectcoind and connectcoin-qt by interacting with them through the RPC and P2P
interfaces.
- [lint](/test/lint/) which perform various static analysis checks.

The fuzz tests, functional
tests and lint scripts can be run as explained in the sections below.

# Running tests locally

Before tests can be run locally, ConnectCoin Core must be built. See the [building instructions](/doc#building-and-development) for help.

The following examples assume that the build directory is named `build`.

## Fuzz tests

See [/doc/fuzzing.md](/doc/fuzzing.md)

### Functional tests

#### Dependencies and prerequisites

The ZMQ functional test requires a python ZMQ library. To install it:

- on Unix, run `sudo apt-get install python3-zmq`
- on mac OS, run `pip3 install pyzmq`

The IPC functional test requires a python IPC library. `pip3 install pycapnp` may work, but if not, install it from source:

```sh
git clone -b v2.2.1 https://github.com/capnproto/pycapnp
pip3 install ./pycapnp
```

If that does not work, try adding `-C force-bundled-libcapnp=True` to the `pip` command.
Depending on the system, it may be necessary to install and run in a venv:

```sh
python -m venv venv
git clone -b v2.2.1 https://github.com/capnproto/pycapnp
venv/bin/pip3 install ./pycapnp -C force-bundled-libcapnp=True
venv/bin/python3 build/test/functional/interface_ipc.py
```

The functional tests assume Python UTF-8 Mode, which is the default on most
systems.
On Windows the `PYTHONUTF8` environment variable must be set to 1:

```cmd
set PYTHONUTF8=1
```

#### Running the tests

Individual tests can be run by directly calling the test script, e.g.:

```
build/test/functional/feature_typed_outputs.py
```

or can be run through the test_runner harness, eg:

```
build/test/functional/test_runner.py feature_typed_outputs.py
```

You can run any combination (incl. duplicates) of tests by calling:

```
build/test/functional/test_runner.py <testname1> <testname2> <testname3> ...
```

Use `--filter` to select a family from the active test suite. For example,
to run the active wallet tests:

```
build/test/functional/test_runner.py --filter="^wallet_"
```

Combine families with a regular expression:

```
build/test/functional/test_runner.py --filter="^(tool_|mempool_)"
```

The runner excludes tests whose fixtures depend on unsupported transaction
formats from its active suites. These tests remain explicitly selectable for
porting and diagnosis; passing their names, including shell-expanded wildcards,
will select them. See `QUARANTINED_TYPED_OUTPUT_SCRIPTS` in
[test_runner.py](functional/test_runner.py). `--filter` without explicit test
names keeps the active suite's exclusions.

Run the regression test suite with:

```
build/test/functional/test_runner.py
```

Run the active base and extended suites with:

```
build/test/functional/test_runner.py --extended
```

ConnectCoin does not have published previous releases yet, so backwards
compatibility tests are skipped. The inherited `test/get_previous_releases.py`
helper downloads Bitcoin Core binaries and must not be used as ConnectCoin test
input: those binaries use a different genesis block and network identity. This
workflow can be re-enabled after ConnectCoin publishes versioned binaries and
project-owned checksums. Merely placing binaries in a releases directory does
not enable these tests; future project-owned compatibility runs must opt in
explicitly with `--previous-releases`.

By default, up to 4 tests will be run in parallel by test_runner. To specify
how many jobs to run, append `--jobs=n`

The individual tests and the test_runner harness have many command-line
options. Run `build/test/functional/test_runner.py -h` to see them all.

#### Speed up test runs with a RAM disk

If you have spare RAM, a RAM disk can hold the functional tests' cache and
temporary data. Its effect on runtime depends on the tests and storage system.
Reserve enough memory for the test processes as well as the RAM disk.

**Linux**

To create a 4 GiB RAM disk at `/mnt/tmp/`:

```bash
sudo mkdir -p /mnt/tmp
sudo mount -t tmpfs -o size=4g tmpfs /mnt/tmp/
```

Configure the size of the RAM disk using the `size=` option. Required capacity
depends on the selected tests, concurrent jobs, and retained logs. The example
size does not guarantee enough space for every test selection.

To use it, set `--cachedir` and `--tmpdirprefix`. Start with one job and monitor
space and memory use:

```bash
build/test/functional/test_runner.py --cachedir=/mnt/tmp/cache --tmpdirprefix=/mnt/tmp --jobs=1
```

Once finished with the tests and the disk, and to free the RAM, simply unmount the disk:

```bash
sudo umount /mnt/tmp
```

**macOS**

To create a 4 GiB RAM disk named "ramdisk" at `/Volumes/ramdisk/`:

```bash
diskutil erasevolume HFS+ ramdisk $(hdiutil attach -nomount ram://8388608)
```

Configure the RAM disk size, expressed as the number of blocks, at the end of the command
(`4096 MiB * 2048 blocks/MiB = 8388608 blocks` for 4 GiB). To run the tests using the RAM disk:

```bash
build/test/functional/test_runner.py --cachedir=/Volumes/ramdisk/cache --tmpdirprefix=/Volumes/ramdisk --jobs=1
```

To unmount:

```bash
umount /Volumes/ramdisk
```

#### Troubleshooting and debugging test failures

##### Resource contention

The P2P and RPC ports used by the connectcoind nodes-under-test are chosen to make
conflicts with other processes unlikely. However, if there is another connectcoind
process running on the system (perhaps from a previous test which hasn't successfully
killed all its connectcoind nodes), then there may be a port conflict which will
cause the test to fail. It is recommended that you run the tests on a system
where no other connectcoind processes are running.

On linux, the test framework will warn if there is another
connectcoind process running when the tests are started.

After a test failure, identify any remaining test nodes by checking their
process command lines against the test's printed temporary directory. Stop
only those nodes, using RPC with the matching regtest data directory when
available. If RPC is unavailable, send a normal termination signal only to
the verified test process IDs. Wait for them to exit before changing their
data or cache. Do not terminate unrelated test runs or wallet/node processes.

##### Data directory cache

Tests that use a cached chain copy a 199-block cache and generate a fresh block
to reach height 200 during setup. The runner creates a temporary cache for each
invocation. Individual scripts use `build/test/cache` by default; `--cachedir`
can select a persistent cache in either mode.

If a persistent cache becomes unusable, stop the test nodes that use it and
verify the exact cache path before moving that cache aside. The next run will
recreate it. A normal runner invocation creates a fresh cache, so clearing
`build/test/cache` does not affect that invocation unless it was explicitly
selected. Keep wallet and other node data directories separate from test caches.

##### Test logging

The tests contain logging at five different levels (DEBUG, INFO, WARNING, ERROR
and CRITICAL). From within your functional tests you can log to these different
levels using the logger included in the test_framework, e.g.
`self.log.debug(object)`. By default:

- when run through the test_runner harness, *all* logs are written to
  `test_framework.log` and no logs are output to the console.
- when run directly, *all* logs are written to `test_framework.log` and INFO
  level and above are output to the console.
- when run by [our CI (Continuous Integration)](/ci/README.md), no logs are output to the console. However, if a test
  fails, the `test_framework.log` and connectcoind `debug.log`s will all be dumped
  to the console to help troubleshooting.

These log files can be located under the test data directory (which is always
printed in the first line of test output):
  - `<test data directory>/test_framework.log`
  - `<test data directory>/node<node number>/regtest/debug.log`.

The node number identifies the relevant test node, starting from `node0`, which
corresponds to its position in the nodes list of the specific test,
e.g. `self.nodes[0]`.

To change the level of logs output to the console, use the `-l` command line
argument.

`test_framework.log` and connectcoind `debug.log`s can be combined into a single
aggregate log by running the `combine_logs.py` script. The output can be plain
text, colorized text or html. For example:

```
build/test/functional/combine_logs.py -c <test data directory> | less -r
```

will pipe the colorized logs from the test into less.

Use `--tracerpc` to trace out all the RPC calls and responses to the console. For
some tests (eg any that use `submitblock` to submit a full block over RPC),
this can result in a lot of screen output.

By default, the test data directory will be deleted after a successful run.
Use `--nocleanup` to leave the test data directory intact. The test data
directory is never deleted after a failed test.

##### Attaching a debugger

A python debugger can be attached to tests at any point. Just add the line:

```py
import pdb; pdb.set_trace()
```

anywhere in the test. You will then be able to inspect variables, as well as
call methods that interact with the connectcoind nodes-under-test.

If further introspection of the connectcoind instances themselves becomes
necessary, this can be accomplished by first setting a pdb breakpoint
at an appropriate location, running the test to that point, then using
`gdb` (or `lldb` on macOS) to attach to the process and debug.

For instance, to attach to `self.nodes[1]` during a run you can get
the pid of the node within `pdb`.

```
(pdb) self.nodes[1].process.pid
```

Alternatively, you can find the pid by inspecting the temp folder for the specific test
you are running. The path to that folder is printed at the beginning of every
test run:

```bash
2017-06-27 14:13:56.686000 TestFramework (INFO): Initializing test directory /tmp/user/1000/testo9vsdjo3
```

Use the path to find the pid file in the temp folder:

```bash
cat /tmp/user/1000/testo9vsdjo3/node1/regtest/connectcoind.pid
```

Then you can use the pid to start `gdb`:

```bash
gdb /home/example/connectcoind <pid>
```

Note: gdb attach step may require ptrace_scope to be modified, or `sudo` preceding the `gdb`.
See this link for considerations: https://www.kernel.org/doc/Documentation/security/Yama.txt

Often while debugging RPC calls in functional tests, the test might time out before the
process can return a response. Use `--timeout-factor 0` to disable all RPC timeouts for that particular
functional test. Ex: `build/test/functional/wallet_hd.py --timeout-factor 0`.

### Lint tests

See the README in [test/lint](/test/lint).

# Writing functional tests

You are encouraged to write functional tests for new or existing features.
Further information about the functional test framework and individual
tests is found in [test/functional](/test/functional).
