"""Original guest retaining a divided-clock IRQ across repeated GPU gate edges."""
from ..guest_program import Program

RESULT = 0x30000
MAGIC = 0x47415445
TIMER_IRQ = 1 << 6


def build(gated, pal=False):
    p = Program(); p.emit(0x40806000, 0, 0)
    p.constant(16, 0x1F801000); p.constant(17, 0x1F801810)
    p.constant(18, 0x1F801100); p.constant(20, 0xA0000000 | RESULT)

    def store(base, offset, value):
        p.constant(8, value); p.emit(0xAC080000 | base << 21 | offset)

    def vblank(label):
        store(16, 0x70, 0xFFFFFFFE)
        p.label(label); p.emit(0x8E080070, 0, 0x31080001)
        p.branch(4, 8, 0, label)

    store(20, 0, MAGIC); store(20, 4, 1); store(20, 8, int(gated) | int(pal) << 1)
    for offset in (12, 16, 20, 24): store(20, offset, 0)
    store(17, 4, 0); store(17, 4, 0x08000001 | (8 if pal else 0))
    store(18, 4, 0x101 if gated else 0x100)
    vblank('first_frame')
    # Arm partway through this frame, so the next snapshot retains a pending
    # Timer 2 interrupt while Timer 0 keeps receiving blanking gate edges.
    p.constant(10, 160)
    p.emit(0x8E290004, 0, 0x00094FC2)
    p.label('scanline')
    p.emit(0x8E280004, 0, 0x000847C2)
    p.branch(4, 8, 9, 'scanline')
    p.emit(0x01004821, 0x254AFFFF)
    p.branch(5, 10, 0, 'scanline')
    store(16, 0x70, 0xFFFFFFBF)
    store(18, 0x28, 0xFFFF)
    # Discard reached flags from Timer 2's earlier free-running interval.
    p.emit(0x8E480024, 0)
    store(18, 0x24, 0x210)
    store(20, 4, 2)
    vblank('pending_frame'); vblank('completed_frame')
    p.emit(0x8E080070, 0, 0xAE88000C)
    # Reading timer registers or disabling synchronization before this capture
    # could run the delayed shared event and hide the missing interrupt.
    store(18, 4, 0)
    p.emit(0x8E890000, 0, 0xAE890010)
    store(20, 4, 3)
    p.label('halt'); p.branch(4, 0, 0, 'halt')
    return p.finish()
