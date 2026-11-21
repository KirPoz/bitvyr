"""Регрессии настоящего offline decoder и общих границ probe/input movie."""
import contextlib
import io
from pathlib import Path
import sys
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import device_probe as probe
import input_movie as movie
import log_text


class LogTextTests(unittest.TestCase):
    """Проверяет внешние byte fixtures и точный ожидаемый текст, не старый verifier schema."""

    def check_decode(self,raw,expected):
        """Сверяет BOM/replace/CRLF контракт decoder с независимым literal результатом."""
        self.assertEqual(log_text.decode_text(raw),expected)

    def test_plain_utf8_keeps_crlf(self):
        """Обычный UTF8 сохраняет кириллицу и точный CRLF."""
        self.check_decode('Тест\r\n'.encode('utf8'),'Тест\r\n')

    def test_utf8_bom(self):
        """BOM снимается при декодировании; caller-owned bytes не перезаписываются."""
        self.check_decode(b'\xef\xbb\xbf'+'Тест\n'.encode('utf8'),'Тест\n')

    def test_utf16_little_endian_bom(self):
        """LE BOM выбирает UTF16 и сохраняет CR/LF."""
        self.check_decode(b'\xff\xfe'+'Тест\r\n'.encode('utf-16-le'),'Тест\r\n')

    def test_utf16_big_endian_bom(self):
        """BE BOM не интерпретируется как LE или UTF8."""
        self.check_decode(b'\xfe\xff'+'Тест\n'.encode('utf-16-be'),'Тест\n')

    def test_invalid_utf8_replacement_is_unchanged(self):
        """Невалидный byte даёт U+FFFD по существующей offline политике."""
        self.check_decode(b'x\xff\r\n','x\ufffd\r\n')

    def test_truncated_utf16_replacement_is_unchanged(self):
        """Неполная UTF16 единица даёт U+FFFD, а не исчезает."""
        self.check_decode(b'\xff\xfeA\x00B','A\ufffd')

    def test_ansi_is_separate_from_decoding(self):
        """Decoder сохраняет ANSI, а общий regex снимает CSI без изменения пробелов/CRLF."""
        raw=b'  \x1b[32mNES_STATUS {}\x1b[0m\r\n'
        decoded=log_text.decode_text(raw)
        self.assertEqual(decoded,raw.decode('ascii'))
        self.assertEqual(log_text.ANSI.sub('',decoded),'  NES_STATUS {}\r\n')


class InputLimitTests(unittest.TestCase):
    """Проверяет реальный общий нулевой input index и frame goal до открытия UART."""

    def test_input_zero_and_final_release(self):
        """Movie допускает edge0 и release360000, но не360001."""
        self.assertEqual(movie.MAX_FRAME,360000)
        self.assertEqual(probe.MAX_FRAME,movie.MAX_FRAME)
        self.assertEqual(movie.parse_edges(['0:1','360000:0']),[(0,1),(360000,0)])
        with self.assertRaises(ValueError):movie.parse_edges(['360001:0'])

    def test_until_frame_one_and_maximum(self):
        """Минимум1 и максимум360000 сохраняются при нулевой нумерации input."""
        for goal in (1,360000):
            self.assertEqual(probe.parse_args(['--game','0','--until-frame',str(goal)]).until_frame,goal)

    def test_until_frame_rejects_zero_and_overflow(self):
        """Невалидная цель отвергается parser до создания serial handle."""
        for goal in ('0','360001'):
            with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):
                probe.parse_args(['--game','0','--until-frame',goal])


if __name__=='__main__':unittest.main()
