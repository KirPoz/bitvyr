"""Проверки ограниченного UART capture: framing, RAW, запросы и отсутствие неявных команд."""
from pathlib import Path
import argparse
import importlib.util
import json
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import record_playthrough as capture


class FramingTests(unittest.TestCase):
    """Проверяет границы chunks, восстановление после длинной строки и сохранность RAW."""
    def test_split_utf8_and_crlf(self):
        """Побайтовый RX не разрушает UTF8-метку и CRLF; framing возвращает исходные байты завершённых строк."""
        framer = capture.LineFramer(32)
        data = 'метка\r\nnext\n'.encode()
        rows = []
        for byte in data:
            rows.extend(framer.feed(bytes([byte])))
        self.assertEqual(rows,[(1,'метка'.encode()),(2,b'next')])

    def test_oversized_line_never_becomes_valid_suffix(self):
        """После oversized-строки framing отбрасывает весь её хвост до newline и не выдаёт суффикс за новый валидный record."""
        framer = capture.LineFramer(8)
        self.assertEqual(framer.feed(b'x'*100),[])
        self.assertEqual(framer.pending,bytearray())
        self.assertEqual(framer.feed(b'OK\nvalid\n'),[(1,None),(2,b'valid')])

    def test_raw_preserved_and_byte_budget(self):
        """RAW сохраняет недекодируемые и нулевые байты; переполнение лимита останавливает запись, оставляя бюджет для capture_end."""
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root)
            sink = capture.SessionWriter(directory,capture.RESERVE+128)
            raw = b'\xff\x00unterminated\r\n'
            sink.write_raw(raw)
            with self.assertRaises(capture.BudgetReached):
                sink.write_raw(b'x'*200)
            sink.emit({'kind':'capture_end','fields':{}},final=True)
            sink.close()
            self.assertEqual((directory/'raw.serial.log').read_bytes(),raw)
            self.assertLessEqual(sum(p.stat().st_size for p in directory.iterdir()),capture.RESERVE+128)

    def test_fsync_failure_still_closes_both_files(self):
        """Ошибка fsync остаётся видимой вызывающему, но оба файловых дескриптора освобождаются."""
        with tempfile.TemporaryDirectory() as root:
            sink = capture.SessionWriter(Path(root),1024*1024)
            with patch.object(capture.os,'fsync',side_effect=OSError('injected fsync failure')):
                with self.assertRaises(OSError):
                    sink.close()
            self.assertTrue(sink.raw.closed)
            self.assertTrue(sink.events.closed)


class ControlTests(unittest.TestCase):
    """Проверяет host-маркеры и строго явный tainted resource snapshot."""
    def setUp(self):
        """Создаёт изолированный новый каталог и запись без оборудования."""
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        for name in ('requests','handled','acks'):
            (self.directory/name).mkdir()
        self.sink = capture.SessionWriter(self.directory,1024*1024)

    def tearDown(self):
        """Закрывает ограниченные файлы до удаления тестового временного каталога."""
        self.sink.close()
        self.temporary.cleanup()

    def enqueue(self, action, **values):
        """Помещает один корректно именованный host-запрос в очередь фикстуры."""
        path = self.directory/'requests'/('a'*32+'.json')
        path.write_bytes(capture.json_bytes({'action':action,**values}))
        return path.name

    def events(self):
        """Возвращает записанные события после передачи буфера файловой системе."""
        self.sink.flush()
        return [json.loads(line) for line in (self.directory/'events.jsonl').read_text().splitlines()]

    def test_no_pending_request_never_writes_uart(self):
        """Пустая очередь host-запросов не порождает UART-команды и не меняет capture."""
        serial = unittest.mock.Mock()
        self.assertFalse(capture.handle_requests(self.directory,self.sink,serial))
        serial.write.assert_not_called()

    def test_marker_is_host_only(self):
        """Запрос mark сохраняет host phase и accepted ACK, не отправляя текст маркера устройству."""
        serial = unittest.mock.Mock()
        name = self.enqueue('mark',text='BTDD: второй уровень',phase='dip')
        capture.handle_requests(self.directory,self.sink,serial)
        serial.write.assert_not_called()
        self.assertEqual(self.events()[0]['record']['fields']['phase'],'dip')
        self.assertEqual(json.loads((self.directory/'acks'/name).read_text())['status'],'accepted')

    def test_snapshot_is_explicit_and_tainted(self):
        """Явный snapshot посылает только tasks/status и отмечает tainted observer scope; это отправка команд, не device completion."""
        class Serial:
            """Минимальный UART, записывающий переданные команды вместо оборудования."""
            def __init__(self):
                """Создаёт пустой список тестовых передач вместо аппаратного UART."""
                self.writes=[]
            def write(self,data):
                """Сохраняет переданные байты и возвращает их полный размер, моделируя успешную передачу без устройства."""
                self.writes.append(data)
                return len(data)
        serial = Serial()
        self.enqueue('snapshot')
        capture.handle_requests(self.directory,self.sink,serial)
        self.assertEqual(serial.writes,[b'tasks\n',b'status\n'])
        records = [row['record'] for row in self.events()]
        self.assertEqual([row['kind'] for row in records],['observer_begin','observer_end'])
        self.assertTrue(all(row['fields']['performance_window_tainted'] for row in records))
        self.assertEqual(records[-1]['fields']['scope'],'commands_sent_not_device_completion')

    def test_snapshot_rejected_for_replay(self):
        """Replay без serial owner отклоняет snapshot и записывает rejected ACK."""
        name = self.enqueue('snapshot')
        capture.handle_requests(self.directory,self.sink,None)
        self.assertEqual(json.loads((self.directory/'acks'/name).read_text())['status'],'rejected')

    def test_partial_write_does_not_claim_completion(self):
        """Короткая передача команды даёт rejected ACK и не публикует ложный observer_end/completion."""
        serial = unittest.mock.Mock()
        serial.write.return_value = 1
        name = self.enqueue('snapshot')
        capture.handle_requests(self.directory,self.sink,serial)
        self.assertEqual(json.loads((self.directory/'acks'/name).read_text())['status'],'rejected')
        self.assertEqual(len(self.events()),1)

    def test_non_object_request_rejected_without_stopping_capture(self):
        """JSON-корни list/null/number отклоняются по одному, сохраняя capture и явную причину request_must_be_object."""
        for index,value in enumerate(([],None,123)):
            name = f'{index:032x}.json'
            (self.directory/'requests'/name).write_bytes(capture.json_bytes(value))
            self.assertFalse(capture.handle_requests(self.directory,self.sink,None))
            acknowledgement = json.loads((self.directory/'acks'/name).read_text())
            self.assertEqual(acknowledgement['status'],'rejected')
            self.assertIn('request_must_be_object',acknowledgement['error'])


