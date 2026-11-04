"""Original ADPCM/noise voices with sweeps, capture RAM and reverb histories."""
from ..guest_program import Program

RESULT = 0x30000
MAGIC = 0x53505553
BLOCK = bytes.fromhex('24 07 12 34 56 78 9a bc de f0 13 57 9b df 24 68')


def build(pal=False):
    p = Program(); p.emit(0x40806000, 0, 0)
    p.constant(16, 0x1F801C00); p.constant(18, 0xA0000000 | RESULT)
    p.constant(19, 0x1F801810)

    def store16(offset, value):
        p.constant(8, value); p.emit(0xA6080000 | offset)

    def store32(base, offset, value):
        p.constant(8, value); p.emit(0xAC080000 | base << 21 | offset)

    store32(19, 4, 0); store32(19, 4, 0x08000001 | (8 if pal else 0))
    store32(18, 0, MAGIC); store32(18, 4, int(pal)); store32(18, 8, 0)
    store32(18, 20, 0xA0030040); store32(18, 64, 0x13579BDF)
    store16(0x1A6, 0x200); store16(0x1AC, 4); store16(0x1AA, 0xC010)
    for i in range(0, len(BLOCK), 2): store16(0x1A8, int.from_bytes(BLOCK[i:i + 2], 'little'))
    p.label('uploaded'); p.emit(0x960801AE, 0, 0x31080400)
    p.branch(5, 8, 0, 'uploaded')
    for voice, pitch in ((1, 0x900), (3, 0x1700)):
        for register, value in ((0, 0x3FFF), (2, 0x3FFF), (4, pitch), (6, 0x200), (8, 0xF), (10, 0)):
            store16(voice * 16 + register, value)
    for register, value in ((0x180, 0xC050), (0x182, 0x3FFF), (0x184, 0x2000), (0x186, 0x2000),
                            (0x194, 8), (0x198, 0xA), (0x1A2, 0x8000 // 8), (0x1C4, 0x4000),
                            (0x1D4, 0x100), (0x1D6, 0x200), (0x1FC, 0x4000), (0x1FE, 0x4000),
                            (0x1AA, 0xFF80), (0x188, 0xA)):
        store16(register, value)
    # Dynamic pointers require actual CPU memory paths in the repeating loop.
    p.emit(0x8E510014, 0)
    p.label('loop'); p.emit(0x8E280000, 0, 0x25080001, 0xAE280000, 0xAE480008,
                           0x960901AE, 0, 0xAE49000C)
    p.branch(4, 0, 0, 'loop')
    return p.finish()
