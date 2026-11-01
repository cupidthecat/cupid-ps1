"""Original firmware guest that observes both CD identification responses."""
from ..guest_program import Program

RESULT_ADDRESS = 0x30100
PENDING = 0x43445045
COMPLETE = 0x43444944


def build():
    program = Program()
    program.constant(18, 0xA0030000)
    program.constant(16, 0x1F801800)
    program.emit(0xAE500000, 0x8E500000, 0)
    program.constant(17, 0xA0000000 + RESULT_ADDRESS)
    program.emit(0xAE200020)
    program.constant(8, 0xFFFFFFFF)
    program.emit(0xAE280008, 0xAE28000C)

    def write(value, offset):
        program.emit(0x24080000 | value, 0xA2080000 | offset)

    def wait_irq(code, label):
        write(1, 0)
        program.label(label)
        program.emit(0x92080003, 0, 0x31080007, 0x24090000 | code)
        program.branch(5, 8, 9, label)

    # Clear the insertion latch with Getstat before requesting identification.
    write(0, 0); write(1, 1)
    wait_irq(3, 'stat-ack')
    program.emit(0x92080001, 0)
    write(0x1F, 3)
    write(0, 0); write(0x1A, 1)
    wait_irq(3, 'id-ack')
    program.emit(0x92080001, 0, 0xAE280004)
    write(0x1F, 3)
    program.constant(8, PENDING); program.emit(0xAE280000)
    wait_irq(5, 'id-completion')
    for index in range(8):
        program.emit(0x92080001, 0, 0xA2280010 | index)
        if index < 3:
            program.emit(0xAE280000 | (4 + index * 4))
    program.constant(8, COMPLETE); program.emit(0xAE280000)
    program.label('done')
    program.emit(0x8E280020, 0, 0x25080001, 0xAE280020)
    program.branch(4, 0, 0, 'done')
    return program.finish()
