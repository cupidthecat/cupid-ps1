# Memory mapping

The normal RAM window occupies the first 8 MB of KUSEG, KSEG0 and KSEG1.
With the default 2 MB backing, that window contains four mirrors. The optional
8 MB backing uses distinct data throughout the window. KUSEG and KSEG0 support
instruction caching; KSEG1 bypasses it. See the
[hardware memory map](https://psx-spx.consoledev.net/memorymap/).

The bus keeps RAM backing size separate from the mapped window. Selecting a
2 MB window leaves the remaining 6 MB unmapped. Host mappings follow that
window and reject addresses outside the supported segments.

## Host access paths

Slow accesses use the bus handlers. The LUT contains a host address bias for
each 4 KB guest page, with unmapped and isolated entries leading to fault
addresses. The x86-64 backend loads the page's bias and adds the original guest
address. Recovery retains that guest address for the bus handler. A protected
canonical RAM write invalidates the affected host blocks before retrying it.

Mmap fastmem reserves the guest address space and maps the RAM aliases into it.
Both mapping layouts use 2 MB chunks with the correct backing offset. This
keeps mirrors within the 8 MB window and leaves later addresses unmapped,
including when the backing size is 8 MB.

Code-page write protection covers the main RAM mapping and every active mmap
alias of that physical page. A write through any alias invalidates the affected
host blocks before retrying the store. Clearing code-page flags restores write
access. Remapping retains protection for pages that still contain code.

RAM writes preserve emulated instruction-cache data. Host block invalidation
therefore uses the current fetch view: cached execution can retain old code,
while an invalidated RAM block must be decoded again. See
[CPU execution](../cpu/execution.md#reserved-instructions-and-retained-fetches).

## Checks and limits

The `FastmemMapping` C tests inspect Linux mapping permissions, protection across
remaps, configured 8 MB backing offsets and unmapped holes. CPU tests also execute
writes beneath cached and uncached code, including through mmap fastmem.
The mapping checks establish agreement with the configured bus model. They do
not qualify physical 8 MB hardware, complete memory-control behavior or bus timing.
See [device bus accesses](device-bus.md) for transfer widths, address stepping
and the remaining external-device limits.
