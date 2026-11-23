"""Границы проверки comment-only изменений: строки, токены и препроцессор."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from audit_cpp_documentation import tokens


class CommentTokenTests(unittest.TestCase):
    """Не разрешает проверке маскировать изменения исполняемого C++ под комментарии."""

    def test_comments_are_whitespace_not_concatenation(self):
        """Комментарий между именами не склеивает их в новый идентификатор."""
        self.assertEqual(tokens('int/**/value;'),tokens('int value; // пояснение'))
        self.assertNotEqual(tokens('int value;'),tokens('intvalue;'))

    def test_protocol_strings_preserved(self):
        """Похожие на комментарии символы внутри протокольной строки остаются данными."""
        self.assertNotEqual(tokens('send("// a /* b */");'),tokens('send("// a");'))

    def test_raw_string_preserved(self):
        """Raw-строка с delimiter, кавычками и комментариями не режется лексером."""
        source='auto s=u8R"tag(// text /* data */ " quoted)tag";'
        self.assertEqual(len(tokens(source)),5)
        self.assertNotEqual(tokens(source),tokens(source.replace('data','other')))

    def test_directive_boundary_preserved(self):
        """Снятие конца директивы не считается форматированием."""
        self.assertNotEqual(tokens('#define A x\ny'),tokens('#define A x y'))

    def test_continuation_is_not_directive_end(self):
        """Продолжение макроса сохраняет одну логическую директиву."""
        self.assertEqual(tokens('#define A x\\\n+y\n'),tokens('#define A x+y\n'))


if __name__=='__main__':
    unittest.main()
