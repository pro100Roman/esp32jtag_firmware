# Software and Flash Breakpoints

Date: 2026-11-26

Status: proposed future work; not implemented

## Goal

Add target-managed software breakpoints to the ESP32JTAG firmware so its
embedded Black Magic GDB server can replace an instruction with a stop
instruction and restore the original instruction when the breakpoint is
removed.

The initial target should be Arm Cortex-M devices, with flash breakpoints first
implemented and validated for the nRF51/nRF52 family.

This feature belongs to the ESP32JTAG firmware. It is not currently required
by WaveCraft.

## SEGGER J-Link Reference Behavior

SEGGER documents two relevant breakpoint types:

- a software breakpoint in RAM replaces the instruction at the breakpoint
  address with a special breakpoint value
- a software breakpoint in flash requires reprogramming flash to replace and
  later restore the instruction

J-Link calls the second mechanism Flash Breakpoints or Unlimited Flash
Breakpoints. The implementation is primarily managed by the host-side J-Link
DLL, which knows the selected target, memory map, and flash loader. The J-Link
probe provides the underlying debug transport and target access.

The J-Link implementation reduces flash operations by:

- caching the original program contents
- delaying flash programming until it is necessary, commonly before resuming
  execution
- applying multiple breakpoints in one flash sector with one sector update
- combining software and hardware breakpoints
- using instruction-set simulation where possible

SEGGER documents the flash programming sequence as compare, blank check,
sector erase, program, verify, and restore. It also warns that flash
breakpoints can temporarily use target RAM and that DMA can continue operating
while the CPU is halted.

Official references:

