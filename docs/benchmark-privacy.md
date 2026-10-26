# Start-up benchmark reports and your privacy

Anvil Manager's **Benchmark** tab measures how long Europa Universalis IV takes
to start on your computer, without a plugin and with it. When the series ends
you can send its report to the Anvil maintainers, copy it, or do nothing. This
page says what a report contains, where it goes when you click **Send**, how
long it is kept and how to have it deleted.

Anvil is an open-source project of the
[happy-dwarf-team](https://github.com/happy-dwarf-team) maintainers. It is not
affiliated with or endorsed by Paradox Interactive.

## Nothing leaves without your click

- A report is sent only when you click **Send** on the preview screen, after
  the preview has shown you the report's exact bytes. There is no automatic
  sending and no retry.
- **Copy** puts the same bytes on your clipboard. Where you paste them (for
  example a GitHub issue) is your choice, under that site's terms.
- Not clicking either keeps the report on your computer only.

## What a report contains

One JSON document per series, exactly as the preview shows it:

- a random **series id**, new for every series, used only to drop a report
  that is sent twice;
- each start of the series: which arm (without or with the plugin), its
  position, its time in milliseconds from the start of the process to the
  game being ready, whether it counted, the cold-start signals (first start
  since boot, antivirus definitions changed, the time per file of the first
  file opens) and whether the keyboard or mouse was used during it;
- for a crash, only that it crashed, how sure Anvil is about the plugin
  involved, and that plugin's id;
- the state of each plugin in each start and the numbers, options and flags
  the plugins report (never text: a plugin can only report values of kinds
  Anvil checks);
- the result the Manager showed you;
- your computer: the processor model and number of cores, memory in whole GB,
  the kind of drive (NVMe, SATA SSD, HDD, other) of the game folder, the EU4
  user folder and the Workshop folder, the Windows build number, and the names
  of your antivirus products;
- your mods: how many are enabled, the Steam Workshop ids of Workshop mods
  (the preview has a switch to leave them out), how many local mods, and the
  total number of files and size of the mod folders, rounded;
- versions: the game build, Anvil, and every loaded plugin's id, version,
  source (local, or Workshop with its item id) and SHA-256 hash.

A report never contains computer or account names, file paths, drive models or
serial numbers, your graphics card, free disk space, names of local mods,
dates or times of day, log lines, crash reports, free text, or any identifier
of you or your installation that lasts beyond the series.

## Where it goes

- **Send** uploads the report over HTTPS to `bench.happydwarf.dev`, a
  Cloudflare Worker run by the Anvil maintainers on Cloudflare's network.
  Cloudflare processes the request for us as our hosting provider.
- Like any web server, Cloudflare sees your IP address. The receiver uses it
  only to limit how many reports one address can send per minute; the address
  lives in the rate limiter's short-lived counters and is not stored. Request
  logs are switched off: the receiver logs only the response code, the report
  format and its size.
- The receiver stores the report's bytes as sent and adds one thing: the day
  of receipt (a date in UTC, without the time). It does not store your IP
  address, country, browser or Manager identification, or the time of day.
- Reports are kept in a Cloudflare D1 database of the Happy Dwarf account,
  stored in the European Union. Only the account's members, the Anvil
  maintainers, can read them. There is
  no public page of reports or results.

## Why we collect it

To find setups where a plugin does not make the game start faster or makes it
slower (for example a hard disk, many mods or little memory) and to fix them.
The reports are a diagnostic signal for the maintainers; they are not sold or
shared, and they are not used to identify anyone.

## How long it is kept

24 months from the day of receipt. A daily job deletes older reports.

## Deleting your report

E-mail `privacy@happydwarf.dev` with the report's **series id**. You find it on
the preview screen under **Send** and in the report itself, which the Manager
keeps in `anvil/data/anvil/benchmarks/` in your game folder (the last ten
series). A maintainer deletes the report by hand and answers you. The e-mail
address is used only to answer your request.

Please do not ask in a public GitHub issue: it would tie your GitHub account to
the report.

Without the series id we cannot find a report, since nothing in it identifies
you. You can also ask the same address any question about this page.

## Changes

This page lives in the Anvil repository, `docs/benchmark-privacy.md`, and its
history shows every change. The Manager links its copy on
[anvil.happydwarf.dev](https://anvil.happydwarf.dev/docs/benchmark-privacy/),
which follows this file. A change to what a report contains is a new report
format, which the Manager's preview shows before you send it.
