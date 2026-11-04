"""Small original MIPS programs and generated firmware/executable inputs."""
import struct


class Program:
    def __init__(self):
        self.words, self.labels, self.fixups = [], {}, []

    def emit(self, *words):
        self.words.extend(words)

    def constant(self, register, value):
        self.emit(0x3C000000 | register << 16 | (value >> 16),
                  0x34000000 | register << 21 | register << 16 | (value & 0xFFFF))

    def label(self, name):
        self.labels[name] = len(self.words)

    def branch(self, opcode, rs, rt, name):
        self.fixups.append((len(self.words), 'branch', name))
        self.emit(opcode << 26 | rs << 21 | rt << 16, 0)

    def call(self, name):
        self.fixups.append((len(self.words), 'call', name))
        self.emit(0x0C000000, 0)

    def finish(self):
        for position, kind, name in self.fixups:
            target = self.labels[name]
            if kind == 'call': self.words[position] |= ((0xBFC00000 + target * 4) >> 2) & 0x3FFFFFF
            else:
                offset = target - position - 1
                assert -32768 <= offset <= 32767
                self.words[position] |= offset & 0xFFFF
        return self.words


def write_inputs(bios, executable, words):
    firmware = bytearray(512 * 1024)
    struct.pack_into('<' + 'I' * len(words), firmware, 0, *words)
    bios.write_bytes(firmware)
    image = bytearray(2048 + len(words) * 4)
    image[:8] = b'PS-X EXE'
    struct.pack_into('<4I', image, 16, 0x80010000, 0, 0x80010000, len(words) * 4)
    struct.pack_into('<' + 'I' * len(words), image, 2048, *words)
    executable.write_bytes(image)
