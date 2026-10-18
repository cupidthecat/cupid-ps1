# SPU DMA reads

The SPU transfer FIFO holds 32 halfwords. DMA reads request 32-bit words, so
each requested word needs two FIFO entries. The underflow check compares
the requested halfword count with the available halfwords.

An oversized read consumes the available entries, then repeats the last
consumed halfword for the rest of the block. An empty FIFO supplies zero.
The FIFO stays empty after underflow; its unsigned count must not wrap.
The low halfword appears first in each output word.

The regressions use patterned SPU RAM and cover empty, partial and full
FIFOs, wrapped queue positions, zero-length reads, exact reads and oversized
blocks. They check every output halfword, surrounding memory, queue contents
and indices, transfer address, request flags and DMA completion. The same
checks run after component restoration. Guest programs configure DMA channel
4 through the interpreter, cached interpreter and native slow, mmap and LUT
paths, with native linking enabled and disabled.

These checks cover FIFO accounting and the existing underflow fill rule.
They do not qualify physical transfer timing, audio playback, voice mixing,
or the SPU's complete behavior.
