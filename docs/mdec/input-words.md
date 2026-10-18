# MDEC input words

The MDEC command register and DMA input accept 32-bit words. The decoder
consumes the queued data as halfwords, with the low halfword first. An RLE
block can end between the two halves of a word, leaving an odd number of
free halfword slots in the modeled queue.

DMA input limits the accepted word count to the number of complete pairs
that fit. It then appends both halves of each accepted word. A manual
command-register write also needs two free halfword slots; otherwise it
leaves the queue unchanged. An input word cannot overwrite older queued
data or contribute only one halfword.

For example, a 33-word DMA block needs 66 halfword slots. The earlier
capacity mask rejected the last word when 66 slots were free and accepted
only 65 halfwords when 67 were free. The DMA controller still completed the
block and advanced its source address. Counting available words directly
preserves the complete submitted stream in both cases.

The `MdecInput` C-test filter covers DMA input and manual word writes. It
uses real commands and RLE data to hold a decoded block pending output,
then fills the input queue through MMIO. The cases vary free capacity,
transfer length, payload patterns and decoder routines. They check every
live queued halfword, append order, head and tail positions, and continuation
after restoring CPU, decoder, DMA and scheduler state. Raw status is logged.

Generated guest programs perform manual writes or configure DMA channel 0
through its registers. Source data and transfer parameters come from RAM.
The cases cover the interpreter, cached interpreter and native slow, mmap
and LUT paths, including disabled native block linking. Completion markers,
DMA registers and execution statistics verify that the selected path ran.

These checks use generated data and require no private BIOS or disc. They
cover input-word integrity in the current queue model. Physical FIFO size,
transfer timing, output-FIFO behavior and full RLE, IDCT and color-conversion
accuracy remain unqualified. No new console capture is available. The
[MDEC register documentation](https://psx-spx.consoledev.net/macroblockdecodermdec/)
describes the word interface and DMA controls.
