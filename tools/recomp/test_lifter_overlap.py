import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


class OverlappingDecodeTest(unittest.TestCase):
    def test_instruction_inside_the_previous_one_is_dropped(self):
        # MSVC memcpy UnwindUp7 (0x373AF0): the real "mov [edi+ecx*4-1Ch], eax"
        # at +4 is four bytes long; a stray decode one byte in produced
        # "inc esp" at +5, which shifted the stack on every 28-byte copy.
        load = Instruction(0, 4, "mov", "eax, dword ptr [esi + ecx*4 - 0x1c]", "8b448ee4")
        load.operands = [Operand(type="reg", reg="eax"),
                         Operand(type="mem", mem_base="esi", mem_index="ecx", mem_scale=4,
                                 mem_disp=-0x1C, mem_size=4)]
        store = Instruction(4, 4, "mov", "dword ptr [edi + ecx*4 - 0x1c], eax", "89448fe4")
        store.operands = [Operand(type="mem", mem_base="edi", mem_index="ecx", mem_scale=4,
                                  mem_disp=-0x1C, mem_size=4),
                          Operand(type="reg", reg="eax")]
        stray = Instruction(5, 1, "inc", "esp", "44")
        stray.operands = [Operand(type="reg", reg="esp")]
        lifter = Lifter()
        lifter.func_start = 0
        lifter.func_end = 0x20

        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[load, store, stray]))
        generated = "\n".join(lifted)

        self.assertNotIn("esp++", generated)
        self.assertIn("skipped 0x00000005", generated)
        self.assertIn("MEM32(edi + ecx * 4 + -28) = eax;", generated)


if __name__ == "__main__":
    unittest.main()