class SerialTests(unittest.TestCase):
    """Проверяет параметры открытия без reset и строгую проверку CLI до COM."""
    def test_dtr_rts_set_before_open_and_no_write(self):
        """Открытие mocked serial выставляет DTR/RTS=false до open и не отправляет неявные команды."""
        fake = unittest.mock.Mock()
        with patch('serial.Serial',return_value=fake):
            result = capture.open_serial({'port':'COM_TEST'},115200)
        self.assertIs(result,fake)
        self.assertFalse(fake.dtr)
        self.assertFalse(fake.rts)
        fake.open.assert_called_once_with()
        fake.write.assert_not_called()

    def test_invalid_limit_rejected(self):
        """Нулевой, NaN, бесконечный и чрезмерный byte budget отвергаются CLI до создания сессии."""
        for value in ('0','nan','inf','8192'):
            with self.subTest(value=value),self.assertRaises(SystemExit):
                capture.parse_args(['capture','--session','not-created','--max-mib',value])

    def test_expected_firmware_is_not_invented_from_package(self):
        """Короткий boot ELF prefix сохраняется как prefix и не превращается в подтверждение полного firmware SHA."""
        record = capture.extra_record('I (100) app_init: ELF file SHA256: 48c997e396e917cc')
        self.assertFalse(record['fields']['full_sha256_verified'])
        self.assertEqual(record['fields']['elf_sha256_prefix'],'48c997e396e917cc')

    def test_transport_close_failure_keeps_logs_and_error_status(self):
        """Ошибка закрытия транспорта сохраняет raw/events, закрытый session status и cleanup_error, не объявляя capture успешным."""
        class Transport:
            """Транспорт с ошибкой закрытия; порт не открывает."""
            def read(self,size):
                """Возвращает пустой тестовый RX; COM не открывается и данные устройства не выдумываются."""
                return b''
            def close(self):
                """Выбрасывает заданную ошибку close, чтобы проверить сохранение raw и отказной cleanup status."""
                raise OSError('injected close failure')
        with tempfile.TemporaryDirectory() as root:
            session = Path(root)/'new-session'
            args = capture.parse_args(['capture','--session',str(session),'--seconds','.001','--no-report'])
            with patch.object(capture,'select_adapter',return_value={'port':'COM_TEST'}), \
                 patch.object(capture,'open_serial',return_value=Transport()):
                self.assertEqual(capture.capture(args),1)
            state = json.loads((session/'status.json').read_text())
            self.assertEqual(state['phase'],'closed')
            self.assertEqual(state['reason'],'capture_cleanup_error')
            self.assertTrue(state['transport_close_failed'])
            self.assertIn('injected close failure',state['cleanup_errors'][0])
            records = [json.loads(line) for line in (session/'events.jsonl').read_text().splitlines()]
            self.assertEqual(records[-1]['record']['kind'],'capture_end')
            (session/'raw.serial.log').rename(session/'raw-closed.log')


if __name__=='__main__':
    unittest.main()
