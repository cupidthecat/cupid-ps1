"""Original guest for DMA prefixes, error flags and unread GPU data on restore."""
from ..guest_program import Program

RESULT = 0x30000
MAGIC = 0x444D4145
LAST = 0x7FFFFC
SENTINEL = 0x44444444
ENABLES = 0x00C40000
# name, channel, address, block control, channel control, GPU readback
CASES = (
    ('read-prefix', 2, LAST, 2, 0x11000000, True),
    ('otc-zero', 6, 0, 2, 0x11000000, False),
    ('otc-four', 6, 4, 3, 0x11000000, False),
    ('otc-invalid', 6, 0x800000, 1, 0x11000000, False),
    ('otc-last', 6, LAST, 1, 0x11000000, False),
    ('write-forward', 2, LAST, 2, 0x11000001, False),
    ('write-reverse', 2, 0, 2, 0x11000003, False),
    ('request-write', 2, LAST, 0x00020002, 0x01000201, False),
    ('request-read', 2, LAST, 0x00020002, 0x01000200, True),
    ('last-header', 2, LAST, 0, 0x01000401, False),
    ('linked-prefix', 2, LAST - 4, 0, 0x01000401, False),
    ('linked-no-payload', 2, LAST, 0, 0x01000401, False),
)


def build():
    p = Program(); p.emit(0x40806000, 0, 0)
    p.constant(16, 0x1F801000); p.constant(17, 0x1F801110)
    p.constant(19, 0x1F801810); p.constant(20, 0xA0000000 | RESULT)

    def store(base, offset, value):
        p.constant(8, value); p.emit(0xAC080000 | base << 21 | offset)

    def capture(base, offset, destination, mask=None):
        p.emit(0x8C080000 | base << 21 | offset, 0)
        if mask is not None:
            p.constant(9, mask); p.emit(0x01094024)
        p.emit(0xAE880000 | destination)

    def gp0(value):
        store(19, 0, value); p.emit(*([0] * 16))

    store(20, 0, MAGIC); store(20, 4, 0); store(20, 8, len(CASES)); store(20, 12, 0)
    for index, (name, channel, address, block, control, read) in enumerate(CASES):
        store(16, 0xF0, 0); store(16, 0xF4, 0x7F000000 | ENABLES)
        store(16, 0x70, 0)
        for pointer in (0, 4, LAST - 4, LAST):
            p.constant(18, 0xA0000000 | (pointer & 0x1FFFFF)); store(18, 0, SENTINEL)
        store(19, 4, 0); store(19, 4, 0x04000003 if read else 0x04000002)
        if read:
            for word in (0xA0000000, 0, 0x00010004, 0x12345678, 0x23456789,
                         0xC0000000, 0, 0x00010004): gp0(word)
        elif channel == 2:
            p.constant(18, 0xA0000000 | (address & 0x1FFFFF))
            store(18, 0, 0x1F000000)
        if name.startswith('linked-') or name == 'last-header':
            p.constant(18, 0xA0000000 | (address & 0x1FFFFF))
            count = 2 if name == 'linked-prefix' else int(name == 'linked-no-payload')
            store(18, 0, count << 24 | 0xFFFFFF)
            if name == 'linked-prefix':
                p.constant(18, 0xA01FFFFC); store(18, 0, 0x1F000000)
        register = 0x80 + channel * 16
        store(16, register, address); store(16, register + 4, block)
        store(16, register + 8, control); store(16, 0xF0, 8 << (channel * 4))
        offset = 16 + index * 40
        for i, source in enumerate((register + 8, register, register + 4, 0xF4)):
            capture(16, source, offset + i * 4)
        for i, pointer in enumerate((0, 4, LAST)):
            p.constant(18, 0xA0000000 | (pointer & 0x1FFFFF)); capture(18, 0, offset + 16 + i * 4)
        capture(19, 4, offset + 28, 1 << 24)
        capture(16, 0x70, offset + 32, 1 << 3)
        if index == 0:
            # Preserve a faulted DMA and the GPU's unread second word at frame 3.
            store(20, 4, 1); store(17, 4, 0x100); store(17, 0, 0)
            p.constant(9, 450); p.label('pause')
            p.emit(0x8E280000, 0, 0x0109402B); p.branch(5, 8, 0, 'pause')
        if read: capture(19, 0, offset + 36)
        else: store(20, offset + 36, 0)
        store(20, 12, index + 1)
    store(20, 4, 2)
    p.label('halt'); p.branch(4, 0, 0, 'halt')
    return p.finish()
