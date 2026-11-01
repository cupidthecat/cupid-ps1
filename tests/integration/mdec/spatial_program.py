"""Original colored blocks through the CPU data port and partial DMA bursts."""
from ..guest_program import Program

RESULT = 0x30000
PIXELS = 0x31000
MAGIC = 0x53504154
# depth, signed, bit15, DMA, flat rounding control
CASES = [(3, False, False, True, False)]
CASES += [(depth, signed, bit15, dma, False)
          for depth in (3, 2) for signed in (False, True)
          for bit15 in ((False, True) if depth == 3 else (False,)) for dma in (False, True)
          if (depth, signed, bit15, dma) != (3, False, False, True)]
CASES += [(3, False, False, False, True)]


def expected(case, old):
    depth, signed, bit15, dma, flat = case
    result = bytearray()
    luma = [-12] * 4 if flat else [-16, -8, 8, 16]
    for pixel in range(256):
        block = ((pixel // 16) // 8) * 2 + (pixel % 16) // 8 if dma else pixel // 64
        value = (luma[block] + (0 if signed else 128)) & 255
        if depth == 3:
            channel = value >> 3 if old else min((value + 4) >> 3, 31)
            color = channel * 1057 | (0x8000 if bit15 else 0)
            result.extend(color.to_bytes(2, 'little'))
        else: result.extend([value] * 3)
    return bytes(result)


def build():
    p = Program(); p.emit(0x40806000, 0, 0, 0)
    p.constant(16, 0x1F801820); p.constant(19, 0x1F801090)
    p.constant(20, 0x1F801110); p.constant(21, 0xA0000000 | RESULT)

    def store(register, offset, value):
        p.constant(8, value); p.emit(0xAC080000 | register << 21 | offset)

    def write(value):
        store(16, 0, value); p.emit(*([0] * 16))

    def wait_clear(register, offset, mask, label):
        p.constant(9, mask); p.label(label)
        p.emit(0x8C080000 | register << 21 | offset, 0, 0x01094024)
        p.branch(5, 8, 0, label)

    def dma(address, words, label):
        store(19, 0, address)
        store(19, 4, ((words // 4) << 16) | 4)
        store(19, 8, 0x01000200)
        wait_clear(19, 8, 0x01000000, label)

    store(16, 4, 0x80000000); store(16, 4, 0x60000000)
    store(19, 0x60, 0x80)  # DPCR enables channel 1.
    write(0x40000001)
    for _ in range(32): write(0)
    write(0x60000000)
    for _ in range(32): write(0x40004000)
    store(21, 0, MAGIC); store(21, 8, len(CASES))
    for index, (depth, signed, bit15, use_dma, flat) in enumerate(CASES):
        address = PIXELS + index * 0x400
        count = 128 if depth == 3 else 192
        write(0x20000006 | depth << 27 | signed << 26 | bit15 << 25)
        for dc in ((0, 0) + ((-96,) * 4 if flat else (-128, -64, 64, 128))):
            write(0xFE000000 | (dc & 0x3FF))
        wait_clear(16, 4, 0x80000000, f'ready-{index}')
        # Reload a RAM pointer so native LUT accesses are observable.
        store(21, 0x20, 0xA0000000 | address); p.emit(0x8EB10020, 0)
        if use_dma:
            if index == 0:
                dma(address, 4, 'first-dma')
                store(21, 4, 1)
                store(20, 4, 0x100); store(20, 0, 0)
                p.constant(9, 450); p.label('pause')
                p.emit(0x8E880000, 0, 0x0109402B)  # sltu t0,t0,t1
                p.branch(5, 8, 0, 'pause')
                dma(address + 16, count - 4, 'continue-dma')
            else: dma(address, count, f'dma-{index}')
        else:
            p.constant(18, count); p.constant(9, 0x80000000)
            p.label(f'read-{index}')
            p.emit(0x8E080004, 0, 0x01094024); p.branch(5, 8, 0, f'read-{index}')
            p.emit(0x8E0A0000, 0, 0xAE2A0000, 0x26310004, 0x2652FFFF)
            p.branch(5, 18, 0, f'read-{index}')
    store(21, 4, 2)
    p.label('halt'); p.branch(4, 0, 0, 'halt')
    return p.finish()
