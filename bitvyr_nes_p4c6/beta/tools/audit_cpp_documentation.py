"""Проверка русской документации C++ и неизменности исполняемых токенов.

Грамматика устанавливается отдельно от прошивки в .local/tools/python-docs.
Препроцессорные ветви разбираются как исходный текст, поэтому отчёт дополняет,
а не заменяет сборку поддерживаемых конфигураций.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SUFFIXES = {'.cpp', '.h', '.hpp', '.inl', '.inc'}
SCOPES = ('app/nes', 'components/board_support', 'main',
          'coprocessor/c6_audio/src', 'coprocessor/c6_audio/apu_fixed')
DOCUMENTED_ADAPTERS = {'app/nes/runtime/bridge/ThinBridge.h',
                       'app/nes/runtime/bridge/ThinBridge.cpp'}


def owned_files(root: Path):
    """Перечисляет собственные C++-файлы, исключая копии сторонних исходников."""
    for scope in SCOPES:
        for path in sorted((root / scope).rglob('*')):
            relative = path.relative_to(root).as_posix()
            if path.suffix in SUFFIXES and ('vendor' not in path.parts or relative in DOCUMENTED_ADAPTERS):
                yield path


def tokens(source: str) -> list[str]:
    """Выделяет лексемы без комментариев; строки и raw-строки сохраняет целиком.

    Конец директивы препроцессора остаётся отдельным токеном. Смена __LINE__
    после добавления комментариев требует отдельного разбора диагностик.
    """
    result, i, line_start, directive = [], 0, True, False
    operators = sorted(('>>=', '<<=', '<=>', '->*', '...', '::', '++', '--',
                        '->', '.*', '&&', '||', '<=', '>=', '==', '!=', '+=',
                        '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<', '>>', '##'),
                       key=len, reverse=True)
    while i < len(source):
        if source.startswith('\\\n', i) or source.startswith('\\\r\n', i):
            i += 2 if source[i + 1] == '\n' else 3
            continue
        c = source[i]
        if c.isspace():
            if c == '\n':
                if directive:
                    result.append('<PP_EOL>')
                directive, line_start = False, True
            i += 1
            continue
        if source.startswith('//', i):
            end = source.find('\n', i)
            i = len(source) if end < 0 else end
            continue
        if source.startswith('/*', i):
            end = source.find('*/', i + 2)
            if end < 0:
                raise ValueError('Незакрытый комментарий')
            i = end + 2
            continue
        if c == '#' and line_start:
            directive = True
        line_start = False
        raw = re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(', source[i:])
        if raw:
            end = source.find(')' + raw.group(1) + '"', i + raw.end())
            if end < 0:
                raise ValueError('Незакрытая raw-строка')
            end += len(raw.group(1)) + 2
            result.append(source[i:end])
            i = end
            continue
        literal = re.match(r'(?:u8|u|U|L)?["\']', source[i:])
        if literal:
            quote, end = source[i + literal.end() - 1], i + literal.end()
            while end < len(source):
                if source[end] == '\\':
                    end += 2
                elif source[end] == quote:
                    end += 1
                    break
                else:
                    end += 1
            result.append(source[i:end])
            i = end
            continue
        word = re.match(r'[A-Za-z_$][\w$]*|(?:\d[\w.\']*)', source[i:])
        if word:
            result.append(word.group())
            i += word.end()
            continue
        op = next((op for op in operators if source.startswith(op, i)), c)
        result.append(op)
        i += len(op)
    if directive:
        result.append('<PP_EOL>')
    return result


def token_digest(path: Path) -> str:
    """Возвращает SHA256 последовательности лексем, независимый от комментариев."""
    data = '\0'.join(tokens(path.read_text(encoding='utf-8-sig'))).encode('utf-8')
    return hashlib.sha256(data).hexdigest()


def parser_cpp():
    """Создаёт синтаксический анализатор; зависимости не включаются в ESP-IDF."""
    sys.path.insert(0, str(ROOT / '.local/tools/python-docs'))
    import tree_sitter_cpp
    from tree_sitter import Language, Parser
    return Parser(Language(tree_sitter_cpp.language()))


def walk(node):
    """Обходит дерево, включая объявления внутри условных ветвей препроцессора."""
    yield node
    for child in node.children:
        yield from walk(child)


def leading_comment(source: bytes, node, comments) -> str:
    """Находит договор перед методом, в том числе перед условным GNU-атрибутом.

    Между Doxygen и методом допускаются только пробелы, управляющие директивы
    и атрибуты. Наличие другого C++-объявления запрещает перенос комментария.
    """
    anchor=node.parent if node.parent and node.parent.type=='template_declaration' else node
    for comment in reversed(comments):
        if comment.end_byte>anchor.start_byte:
            continue
        if anchor.start_byte-comment.end_byte>2048:
            break
        text=source[comment.start_byte:comment.end_byte].decode('utf-8')
        if not(text.startswith('/**') or text.startswith('///')):
            continue
        gap=source[comment.end_byte:anchor.start_byte].decode('utf-8')
        gap=re.sub(r'^\s*#(?:if|ifdef|ifndef|elif|else|endif)\b[^\n]*','',gap,flags=re.MULTILINE)
        gap=re.sub(r'\[\[.*?\]\]','',gap,flags=re.DOTALL)
        gap=re.sub(r'__attribute__\s*\(\(.*?\)\)|__declspec\s*\([^)]*\)','',gap,flags=re.DOTALL)
        if not gap.strip():
            return text
        break
    return ''


def functions(path: Path, parser) -> list[dict]:
    """Находит определения и объявления методов с ближайшим Doxygen-комментарием.

    Лямбды и вызовы не выдаются за именованные методы. Макросы и неполные .inl
    могут требовать ручного дополнения: ошибочные узлы перечисляются в отчёте.
    """
    source = path.read_bytes()
    tree = parser.parse(source)
    comments=[node for node in walk(tree.root_node) if node.type=='comment']
    rows, seen = [], set()
    for node in walk(tree.root_node):
        if node.type not in ('function_definition', 'declaration', 'field_declaration'):
            continue
        declarator = node.child_by_field_name('declarator')
        if declarator is None:
            continue
        function = declarator
        while function and function.type != 'function_declarator':
            inner = function.child_by_field_name('declarator')
            if inner is None:
                inner = next((c for c in function.named_children if c.type in
                              ('function_declarator','reference_declarator','pointer_declarator',
                               'parenthesized_declarator')), None)
            function = inner
        if function is None:
            continue
        name_node = function.child_by_field_name('declarator')
        if name_node is None:
            continue
        name = source[name_node.start_byte:name_node.end_byte].decode('utf-8')
        if name in {'for','if','while','switch','catch','return','sizeof','alignof','decltype','static_assert'}:
            # В неполных macro/.inl ветвях recovery-узел может выглядеть как метод.
            continue
        if name.startswith('(*') or name.startswith('(&'):
            # Поле-указатель на callback не является объявлением метода.
            continue
        parameters = function.child_by_field_name('parameters')
        signature = source[node.start_byte:(function.end_byte)].decode('utf-8')
        comment = leading_comment(source,node,comments)
        start = node.start_point.row + 1
        if (start, name) in seen:
            continue
        seen.add((start, name))
        rows.append({'path': path.relative_to(ROOT).as_posix(), 'line': start,
                     'name': name, 'signature': signature[:1200],
                     'definition': node.type == 'function_definition',
                     'parameters': source[parameters.start_byte:parameters.end_byte].decode('utf-8') if parameters else '',
                     'russian_comment': bool(re.search('[А-Яа-яЁё]', comment)),
                     'doxygen': comment.startswith('/**') or comment.startswith('///'),
                     'comment': comment, 'parse_has_error': tree.root_node.has_error})
    return rows


def main():
    """Сохраняет проверяемый отчёт и возвращает ошибку при изменении C++-лексем."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--inventory', action='store_true')
    parser.add_argument('--require-russian', action='store_true',help='Ошибка при отсутствии русского Doxygen у именованного метода')
    args = parser.parse_args()
    files = list(owned_files(ROOT))
    report = {'scope': list(SCOPES), 'excluded': ['upstream vendor, кроме явно перечисленных адаптеров', 'managed_components', 'build*', 'artifacts'],
              'explicit_adapters': sorted(DOCUMENTED_ADAPTERS),
              'files': len(files), 'changed_tokens': [], 'missing_baseline': [],
              'limits': 'Проверка токенов не заменяет сборку. __LINE__ и диагностические хеши меняются; старые frozen-доказательства относятся к прежним исходникам.'}
    if args.baseline:
        for path in files:
            before = args.baseline / path.relative_to(ROOT)
            if not before.exists():
                report['missing_baseline'].append(path.relative_to(ROOT).as_posix())
            elif token_digest(path) != token_digest(before):
                report['changed_tokens'].append(path.relative_to(ROOT).as_posix())
    if args.inventory or args.require_russian:
        cpp = parser_cpp()
        rows = [row for path in files for row in functions(path, cpp)]
        report['functions'] = rows
        report['function_occurrences'] = len(rows)
        report['russian_documented_occurrences'] = sum(r['russian_comment'] and r['doxygen'] for r in rows)
        report['missing_russian'] = [{'path':r['path'],'line':r['line'],'name':r['name']}
                                     for r in rows if not(r['russian_comment'] and r['doxygen'])]
    report['passed_token_comparison'] = not report['changed_tokens'] and not report['missing_baseline'] if args.baseline else None
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k != 'functions'}, ensure_ascii=False))
    return int(bool((args.baseline and not report['passed_token_comparison']) or
                    (args.require_russian and report.get('missing_russian'))))


if __name__ == '__main__':
    raise SystemExit(main())
