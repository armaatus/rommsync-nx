# Hardware results

Every run of [HARDWARE_TEST_PLAN.md](HARDWARE_TEST_PLAN.md) on a real console,
one record per run, in one shape -- so a run on one console can be read against
a run on another, and the launch gate (M12-0, #305) can cite a row rather than
a memory.

> **No run is recorded yet.** None may be before `./scripts/v1-gate.sh` exits 0
> (CLAUDE.md, hard rule 1). This page is the template until then.

## How to record a run

1. Copy [the template](#the-template) below to the end of [Runs](#runs) under a
   heading `### Run <YYYY-MM-DD> -- <who>, <console model>`. Never edit an
   earlier run: a later run that disagrees is a new run.
2. Fill in **every** field of the console table. A field you cannot read says
   *unknown* and why; a blank is not a value.
3. Run the scenarios in the plan's order. Each row gets one result:

   | Result | Means |
   |---|---|
   | **PASS** | the scenario's *Pass* criterion held |
   | **FAIL** | it did not; *Evidence* names the log line and the files DBG-01 collected |
   | **BLOCKED** | an earlier failure made it impossible to run; *Notes* names which |
   | **NOT RUN** | skipped on purpose (no exFAT card, no second account...); *Notes* says why |

4. *Evidence* is the log line the console actually wrote, verbatim, and the
   name of the diagnostics bundle (OVL-02) when one was taken. *Notes* holds
   any answer a scenario's **Answers** asks for, and any layout constant that
   had to move.
5. A FAIL becomes an issue of its own, linked from its row. An answer that
   corrects a doc is a change to that doc, linked from its row.

The M12-4 compatibility matrix (#310) and the M12-5 server matrix (#311) add
their own tables to this page when they run; each cell cites the run it came
from.

## The template

### The console

| Field | Value |
|---|---|
| Console model | Erista / Mariko / Lite / OLED |
| HOS | |
| Atmosphère | version and hash |
| emuMMC or sysMMC | |
| SD card filesystem | FAT32 / exFAT, size |
| Ultrahand | Ultrahand Overlay version (or the Tesla menu's) |
| nx-ovlloader | version, and its heap size |
| Co-resident sysmodules | each with its version |
| Build sha | the commit, and `VERSION` as PRE-03 read it off the card |
| RomM | version, and how it is reached (LAN http, https with which CA...) |
| Media and backups | PRE-01: the NAND backup's location, spare card or emuMMC |

### The scenarios

| Scenario | Result | Evidence | Notes |
|---|---|---|---|
| PRE-01 | | | |
| PRE-02 | | | |
| PRE-03 | | | |
| INST-01 | | | |
| INST-02 | | | |
| INST-03 | | | |
| INST-04 | | | |
| INST-05 | | | |
| INST-06 | | | |
| BOOT-01 | | | |
| BOOT-02 | | | |
| BOOT-03 | | | |
| NET-01 | | | |
| NET-02 | | | |
| NET-03 | | | |
| NET-04 | | | |
| NET-05 | | | |
| NET-06 | | | |
| NET-07 | | | |
| NET-08 | | | |
| NET-09 | | | |
| LIFE-01 | | | |
| LIFE-02 | | | |
| LIFE-03 | | | |
| LIFE-04 | | | |
| LIFE-05 | | | |
| LIFE-06 | | | |
| LIFE-07 | | | |
| LIFE-08 | | | |
| LIFE-09 | | | |
| LIFE-10 | | | |
| LIFE-11 | | | |
| SAVE-01 | | | |
| SAVE-02 | | | |
| SAVE-03 | | | |
| SAVE-04 | | | |
| SAVE-05 | | | |
| SAVE-06 | | | |
| SAVE-07 | | | |
| SAVE-08 | | | |
| SAVE-09 | | | |
| SAVE-10 | | | |
| SAVE-11 | | | |
| MEM-01 | | | |
| MEM-02 | | | |
| MEM-03 | | | |
| CLK-01 | | | |
| CLK-02 | | | |
| CLK-03 | | | |
| CLK-04 | | | |
| OVL-01 | | | |
| OVL-02 | | | |
| DBG-01 | | | |
| DBG-02 | | | |

## Runs

No run is recorded yet.
