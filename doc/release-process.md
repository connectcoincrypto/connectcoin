# ConnectCoin release process

ConnectCoin has no production release, official binary host, signing team,
attestation repository, update channel, or supported-version lifecycle yet.
Do not publish binaries as a supported mainnet release until the pre-launch
blockers in [connectcoin-branding.md](connectcoin-branding.md) and
[SECURITY.md](/SECURITY.md) are resolved.

The historical release notes and some Guix tooling in this tree are inherited
from Bitcoin Core. Bitcoin Core's download servers, signing keys,
`guix.sigs`, detached-signature repository, Transifex project, mailing lists,
and security contacts do not authenticate or support ConnectCoin releases.

## Development build checklist

For an explicitly labeled development build:

1. Freeze the source revision and record the upstream Bitcoin Core base commit.
2. Review every value in `doc/connectcoin-branding.md`, including name, ticker,
   address/key prefixes, URI scheme, ports, message starts, seeds, and genesis
   hashes.
3. Build from a clean worktree for every supported platform.
4. Run the complete unit/CTest suite and the relevant functional, fuzz, GUI,
   packaging, and upgrade tests. Record skipped tests and exact toolchain
   versions.
5. Inspect the packaged filenames, executable metadata, icons, default data
   directories, configuration paths, autostart entries, and displayed units.
6. Publish SHA-256 hashes with an explicit warning that the build is
   experimental and not suitable for production funds.

## Requirements before a public release

The project must establish and document:

- a cleared project name and ticker;
- project-owned repository, domain, package namespaces, DNS seeds, and support
  channels;
- a private vulnerability-reporting channel and response policy;
- release signing keys, at least two independent builders, a project-owned
  attestation repository, and reproducible-build verification instructions;
- a versioning, upgrade, rollback, and supported-lifecycle policy;
- a translation workflow that does not reuse unreviewed Bitcoin-branded text;
- independent consensus, wallet, networking, and cryptographic review.

Only after those controls exist should the inherited release automation be
adapted to project-owned URLs and signing repositories and enabled for public
artifacts. The disable markers in `contrib/windeploy`, `contrib/macdeploy`, and
`contrib/guix` must remain in place until that review is complete; removing a
marker alone does not establish trust.
