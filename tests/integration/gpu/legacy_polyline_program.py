"""Original stimulus used to capture the immutable version-93 GPU fixtures."""
from ..guest_program import Program


def build(shaded):
    p = Program(); p.emit(0x40806000, 0, 0)
    p.constant(16, 0x1F801810); p.constant(17, 0x1F801110); p.constant(20, 0xA0030000)

    def store(base, offset, value):
        p.constant(8, value); p.emit(0xAC080000 | base << 21 | offset)

    def gp0(value):
        store(16, 0, value); p.emit(*([0] * 16))

    store(20, 0, 0x504C3933); store(20, 4, 1); store(20, 8, 0)
    store(16, 4, 0); gp0(0xE3000000); gp0(0xE407FFFF)
    gp0(0x580000FF if shaded else 0x480000FF); gp0(32 | 24 << 16)
    if shaded: gp0(0x0000FF00)
    gp0(40 | 24 << 16)
    if shaded: gp0(0x00FF0000)
    gp0(40 | 32 << 16)
    store(17, 4, 0x100); store(17, 0, 0); p.constant(9, 450); p.label('pause')
    p.emit(0x8E280000, 0, 0x0109402B); p.branch(5, 8, 0, 'pause')
    gp0(0x50005000); gp0(0xC0000000); gp0(32 | 24 << 16); gp0(1 | 1 << 16)
    p.emit(0x8E080000, 0, 0xAE880008); store(20, 4, 2)
    p.label('halt'); p.branch(4, 0, 0, 'halt')
    return p.finish()
