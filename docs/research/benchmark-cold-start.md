# Making and recognising a cold start without administrator rights

Research note for [issue #129](https://github.com/happy-dwarf-team/anvil/issues/129), part of the
map [#127](https://github.com/happy-dwarf-team/anvil/issues/127) (a start-up benchmark in Anvil that
compares starts without and with Bellows; no administrator rights, no UAC, no system cache flush).
Researched 2026-10-25 on Windows 11 Pro 10.0.26200, 64 GB RAM, NVMe, Microsoft Defender Antivirus
in normal mode with real-time protection on. The game was not run for this note. Measurements of
the game come from the lab repository (`eu4-engine-lab`, `docs/startup-cold-start.md` and
`docs/startup-gui-stall.md`), quoted as the lab recorded them.

Sections: **short answer**, **verified facts** (each with its source: documentation, source code,
or a read-only check or scratch-file experiment on this machine), **inferences** (reasoning from
the facts, not observed), **open questions**.

## Short answer

- **No way exists to make the start cold on demand without administrator rights, at least not in the
  way that matters.** The two system-wide ways to empty the file cache need privileges that a
  non-elevated token does not have: `NtSetSystemInformation` memory list commands need
  `SeProfileSingleProcessPrivilege`, and `SetSystemFileCacheSize` needs `SE_INCREASE_QUOTA_NAME`.
  A per-file trick does work as an ordinary user: opening a file with `FILE_FLAG_NO_BUFFERING` and
  closing it evicts that file's cached data. It costs about 0.04-0.05 ms per small file and about
  0.2 s per GiB.
- **The per-file eviction does not reproduce the lab's "first start after a reboot".** After eviction,
  reading the files was slower (disk instead of RAM), but opening them stayed at ~0.02 ms. After a
  reboot the lab measured ~4.45 ms per first open, independent of file size. That cost was 51 of the
  61 s and it is not file data in the standby list. Evicting the standby list, even with
  administrator rights, would most likely give only the smaller part: the slower reads, ~1.5 s of
  `ReadFile` and a slower background prefetch in the lab. What else the open cost is remains a
  hypothesis: the most likely cause is per-file work by the antivirus filter after a boot. Cold
  directory and MFT metadata is the alternative.
- **The Defender kind of cold start can be detected without elevation, but it cannot be produced.**
  The Defender Operational log grants read access to interactive users, and event 2000 (definitions
  updated) gives the time and the old and new versions. Whether the update is installed is up to
  Defender. On this machine there were 2-3 updates a day.
- **Signals of a cold start that need no elevation:** whether this is the first start since boot (boot time
  from `GetTickCount64`, kept by Anvil between starts); whether the definitions version changed
  since the last start (event 2000, or `MSFT_MpComputerStatus.AntivirusSignatureVersion`); and the
  file-open cost measured in the start itself, the most direct signal. This is the game's first opens
  of existing files in the arm without Bellows, and Bellows' `open_warmup` time and throughput in the
  arm with Bellows. The lab's values are ~100 times apart. How the cost grows with file size
  separates the two kinds of cold start.
- **Consequence for the spec:** cold starts are measured opportunistically, as the map's frame already
  allows: the first start after each boot, plus starts after a definitions update. Each start
  is classified by the measured open cost, with the boot and definitions markers as labels. The
  per-file eviction could produce a separate "cold file data" state, but it must not be called a
  cold start.

## Verified facts

### What the lab measured (from the lab repository, not re-measured here)

1. First start after a reboot (`reboot-cold-ft-1`): 61.03 s to ready, against ~11-14 s warm. First
   open of an existing file averaged **4.45 ms** (11 502 files, 51.2 s in total). A repeated open of
   the same file in the same start took **0.038 ms**. Opens of files that do not exist (6056) took
   81 ms in total. The game's opens are serialised by PhysFS, so the cost lies on the critical path.
2. The cost is per file, not per byte: after that reboot, 38 loading screens (~19 MB each) opened in
   173 ms in total (~4.5 ms each). After a Defender definitions update without a reboot, a loading
   screen took ~1 s to open. The lab therefore calls them two kinds of cold start: after a reboot,
   a fixed cost per first open, and after a definitions update, a cost that grows with file size.
3. Opening each file once and closing it, without reading (Bellows' `open_warmup`, 8 threads), took
   9.1 s cold for 11 525 files (~1260 opens/s) and 71-119 ms warm. With it, the cold start took 15.06-
   16.53 s. What remained of the cold-warm gap was mostly before the application starts (+2.5-3 s:
   loading EXE/DLL images and drivers) and slower content reads with an empty cache (`ReadFile` of
   all threads 2.18 s cold against 0.64 s warm; background prefetch 15.0 s against 9.0 s).
4. CPU time of the process, page faults and bytes read were the same as warm in the Defender case,
   and the NVMe disk did not explain the difference.

### Emptying the file cache system-wide needs privileges an ordinary user lacks

5. `SetSystemFileCacheSize` "limits the size of the working set for the file system cache"; to flush
   the cache, both sizes are set to `(SIZE_T) -1`. "The calling process must enable the
   **SE_INCREASE_QUOTA_NAME** privilege." Source:
   [SetSystemFileCacheSize, Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-setsystemfilecachesize).
6. The standby list commands (`MemoryEmptyWorkingSets`, `MemoryFlushModifiedList`,
   `MemoryPurgeStandbyList`, `MemoryPurgeLowPriorityStandbyList`) go through
   `NtSetSystemInformation(SystemMemoryListInformation, …)`. Microsoft does not document them; the
   phnt headers of System Informer list them and note "requires SeProfileSingleProcessPrivilege".
   Source: [phnt `ntexapi.h`](https://github.com/winsiderss/phnt/blob/master/ntexapi.h)
   (`SYSTEM_MEMORY_LIST_COMMAND`, `SystemMemoryListInformation`). This is a reverse-engineered
   header, not Microsoft documentation. RAMMap's "Empty" menu uses the same commands; its Learn page
   does not describe the menu or its privilege
   ([RAMMap, Sysinternals](https://learn.microsoft.com/en-us/sysinternals/downloads/rammap)).
7. On this machine, `whoami /priv` in a non-elevated session (mandatory label medium, S-1-16-8192)
   lists only `SeShutdownPrivilege`, `SeChangeNotifyPrivilege`, `SeUndockPrivilege`,
   `SeIncreaseWorkingSetPrivilege` and `SeTimeZonePrivilege`. Neither `SeIncreaseQuotaPrivilege`
   nor `SeProfileSingleProcessPrivilege` is present, so neither call can succeed without elevation.
   Neither call was attempted.

### A non-cached open evicts one file's cached data, as an ordinary user

8. Microsoft's FastFAT sample (`filesys/fastfat/create.c`, `FatOpenExistingFcb`): "If this is a
   non-cached open on a non-paging file, and there are no open cached handles, but there is still a
   data section, attempt a flush and purge operation". The code calls `CcFlushCache` and then
   `CcPurgeCacheSection` when `FO_NO_INTERMEDIATE_BUFFERING` is set and every open handle is
   non-cached. Source:
   [Windows-driver-samples, fastfat/create.c](https://github.com/microsoft/Windows-driver-samples/blob/main/filesys/fastfat/create.c).
   NTFS is not open source; fact 9 shows the same behaviour on NTFS.
9. Experiment on this machine (NTFS). Only files the experiment wrote itself, in a temporary folder
   that was deleted afterwards; no game file was touched. 2000 files of 64 KiB and one 1 GiB file
   of random bytes; after a 15 s pause, each was read once to cache it. "Open" is `CreateFileW` with
   `GENERIC_READ`, `FILE_SHARE_READ | FILE_SHARE_WRITE`, `OPEN_EXISTING` and an immediate close (the
   game's parameters); "read" is open, buffered read to the end of the file and close.

   | Step | 2000 × 64 KiB | 1 GiB |
   | --- | ---: | ---: |
   | open, warm | 48.8 ms (0.024 ms/file) | |
   | read, warm | 80.6 ms | 118 ms (~8.7 GB/s) |
   | open with `FILE_FLAG_NO_BUFFERING` and close (the eviction) | 95.1 ms (0.048 ms/file) | 206 ms |
   | open, after the eviction | 44.8 ms (0.022 ms/file) | |
   | read, after the eviction | 357.9 ms (4.4 × warm) | 271 ms (~3.9 GB/s, disk speed) |
   | read again | 76.8 ms | 116 ms |

   A second run with 1000 new files gave the same picture: first open after creation 0.022 ms/file,
   eviction 0.079 ms/file, open afterwards 0.022 ms/file, and read afterwards 161 ms against 36 ms warm.
   The eviction works without elevation and with read-only access, but it makes only reads slower,
   never opens. This experiment cannot separate a cold Defender state from cold data, because new
   files are scanned when they are written.

### Memory priority does not evict

10. Memory priority (`MEMORY_PRIORITY_VERY_LOW` … `MEMORY_PRIORITY_NORMAL`, set with
    `SetProcessInformation` / `SetThreadInformation`) "serves as a hint to the memory manager when it
    trims pages from the working set". It orders trimming and reuse of pages; it does not remove them.
    Source:
    [MEMORY_PRIORITY_INFORMATION, Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-memory_priority_information).

### Defender: what an ordinary user can read

11. Defender's events are in Applications and Services Logs > Microsoft > Windows > Windows Defender >
    Operational. Event 2000 (`MALWAREPROTECTION_SIGNATURE_UPDATED`) carries the current and previous
    definitions version, the type (antivirus or antispyware), Full or Delta, and the engine
    versions; 2002 is an engine update (`MALWAREPROTECTION_ENGINE_UPDATED`) and 5007 a configuration
    change. Source:
    [Microsoft Defender Antivirus event IDs, Microsoft Learn](https://learn.microsoft.com/en-us/defender-endpoint/troubleshoot-microsoft-defender-antivirus).
12. On this machine, the log's security descriptor
    (`Get-WinEvent -ListLog 'Microsoft-Windows-Windows Defender/Operational'`) includes
    `(A;;0x1;;;IU)`: read access for interactive users, and also for batch logons (`S-1-5-3`) and
    Event Log Readers (`S-1-5-32-573`). `Get-WinEvent` read events 2000 with all fields at medium
    integrity. One update writes two 2000 events with the same time (type 1 and type 2). Updates on
    this machine: 23.10 16:14; 24.10 05:31, 15:53, 17:43.
13. `Get-MpComputerStatus` works without elevation (`AMRunningMode`, `RealTimeProtectionEnabled`,
    `AntivirusSignatureVersion`, `AMEngineVersion`). Its `AntivirusSignatureLastUpdated` is **not** the
    installation time: it showed 24.10 10:51:40 for version 1.461.183.0, which event 2000 shows
    installed at 24.10 17:43:21. `Get-MpPreference` refuses to show exclusions without elevation
    ("N/A: Must be an administrator to view exclusions").
14. Microsoft does not document how long Defender's scan results stay valid, nor what invalidates
    them. The searched Learn pages say only that with real-time protection on "files are scanned
    before they are accessed and executed"
    ([full scan considerations](https://learn.microsoft.com/en-us/defender-endpoint/mdav-scan-best-practices)).
    Defender's own tool for scan cost per file, `New-MpPerformanceRecording`, needs an elevated
    session ([performance analyzer reference](https://learn.microsoft.com/en-us/defender-endpoint/performance-analyzer-reference)).

### Time since boot and the kind of boot

15. `GetTickCount64` "retrieves the number of milliseconds that have elapsed since the system was
    started", with 10-16 ms resolution; `QueryUnbiasedInterruptTime` gives the time in the working
    state, without sleep. Source:
    [GetTickCount64, Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-gettickcount64).
    On this machine `now − GetTickCount64()` matched `Win32_OperatingSystem.LastBootUpTime`
    (24.10 15:42:39) and the System log's Kernel-General event 12 (24.10 15:42:40).
16. Fast Startup (Windows 8 and later) is not a cold boot. Windows "closes all applications and logs
    off all user sessions", then "saves the kernel memory image (including the loaded kernel-mode
    drivers) in Hiberfil.sys", and the next start "simply loads the hibernation file". Source:
    [Distinguishing Fast Startup from Wake-from-Hibernation, Microsoft Learn](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/distinguishing-fast-startup-from-wake-from-hibernation).
    A restart always does a full boot.
17. On this machine `HiberbootEnabled` is 1, but `powercfg /a` reports Fast Startup unavailable
    because hibernation is not enabled. All six recent Kernel-Boot event 27 entries in the System log
    show `BootType=0`, and each has a Kernel-General 12 event. The System log is readable without
    elevation. The lab's reboot measurements are therefore full boots. Microsoft's meaning of
    `BootType` values was not found in its documentation.

## Inferences (not observed)

- **The ~4.45 ms is not the standby list.** Four facts point there. The cost does not grow with
  size (fact 2). It hits only existing files, while failed opens stay cheap, so directory lookup is
  not the cost (fact 1). Opening without reading removes it (fact 3). Evicting the data leaves opens
  at 0.02 ms (fact 9). The most likely owner is a file system filter that works on successful opens:
  Defender's real-time protection is the only one known to be active. Its in-memory state about known
  files would be lost or need revalidation after a boot; a definitions update would instead force
  content rescans, which grow with size. The alternative is cold NTFS metadata (MFT records, the
  directory index), which a per-file eviction does not touch. Against it: an NVMe read takes about
  0.1 ms, not 4 ms, and failed lookups in the same directories were cheap. Neither is verified; both
  need a trace of what `CreateFileW` waits on (stack or minifilter timing), which needs
  administrator rights (WPR, `New-MpPerformanceRecording`).
- **Consequence:** emptying the standby list, even with administrator rights (RAMMap, `EmptyStandbyList`),
  would most likely reproduce only the read part of a reboot's cold start: a few seconds, not ~50 s.
  It is not a substitute for a reboot, and the frame already excludes it.
- **The per-file eviction has limits.** It needs no other cached handle on the file (FastFAT
  condition) and does not purge mapped views; EXE/DLL images are separate image sections. It
  gives a "cold file data, warm everything else" state that no player meets by itself. It would also
  evict the player's own cache for these files, which is harmless but not invisible.
- **Memory pressure** (allocating and touching close to the size of RAM) would push standby pages out,
  lowest priority first, but also trims other programs' working sets into the page file. Its
  timing is uncertain (on 64 GB, tens of GB to allocate), and it reproduces, at best, the same read
  part. Not tested; not recommended for players.
- **With Fast Startup** (shutdown followed by power-on on machines with hibernation enabled), kernel
  memory is restored from Hiberfil.sys. A "first start after power-on" may then be warmer than a
  first start after a restart. A boot marker from `GetTickCount64` does not reset across a Fast
  Startup, because the kernel is not restarted. That makes the marker conservative: such a start
  would look like a later start of the same boot. This is not tested; this machine cannot do Fast
  Startup.
- **Classifying a start.** In the arm without Bellows, Anvil can time the game's own first opens of
  existing files (the lab hooked the game's `CreateFileW` import; Anvil supports import
  replacement). The mean first-open cost then separates cold (~4 ms) from warm (~0.04 ms) by
  about two orders of magnitude, and its correlation with file size separates the reboot kind from
  the Defender kind. In the arm with Bellows, the game's opens are fast even when cold, so the signal
  is `open_warmup`'s own wall time and throughput: 9 s against 0.1 s for ~11.5 k files, already
  logged as `open_warmup: …, finished in N ms`. A probe that opens files before the game would warm
  them and change the start it measures, so the timing should stay passive.
- **The definitions-update kind will mix into series.** At 2-3 updates a day, a warm series of
  starts can contain a start after an update. Recording the definitions version (or the newest event
  2000) at every start, and marking the start after a change, keeps that out of the warm group.
  On machines without Defender as active antivirus (`AMRunningMode` other than Normal), events 2000
  do not describe the active scanner, and only the measured open cost remains.

## Open questions

- What `CreateFileW` waits for on the first open after a boot (Defender's filter or NTFS metadata):
  this needs a stack trace or Defender's performance recording, both of which need elevation.
- Whether a Fast Startup gives a cold or warm start, and whether `GetTickCount64` resets across it,
  on a machine with hibernation enabled.
- Whether `Update-MpSignature` works without elevation; this was not tried, because it changes
  system state and the frame does not produce cold starts.
- How the open cost looks with other antivirus products and on HDDs (the map's diagnostic goal).
