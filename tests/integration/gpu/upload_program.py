"""Original guest checks for size masking and upload data across reset/restore."""
from ..guest_program import Program

RESULT = 0x30000
MAGIC = 0x55504C44
SIZES = (0xFFFFFFFF, 0x01FF03FF)
PIXELS = (0x1F005678, 0x12345678)


def build():
    p = Program(); p.emit(0x40806000, 0, 0)
    p.constant(16, 0x1F801810); p.constant(17, 0x1F801110)
    p.constant(20, 0xA0000000 | RESULT)

    def store(base, offset, value):
        p.constant(8, value); p.emit(0xAC080000 | base << 21 | offset)

    def gp0(value):
        store(16, 0, value); p.emit(*([0] * 16))

    store(20, 0, MAGIC); store(20, 4, 0); store(20, 8, len(SIZES)); store(20, 12, 0)
    p.emit(0x8E880000, 0, 0xAE880000)  # Read and retain the result header through RAM.
    store(16, 4, 0)
    for index, size in enumerate(SIZES):
        store(16, 4, 0x02000000)
        gp0(0xBFFFFFFF); gp0(1022 | 510 << 16); gp0(size)
        for word in PIXELS: gp0(word)
        if index == 0:
            store(20, 4, 1)
            store(17, 4, 0x100); store(17, 0, 0)
            p.constant(9, 450); p.label('pause')
            p.emit(0x8E280000, 0, 0x0109402B); p.branch(5, 8, 0, 'pause')
        p.emit(0x8E080004, 0, 0xAE880000 | (16 + index * 16))
        # Aborting applies the received prefix and releases the parser.
        store(16, 4, 0x01000000)
        gp0(0xC0000000); gp0(1022 | 510 << 16); gp0(4 | 1 << 16)
        for offset in range(2):
            p.emit(0x8E080000, 0, 0xAE880000 | (20 + index * 16 + offset * 4))
        gp0(0x1F000000)
        p.emit(0x8E080004, 0, 0xAE880000 | (28 + index * 16))
        store(20, 12, index + 1)
    store(20, 4, 2)
    p.label('halt'); p.branch(4, 0, 0, 'halt')
    return p.finish()
