"""Comparison snapshots must survive a join of different CMP operands."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator, _merge_flag_states
from tools.recomp.disasm import Operand

BASE = 0x10000

def translate_join(consumer=bytes.fromhex('0f95c0c3')):
    # test ecx,ecx; jz alternate; cmp eax,edx; jmp join; nop;
    # alternate: cmp ebx,esi; join: consumer.
    image = bytes.fromhex('85c9740539d0eb039039f3') + consumer
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                              entry_point=BASE, kernel_thunk_addr=BASE,
                              origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])

def test_different_cmp_operands_join_for_setne():
    code = translate_join()
    assert 'CMP_NE(_fa, _fb)' in code, code
    assert 'RECOMP_FLAGS_FALLBACK' not in code

def test_different_cmp_operands_join_for_cmovne():
    code = translate_join(bytes.fromhex('0f45c7c3'))
    assert 'if (CMP_NE(_fa, _fb)) eax = edi;' in code, code

def test_unknown_path_is_not_guessed():
    assert _merge_flag_states([None, ('cmp', [])]) is None

def test_mixed_operations_are_not_guessed():
    a = Operand(type='reg', reg='eax')
    b = Operand(type='reg', reg='edx')
    assert _merge_flag_states([('cmp', [a, b]), ('test', [a, b])]) is None

def test_mixed_widths_merge_but_are_tagged():
    wide = [Operand(type='reg', reg='eax'), Operand(type='reg', reg='edx')]
    narrow = [Operand(type='reg', reg='al'), Operand(type='reg', reg='dl')]
    kind, ops = _merge_flag_states([('cmp', wide), ('cmp', narrow)])
    assert kind == 'cmp' and getattr(ops, 'mixed_width', False)

def test_mixed_width_test_join_for_jne():
    # test ecx,ecx; jz alternate; test eax,eax; jmp join; nop;
    # alternate: test ah,41h; join: jne +1; nop; ret  (NFL 2K5 0xF4AFB)
    image = bytes.fromhex('85c9740585c0eb0490f6c441') + bytes.fromhex('750190c3')
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image), '_addr': BASE, 'size': len(image)}}
    code = FunctionTranslator(image, db).translate_function(BASE, db[BASE])
    assert 'if (TEST_NZ(_fa, _fb))' in code, code
    assert 'RECOMP_FLAGS_FALLBACK' not in code, code

def test_mixed_width_cmp_join_refuses_sign():
    # cmp eax,edx / cmp al,dl joined, then js: SF needs the width.
    image = bytes.fromhex('85c9740539d0eb039038d0') + bytes.fromhex('780190c3')
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image), '_addr': BASE, 'size': len(image)}}
    code = FunctionTranslator(image, db).translate_function(BASE, db[BASE])
    assert 'RECOMP_FLAGS_FALLBACK' in code and '/* js' in code, code

def test_loop_decrements_ecx_and_branches():
    # mov ecx,3; L: inc eax; loop L; ret  (NFL 2K5's MMX memcpy at 0x3CEE8F
    # ends in `loop`; lifted as a flag jcc it never looped)
    image = bytes.fromhex('b903000000') + bytes.fromhex('40e2fdc3')
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image), '_addr': BASE, 'size': len(image)}}
    code = FunctionTranslator(image, db).translate_function(BASE, db[BASE])
    assert 'ecx--; /* loop */' in code, code
    assert 'if (ecx != 0) goto loc_' in code, code
    assert 'RECOMP_FLAGS_FALLBACK' not in code, code

def test_rcr_rotates_through_carry():
    # shr ecx,1; rcr ebx,1; ret  (MSVC _aullrem normalisation, NFL 2K5 0x41A5DF)
    image = bytes.fromhex('d1e9d1dbc3')
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image), '_addr': BASE, 'size': len(image)}}
    code = FunctionTranslator(image, db).translate_function(BASE, db[BASE])
    assert '/* rcr */' in code and '_cf = (int)' in code, code
    assert 'RECOMP_TODO' not in code, code
