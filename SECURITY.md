# Security policy

Anvil is installed as `version.dll` next to `eu4.exe` and runs inside the game
process, so a flaw in it can affect everyone who installs it. Please report
security problems privately, not in public issues.

## Reporting a vulnerability

Use GitHub's private vulnerability reporting:
[Report a vulnerability](https://github.com/happy-dwarf-team/anvil/security/advisories/new).
Include the Anvil version (first line of `anvil/anvil.log`), how to reproduce
the problem, and what an attacker gains.

This is a volunteer project; reports are handled on a best-effort basis. We
will confirm the report, agree on a fix and a disclosure date with you, and
credit you in the advisory unless you prefer otherwise.

## Supported versions

Only the latest release receives security fixes. Builds from `main` and older
releases are not supported.

## In scope

- The `version.dll` proxy and loader: memory safety, the entry-point hook,
  forwarding to the system `version.dll` (e.g. loading a DLL from an
  unexpected location).
- Plugin discovery and loading: `plugin.toml` parsing, paths outside the plugin
  folder, plugins loaded despite being disabled, skipped or failing their
  `[[expect]]` checks.
- The patch registry: writes outside the game image, missed overlap or
  byte checks.
- Release integrity: release assets that do not match `SHA256SUMS.txt` or
  their build provenance attestation.

## Out of scope

- What a plugin does once it is loaded. Plugins are native code with the
  game's permissions by design; install only plugins you trust.
- Vulnerabilities in Europa Universalis IV itself; report those to Paradox
  Interactive.
- Antivirus detections of `version.dll` (proxy DLLs are a known heuristic
  pattern); open a regular issue with the detection name instead.
