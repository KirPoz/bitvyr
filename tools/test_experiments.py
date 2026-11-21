"""Проверяет целостность нового append и $shared на временных файлах, без главного журнала."""
from pathlib import Path
from unittest import mock
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE=Path(__file__).resolve().parent
SPEC=importlib.util.spec_from_file_location('bitvyr_experiments',HERE/'experiments.py')
journal=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(journal)


class ExperimentsTests(unittest.TestCase):
    """Проверяет data-contract отдельного журнала; внешний experiments.json не читается и не меняется."""
    def setUp(self):
        """Создаёт изолированную схему и один допустимый UTF8 event без абсолютных путей."""
        self.temporary=tempfile.TemporaryDirectory();self.addCleanup(self.temporary.cleanup)
        root=Path(self.temporary.name);self.target=root/'history.json';self.event_path=root/'event.json'
        self.target.write_text(json.dumps(dict(chronology=[],essential_payloads={},shared_essential_subtrees={})),encoding='utf-8')
        self.event=dict(id='append-contract',date='2026-11-17',title='Временная проверка',status='PASS',result='fixture',decision='только тест')
        self.write_event(self.event)

    def write_event(self,event):
        """Записывает только входную тестовую карточку; production append читает её самостоятельно."""
        self.event_path.write_text(json.dumps(event,ensure_ascii=False),encoding='utf-8')

    def assert_clean(self):
        """Проверяет освобождение собственных временного файла и lock после завершённой операции."""
        self.assertFalse(self.target.with_name(self.target.name+'.lock').exists())
        self.assertFalse(self.target.with_name(self.target.name+'.tmp').exists())

    def test_atomic_append_and_idempotence(self):
        """Полный новый JSON существует до replace, прежние байты неизменны; повтор не переписывает журнал."""
        before=self.target.read_bytes();replace=os.replace;observed=[]

        def checked_replace(source,destination):
            """Наблюдает границу атомарной замены и затем вызывает настоящий os.replace."""
            self.assertEqual(self.target.read_bytes(),before)
            self.assertEqual(Path(source).parent,self.target.parent)
            self.assertEqual(Path(destination),self.target)
            self.assertTrue(self.target.with_name(self.target.name+'.lock').exists())
            self.assertEqual(json.loads(Path(source).read_text(encoding='utf-8'))['chronology'],[self.event])
            observed.append(True);return replace(source,destination)

        with mock.patch.object(journal.os,'replace',side_effect=checked_replace):result=journal.append(self.target,self.event_path)
        self.assertEqual(observed,[True]);self.assertEqual(result['status'],'APPENDED')
        self.assertEqual(result['previous_sha256'],hashlib.sha256(before).hexdigest())
        after=self.target.read_bytes();self.assertEqual(result['sha256'],hashlib.sha256(after).hexdigest())
        self.assertEqual(journal.read(self.target)['chronology'],[self.event]);self.assert_clean()
        self.assertEqual(journal.append(self.target,self.event_path),dict(status='UNCHANGED',id=self.event['id']))
        self.assertEqual(self.target.read_bytes(),after);self.assert_clean()

    def test_conflict_preserves_history(self):
        """Тот же id с другим результатом отвергается, первоначальные байты и единственная запись сохраняются."""
        journal.append(self.target,self.event_path);before=self.target.read_bytes()
        self.write_event(dict(self.event,result='другой результат'))
        with self.assertRaises(ValueError):journal.append(self.target,self.event_path)
        self.assertEqual(self.target.read_bytes(),before);self.assert_clean()

    def test_existing_lock_is_not_owned_or_removed(self):
        """Занятый lock запрещает второго писателя; чужой lock не удаляется при отказе os.open."""
        lock=self.target.with_name(self.target.name+'.lock');lock.write_text('другой владелец',encoding='utf-8')
        before=self.target.read_bytes()
        with self.assertRaises(FileExistsError):journal.append(self.target,self.event_path)
        self.assertEqual(self.target.read_bytes(),before);self.assertEqual(lock.read_text(encoding='utf-8'),'другой владелец')

    def test_failed_replace_keeps_original_and_cleans_owned_files(self):
        """Ошибка файловой замены не выдаёт APPENDED, не меняет историю и освобождает собственные tmp/lock."""
        before=self.target.read_bytes()
        with mock.patch.object(journal.os,'replace',side_effect=OSError('fixture replace failure')):
            with self.assertRaises(OSError):journal.append(self.target,self.event_path)
        self.assertEqual(self.target.read_bytes(),before);self.assert_clean()

    def test_invalid_event_and_duplicate_history_rejected(self):
        """Неполное событие, неверная дата и повтор id во входном журнале не допускают append."""
        before=self.target.read_bytes()
        for invalid in ({'id':'missing'},dict(self.event,date='2026-02-31')):
            self.write_event(invalid)
            with self.assertRaises(ValueError):journal.append(self.target,self.event_path)
            self.assertEqual(self.target.read_bytes(),before);self.assert_clean()
        self.write_event(self.event)
        self.target.write_text(json.dumps(dict(chronology=[self.event,self.event])),encoding='utf-8');duplicate=self.target.read_bytes()
        with self.assertRaises(ValueError):journal.append(self.target,self.event_path)
        self.assertEqual(self.target.read_bytes(),duplicate);self.assert_clean()

    def test_shared_expansion_and_cycle_rejection(self):
        """Вложенные $shared разворачиваются без изменения pool; цикл и отсутствующий ключ не скрываются."""
        pool=dict(leaf={'count':3},branch=[{'$shared':'leaf'},{'value':7}]);before=json.dumps(pool,sort_keys=True)
        self.assertEqual(journal.expand({'nested':{'$shared':'branch'}},pool),{'nested':[{'count':3},{'value':7}]})
        self.assertEqual(json.dumps(pool,sort_keys=True),before)
        with self.assertRaises(ValueError):journal.expand({'$shared':'a'},dict(a={'$shared':'b'},b={'$shared':'a'}))
        with self.assertRaises(KeyError):journal.expand({'$shared':'missing'},pool)


    def test_original_artifact_sha_resolves_without_build(self):
        """Исходный SHA старого индекса открывает embedded essential; внешнего файла не требуется."""
        data=dict(chronology=[],essential_payloads={'payload':{'count':{'$shared':'leaf'}}},
                  shared_essential_subtrees={'leaf':7},artifact_index={'original':{'essential_payload_sha256':'payload'}},text_result_index={})
        self.assertEqual(journal.lookup_payload(data,'original'),{'count':7})
        self.assertEqual(journal.lookup_payload(data,'payload'),{'count':7})
        with self.assertRaises(KeyError):journal.lookup_payload(data,'missing')

    def test_historical_artifact_links_to_existing_event(self):
        """Исходный SHA встроенной записи возвращает её индекс и существующее событие."""
        item = {'kind': 'event_record', 'already_inline_managed_event_id': self.event['id']}
        data = dict(chronology=[self.event], essential_payloads={}, shared_essential_subtrees={},
                    artifact_index={'original': item})
        self.assertEqual(journal.lookup_payload(data, 'original'),
                         {'source_sha256': 'original', 'artifact_index_entry': item, 'event': self.event})
        data['chronology'].clear()
        with self.assertRaises(ValueError):
            journal.lookup_payload(data, 'original')

    def test_historical_trace_summary_resolves_by_original_sha(self):
        """Оба индекса сводок отвечают без сохранённых байтов трассы."""
        data = dict(chronology=[], essential_payloads={}, shared_essential_subtrees={},
                    artifact_index={'trace': {'trace_summary': {'rows': 120}},
                                    'compiler': {'compiler_trace_summary': {'files': 4}}})
        for digest, item in data['artifact_index'].items():
            with self.subTest(digest=digest):
                self.assertEqual(journal.lookup_payload(data, digest),
                                 {'source_sha256': digest, 'artifact_index_entry': item})

    def test_cleanup_payload_is_self_contained_and_hash_checked(self):
        """Числа/null/UTF8 читаются после удаления build; повреждённая проекция не принимается."""
        payload={'status':'PASS','mean':54.88094341561308,'RAM':None,'note':'Только software snapshot'}
        digest=hashlib.sha256(json.dumps(payload,ensure_ascii=False,sort_keys=True,separators=(',',':'),allow_nan=False).encode('utf-8')).hexdigest()
        archive={'artifacts':{'source_sha':{'historical_paths':['missing/build/summary.json'],'payload_sha256':digest}},'payloads':{digest:payload}}
        data=dict(chronology=[{'result':{'durable_build_evidence':archive}}],essential_payloads={},shared_essential_subtrees={},
                  artifact_index={'source_sha': {'trace_summary': {'rows': 120}}})
        self.assertEqual(journal.lookup_payload(data,'source_sha'),payload)
        archive['payloads'][digest]=dict(payload,mean=1)
        with self.assertRaises(ValueError):journal.lookup_payload(data,'source_sha')

    def test_append_preserves_all_historical_sections(self):
        """Append не меняет opaque историю/индексы/shared; сохраняет точные измерения и русский текст."""
        old={'chronology':[dict(self.event,id='old')],'essential_payloads':{'x':{'value':None,'n':2**63}},
             'shared_essential_subtrees':{'shared':[1,2]},'artifact_index':{'sha':{'kind':'old'}},'legacy':['Прежний результат',54.88094341561308]}
        self.target.write_text(json.dumps(old,ensure_ascii=False),encoding='utf-8')
        journal.append(self.target,self.event_path);after=journal.read(self.target)
        self.assertEqual(after['chronology'][:-1],old['chronology'])
        for key in old:
            if key!='chronology':self.assertEqual(after[key],old[key])
        self.assert_clean()


    def test_split_journal_keeps_history_readable_and_writes_only_current(self):
        """Внешние части доступны по ID и SHA, а запись и проверка конфликтов охватывают обе эпохи."""
        base = self.target.parent
        history = base / 'old.json'
        evidence = base / 'evidence.json'
        old = dict(self.event, id='older')
        history.write_text(json.dumps([old]), encoding='utf-8')
        evidence.write_text(json.dumps(dict(essential_payloads={'digest': {'frames': 120}},
                                            shared_essential_subtrees={})), encoding='utf-8')
        self.target.write_text(json.dumps(dict(chronology=[],
                                               parts={'chronology': history.name, 'evidence': evidence.name})),
                               encoding='utf-8')
        readonly = tuple(path.read_bytes() for path in (history, evidence))
        journal.append(self.target, self.event_path)
        self.assertEqual(journal.read(self.target)['chronology'], [old, self.event])
        self.assertEqual(journal.lookup_payload(journal.read(self.target), 'digest'), {'frames': 120})
        self.write_event(old)
        self.assertEqual(journal.append(self.target, self.event_path)['status'], 'UNCHANGED')
        self.write_event(dict(old, status='HOLD'))
        with self.assertRaises(ValueError):
            journal.append(self.target, self.event_path)
        self.assertEqual(journal.read_json(self.target)['chronology'], [self.event])
        self.assertEqual(tuple(path.read_bytes() for path in (history, evidence)), readonly)
        self.assert_clean()


    def test_export_cli_preserves_history_and_sha_lookups(self):
        """Единый файл сохраняет порядок, оба вида доказательств и чтение без исходных частей."""
        base = self.target.parent
        history = base / 'old.json'
        evidence = base / 'evidence.json'
        older = dict(self.event, id='older', date='2026-11-18')
        shared = {'frames': 120, 'note': 'Русский текст'}
        history.write_text(json.dumps([older]), encoding='utf-8')
        evidence.write_text(json.dumps({
            'essential_payloads': {'digest': {'$shared': 'leaf'}},
            'shared_essential_subtrees': {'leaf': shared},
            'artifact_index': {'original': {'essential_payload_sha256': 'digest'}},
        }), encoding='utf-8')
        self.target.write_text(json.dumps({
            'chronology': [self.event],
            'parts': {'chronology': history.name, 'evidence': evidence.name},
        }), encoding='utf-8')
        output = base / 'bundle' / 'full.json'
        inputs = {path: path.read_bytes() for path in (self.target, history, evidence)}

        def run_cli(*args):
            return json.loads(subprocess.check_output([sys.executable, '-B', str(HERE / 'experiments.py'),
                                                        '--file', str(args[0]), *args[1:]], encoding='utf-8'))

        receipt = run_cli(self.target, '--export', str(output))
        self.assertEqual(receipt, {'status': 'EXPORTED', 'path': str(output),
                                   'events': 2, 'bytes': output.stat().st_size})
        self.assertEqual({path: path.read_bytes() for path in inputs}, inputs)
        self.assertEqual(journal.read(output), journal.read(self.target))
        for path in inputs:
            path.unlink()
        standalone = journal.read(output)
        self.assertNotIn('parts', standalone)
        self.assertEqual(standalone['chronology'], [older, self.event])
        self.assertEqual(run_cli(output, '--id', older['id']), older)
        self.assertEqual(run_cli(output, '--sha', 'original'), shared)

    def test_export_rejects_sources_and_hardlink_alias(self):
        """Пути журнала и его частей защищены, даже если указано второе имя того же файла."""
        base = self.target.parent
        history = base / 'old.json'
        evidence = base / 'evidence.json'
        history.write_text('[]', encoding='utf-8')
        evidence.write_text('{}', encoding='utf-8')
        self.target.write_text(json.dumps({
            'chronology': [],
            'parts': {'chronology': history.name, 'evidence': evidence.name},
        }), encoding='utf-8')
        alias = base / 'alias.json'
        os.link(evidence, alias)
        inputs = {path: path.read_bytes() for path in (self.target, history, evidence, alias)}
        for output in inputs:
            with self.subTest(output=output):
                with self.assertRaises(ValueError):
                    journal.export(self.target, output)
        self.assertEqual({path: path.read_bytes() for path in inputs}, inputs)

    def test_export_failure_preserves_existing_output(self):
        """Ошибка записи или замены сохраняет предыдущий полный экспорт и удаляет временный файл."""
        output = self.target.parent / 'full.json'
        output.write_bytes(b'{"old":true}\n')
        previous = output.read_bytes()
        files = set(output.parent.iterdir())

        def partial_dump(data, stream, **options):
            stream.write('{"incomplete":')
            raise OSError('fixture write failure')

        with mock.patch.object(journal.json, 'dump', side_effect=partial_dump):
            with self.assertRaises(OSError):
                journal.export(self.target, output)
        self.assertEqual(output.read_bytes(), previous)
        self.assertEqual(set(output.parent.iterdir()), files)
        with mock.patch.object(journal.os, 'replace', side_effect=OSError('fixture replace failure')):
            with self.assertRaises(OSError):
                journal.export(self.target, output)
        self.assertEqual(output.read_bytes(), previous)
        self.assertEqual(set(output.parent.iterdir()), files)
        self.assertEqual(journal.export(self.target, output)['status'], 'EXPORTED')
        self.assertEqual(journal.read(output), journal.read(self.target))


if __name__=='__main__':unittest.main(verbosity=2)
