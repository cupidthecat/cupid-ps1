"""Original MIPS no-function status, table sequencing and output checks."""
from ..guest_program import Program

RESULT_ADDRESS = 0x30040
RESULT_MAGIC = 0x4D444543


def build():
    program = Program()
    program.constant(16, 0x1F801820); program.constant(18, 0xA0030000)
    program.emit(0xAE500000, 0x8E500000, 0)
    program.constant(17, 0xA0000000 + RESULT_ADDRESS)
    check_number = 0

    def write(value, offset=0):
        program.constant(8, value); program.emit(0xAE080000 | offset)

    def expect_status(value):
        nonlocal check_number
        check_number += 1
        program.constant(21, check_number)
        program.constant(9, value); program.emit(0x8E080004, 0, 0x01095026)
        program.branch(5, 10, 0, 'failed')

    write(0x80000000, 4); write(0x60000000, 4)
    for command in (0, 4, 5, 6, 7):
        for flags in (0, 15):
            for count in (0, 1, 0xFFFF):
                write(command << 29 | flags << 25 | count)
                expect_status(0x90040000 | flags << 23 | count)
    write(0x40000000); expect_status(0xB004000F)
    for _ in range(16): write(0x01010101)
    expect_status(0x9004FFFF)
    write(0x60000000)
    for _ in range(32): write(0)
    expect_status(0x9004FFFF)
    write(0x28000001); write(0xFE000000)
    program.label('wait-output')
    program.emit(0x8E080004, 0, 0x000857C2)
    program.branch(5, 10, 0, 'wait-output')
    expect_status(0x1A04FFFF)
    for command in (0, 4, 5, 6, 7):
        write(command << 29 | 11 << 25 | 0xFFFF)
        expect_status(0x1D84FFFF)
    program.constant(9, 0x80808080)
    for word in range(16):
        program.emit(0x8E080000, 0, 0x01095026)
        program.branch(5, 10, 0, 'failed')
        if word in (0, 15): program.emit(0xAE280008 | (4 if word else 0))
    expect_status(0x9584FFFF); program.emit(0xAE280004)
    write(0x60000000)
    for _ in range(32): write(0x7FFF7FFF)
    write(0x2C000001); write(0xFE0007E0)
    program.label('wait-signed-output')
    program.emit(0x8E080004, 0, 0x000857C2)
    program.branch(5, 10, 0, 'wait-signed-output')
    expect_status(0x1B04FFFF)
    program.constant(9, 0xF8F8F8F8)
    for word in range(16):
        program.emit(0x8E080000, 0, 0x01095026)
        program.branch(5, 10, 0, 'failed')
        if word == 15: program.emit(0xAE280014)
    expect_status(0x9304FFFF)
    program.constant(8, 30); program.emit(0xAE280010)
    program.constant(8, RESULT_MAGIC); program.emit(0xAE280000)
    program.label('done'); program.emit(0x8E280004, 0, 0xAE280018)
    program.branch(4, 0, 0, 'done')
    program.label('failed'); program.emit(0xAE280004, 0xAE290008, 0xAE2A000C, 0xAE350010)
    program.constant(8, 0xBAD0DEAD); program.emit(0xAE280000)
    program.label('failed-halt'); program.branch(4, 0, 0, 'failed-halt')
    return program.finish()