- [J-Link User Guide: Flash breakpoints](https://kb.segger.com/J-Link_settings_file)
- [J-Link flash programming](https://kb.segger.com/J-Link_flash_programming)
- [Unlimited Number of Breakpoints in Flash](https://kb.segger.com/Unlimited_Number_of_Breakpoints_in_Flash)
- [J-Link Control Panel breakpoint types](https://kb.segger.com/J-Link_Control_Panel)
- [J-Link Commander `SetBP` and `ClearBP`](https://kb.segger.com/J-Link_Commander)

## Current ESP32JTAG Behavior

The embedded Black Magic GDB server already parses the remote protocol `Z` and
`z` breakpoint/watchpoint packets in
`components/blackmagic_esp32/src/gdb_main.c`.

The Cortex-M backend currently implements:

- hardware instruction breakpoints through the FPB unit
- read, write, and access watchpoints through DWT

It does not implement `TARGET_BREAK_SOFT`. The Cortex-M break/watch handler
returns unsupported for that type. Consequently, the firmware does not own a
software-breakpoint lifecycle or a flash-breakpoint cache.

The existing GDB memory map reports RAM and flash separately. GDB may therefore
insert ordinary software breakpoints into writable RAM itself, while using
hardware breakpoints for read-only flash. That client-managed RAM behavior is
different from implementing `Z0` inside the firmware.

The nRF51/nRF52 target driver already provides most required flash primitives:

- nRF52 flash page size detection and a 4 KiB family default
- flash and RAM memory-map registration
- NVMC write and erase preparation
- page erase
- flash write
- completion and return to read-only mode

The nRF52832 NVMC can write only full aligned 32-bit words. Programming can
change erased bits from `1` to `0`, but restoring a bit from `0` to `1`
requires erasing the complete flash page. A flash breakpoint must therefore
preserve and rebuild the whole page rather than only write the breakpoint
halfword.

Nordic reference:

- [nRF52832 NVMC](https://docs.nordicsemi.com/r/bundle/ps_nrf52832/page/nvmc.html)

## Architecture Boundary

Implement the feature in the Black Magic GDB and target layers, not in the
CMSIS-DAP USB or TCP command engine.

CMSIS-DAP is a target-independent transport. It carries low-level DP/AP, SWD,
and JTAG operations and does not know:

- the selected target family
- which address ranges are RAM or flash
- flash page geometry
- the instruction that was replaced
- when the debugger is stepping over a breakpoint
- when a firmware download invalidates cached program contents

For a CMSIS-DAP session, the host debugger remains responsible for breakpoint
policy. The firmware-managed feature applies to the ESP32JTAG Black Magic GDB
server, where target and flash-driver information is available.

## Proposed RAM Breakpoint Flow

For a Cortex-M RAM breakpoint:

1. Halt the core if required by the active debug policy.
2. Validate the breakpoint address and Thumb halfword alignment.
3. Read and retain the original instruction halfword.
4. Write `BKPT #0`, encoded as `0xBE00`.
5. Verify the patched halfword.
6. When the breakpoint is removed, restore and verify the original halfword.

Resuming from a hit software breakpoint requires a step-over lifecycle:

1. restore the original instruction
2. single-step the core once
3. reinsert the breakpoint if it remains active
4. resume normal execution

Memory reads exposed to the debugger should remain coherent with the original
program contents. Writes overlapping an active breakpoint must update the
saved original bytes or reject the operation with an actionable error.

## Proposed Flash Breakpoint Flow

Use a page-oriented breakpoint manager above the target flash-driver API.

For the first breakpoint in a flash page:

1. Locate the containing `target_flash_s` region.
2. Read and retain a canonical, unmodified copy of the entire page.
3. Apply all active breakpoint patches for that page to a working copy.
4. Halt the target.
5. Erase the page through the target flash driver.
6. Program the patched page.
7. Verify the programmed contents.

When a breakpoint is removed, rebuild the working copy from the canonical page
and apply the remaining breakpoints. When the final breakpoint is removed,
program the canonical page without patches.

Flash updates should be deferred until immediately before execution resumes.
This permits multiple breakpoint changes in one page to be committed with a
single erase/program cycle.

Never treat a page read after breakpoint insertion as the canonical original.
The manager must retain the unmodified page until all breakpoints in that page
have been restored.

## Suggested Data Model

Each managed breakpoint should retain at least:

- address
- breakpoint kind: RAM or flash
- original instruction halfword
- enabled state or reference count
- containing flash region and page address, when applicable

Each modified flash page should retain:

- target identity
- flash region identity
- page address and page size
- canonical original bytes
- active breakpoint addresses
- dirty and programmed state
- checksums for the canonical and patched copies

The first implementation may retain page backups in ESP32 RAM or PSRAM for the
duration of the GDB session. A later design should consider a recovery journal
if restoration after probe reset or power loss is required.

## Lifecycle Requirements

Breakpoint state must be considered during:

- `Z0` insertion and `z0` removal
- continue and single-step
- target reset
- GDB detach and kill
- target detach or replacement
- flash download
- raw memory writes overlapping a patched address or page
- unexpected client disconnect

Clean detach, reset, and kill paths should attempt to restore every modified
instruction and flash page. Restoration failure must be reported and must not
silently discard the cached original data.

An explicit recovery monitor command should be considered, for example:

```text
monitor restore_flash_breakpoints
```

## Risks and Constraints

### Flash endurance

Every set/clear cycle may require erasing a complete page. Deferred commits and
page-level batching are required to avoid unnecessary wear.

### Interrupted programming

Power loss, probe reset, or transport loss during page erase/program can leave
the target page incomplete or leave breakpoint instructions in flash. A RAM-
only cache cannot guarantee recovery after the ESP32JTAG probe itself resets.

### Target timing

Flash programming halts or stalls target execution. It will disrupt real-time
radio and SoftDevice timing. This is expected for an intrusive debug feature
and must remain separate from non-halting trace workflows.

### DMA and peripherals

Halting the CPU does not necessarily halt DMA or all peripherals. A future
RAM-based flash loader must reserve work RAM carefully and either preserve its
contents or document target-specific restrictions.

### Cache coherency

Targets with instruction caches require appropriate cache invalidation after
patching or restoring instructions.

### General target support

RAM breakpoints can be mostly architecture-specific. Flash breakpoints are
target-specific because page geometry, protection, erase, write, verification,
and work-RAM requirements differ by MCU family.

## Proposed Milestones

### Milestone 1: Cortex-M RAM breakpoints

- implement `TARGET_BREAK_SOFT` for writable RAM
- add set, hit, clear, and step-over tests
- preserve original memory in debugger reads
- validate through the ESP32JTAG Black Magic TCP GDB server

### Milestone 2: nRF51/nRF52 flash breakpoints

- add the page cache and deferred commit manager
- use the existing nRF51/nRF52 flash callbacks
- support multiple breakpoints in one page
- verify exact page restoration
- add explicit restore-all handling

### Milestone 3: lifecycle hardening

- restore on detach, kill, reset, and clean disconnect
- detect overlapping downloads and memory writes
- add checksums and failure recovery
- evaluate persistent recovery metadata

### Milestone 4: additional targets

- separate the generic page manager from family-specific flash behavior
- add support only for target drivers with validated erase/write/verify paths
- document target-specific limitations and hardware validation

## Validation Scenarios

At minimum, hardware validation should cover:

- setting and clearing one RAM breakpoint restores the exact original bytes
- hitting and continuing past a RAM breakpoint executes the replaced instruction
- setting two flash breakpoints in one page performs one deferred page update
- clearing one flash breakpoint keeps the other active
- clearing the final breakpoint restores the complete original page
- breakpoints at the first and last valid halfword of a page
- rejection of invalid, protected, or unaligned addresses
- reset and clean detach with active breakpoints
- interrupted or failed erase/program with an actionable recovery result
- firmware download while breakpoint state exists
- instruction-cache invalidation where required

## Decision

Defer implementation until firmware-managed GDB breakpoints become a product
priority.

When work starts, keep the first change limited to Cortex-M RAM breakpoints.
Add nRF51/nRF52 flash breakpoints only after the RAM lifecycle and GDB protocol
behavior are validated. Do not add target-specific breakpoint behavior to the
CMSIS-DAP transport.
