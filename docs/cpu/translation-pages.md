# CPU translation pages

The code cache bounds instruction analysis to one host protection page for ROM
and manually checked mappings, as it already does for protected RAM. A branch
at the end of a page keeps its delay slot in the same block. RAM page-list and
invalidation flags retain their existing handling.

This bounds host compilation work. It does not limit guest execution, change
instruction costs, defer events or change PS1 cache-line sizes. Execution
continues through the next translated block. Native instruction-fetch checks,
load delays and arithmetic deadlines remain active across that transition.

Long ROM sequences can contain frequent device accesses and event boundaries.
An event may return to the dispatcher in the middle of a translated block. If
the next translation analyzes the entire remaining sequence again, its unused
suffix consumes compilation time and near/far code space repeatedly. Page
boundaries keep that work bounded while preserving the instruction stream.

## Regression coverage

The `BiosPageBoundary` filter checks cached and uncached BIOS addresses under
the interpreter, cached interpreter, and linked/unlinked native execution with
fast memory disabled, LUT and mmap. It checks a delayed load across the page,
taken and untaken branches with a delay slot across the edge, and a fault in
that slot. The fault retains its exception code, branch-delay bit, EPC and bad
address. Native cases require actual native progress and permit only counted
dispatcher delay-slot interpretation.
An otherwise zero-filled ROM also has to execute its NOP stream normally;
translation cannot depend on finding a branch somewhere later in the image.

```sh
make test TEST_FILTER=BiosPageBoundary TEST_JOBS=4
```

The existing DMA-priority guest retains its complete 128-case instruction
stream, original time limit and exact reset-before-restore comparisons. No
cases, state fields or assertions are removed to accommodate compilation cost.
This change uses the existing save-state layout.

These checks establish selected instruction and saved-continuation behavior.
They do not establish complete hardware timing or gameplay performance.
