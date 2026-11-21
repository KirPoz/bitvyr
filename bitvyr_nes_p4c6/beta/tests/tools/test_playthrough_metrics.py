"""Проверяет парсер UART на сохранённых строках платы и синтетических отказах.

Источник строк TMNT II: SHA-256 b0dabfca434a3ef234befbe508b2bec254d6034bb77aa53f58cb6de7151d8c9f.
Тесты разбирают форматы вывода и проверяют единицы измерения.
"""

import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import playthrough_metrics as metrics


RAW_GAME = ('I (1590904) nes_app: NES_GAME_START index=16 '
            'name=Teenage_Mutant_Ninja_Turtles_II_-_The_Arcade_Game_(U)_[!] '
            'mapper=4 region=NTSC crc=c9ffbbdb source=sd zip=1 size=524304')
RAW_INPUT = ('NES_INPUT generation=62 frame=0 xbox_raw=0 xbox_effective=0 serial=0 '
             'touch=0 applied=0 contacts=0 ids=0000 poll_ok=1 suppressed=0 action=none')
RAW_TIMES = ('I (1652750) nes_app: NES_OUTPUT_FRAME_PIPELINE_TIMES game=16 frames=3480 '
             'produced_frames=3480 submitted_frames=3473 samples=120 generation=62 mapper=4 '
             'region=NTSC core_us=20348 producer_wall_us=20645 seal_us=13 audio_retire_us=35 '
             'snapshot_us=154 consumer_active_us=6692 dsp_us=1640 witness_copy_us=148 '
             'rgb_copy_us=0 present_us=1181 wait_us=9435 audio_us=150 slots=2 schema=2 '
             'producer=timing_fetch consumer=output_compositor read_witness=unavailable '
             'effective_tail=off game_fps_claim=0')
RAW_LOOP = ('NES_FRAME_LOOP_METRICS generation=62 accepted_first=3361 accepted_last=3480 '
            'completed_main_iterations=120 submitted_before=3353 submitted_after=3473 '
            'present_started_before=3353 present_started_after=3473 pending_after=0 '
            'pending_generation_after=0 pending_frame_after=0 dispatched_input_first=3361 '
            'dispatched_input_last=3480 accepted_input_first=3360 accepted_input_last=3479 '
            'accepted_input_service_us=610 loop_wall_us=21221 outer_join_us=9488 cli_menu_us=96 '
            'pace_us=32 input_current_us=632 dispatch_us=67 consume_present_us=10219 report_us=87 '
            'end_yield_us=522 remaining_us=74 loop_wall_max_us=28924 '
            'loop_wall_max_accepted_frame=3472 outer_join_max_us=12026 cli_menu_max_us=123 '
            'pace_max_us=45 input_current_max_us=3670 dispatch_max_us=927 '
            'consume_present_max_us=17312 report_max_us=4096 end_yield_max_us=1031 '
            'remaining_max_us=99 stage_maxima_sum_valid=0 main_core=0 '
            'unit=completed_main_iteration game_fps_claim=0 producer_time_added=0 '
            'separate_ppa_elapsed_added=0')
RAW_AUDIO = ('NES_FRAME_AUDIO_STATUS {"generation":62,"accepted_frame":3492,"playing":true,'
             '"muted":false,"volume":1,"active":true,"accepted_frames":2782617,'
             '"dma_frames":2981120,"missed_refill_blocks":54,"missed_refill_frames":13824,'
             '"max_write_gap_us":51862,"max_write_wait_us":15725,"source":"board_audio_stats",'
             '"game_fps_claim":false}')
RAW_SETTINGS = ('NES_SETTINGS {"volume":1,"hardware_volume":1,"muted":false,"swap_ab":false,'
                '"show_fps":true,"region":"NTSC 60","active_region":"NTSC",'
                '"screen":"settings","paused":true,"dirty":true}')


class PlaythroughMetricsTests(unittest.TestCase):
    """Сверяет wire-факты и ограниченные ошибки без межкадровых догадок."""

    def assert_error(self, line, code):
        """Проверяет отдельный отказ, не превращая неизвестный лог в успех."""
        result = metrics.parse_line(line)
        self.assertEqual(result['kind'], 'parse_error', result)
        self.assertEqual(result['fields']['error'], code, result)
        return result

    def test_actual_times_has_wire_units_not_fps(self):
        """TIMES сохраняет accepted/produced/submitted отдельно и исходную ms-метку."""
        record = metrics.parse_line(RAW_TIMES)
        self.assertEqual(record['kind'], 'output_frame_pipeline_times')
        self.assertEqual(record['raw_type'], 'NES_OUTPUT_FRAME_PIPELINE_TIMES')
        self.assertEqual(record['device_timestamp_ms'], 1652750)
        fields = record['fields']
        self.assertEqual((fields['frames'], fields['produced_frames'], fields['submitted_frames']),
                         (3480, 3480, 3473))
        self.assertEqual(fields['samples'], 120)
        self.assertEqual(fields['core_us'], 20348)
        self.assertEqual(fields['read_witness'], 'unavailable')
        self.assertNotIn('fps', fields)
        self.assertEqual(type(fields['game_fps_claim']), int)

    def test_actual_loop_no_inherited_timestamp(self):
        """Plain printf не наследует время предыдущего ESP сообщения."""
        metrics.parse_line(RAW_TIMES)
        record = metrics.parse_line(RAW_LOOP)
        self.assertEqual(record['kind'], 'frame_loop_metrics')
        self.assertNotIn('device_timestamp_ms', record)
        self.assertEqual(record['fields']['accepted_input_first'], 3360)
        self.assertEqual(record['fields']['accepted_first'], 3361)
        self.assertEqual(record['fields']['loop_wall_us'], 21221)

    def test_actual_audio_json_preserves_pcm_counts(self):
        """Аудио PCM/I2S счётчики сохраняются, не переводятся в число видеокадров."""
        record = metrics.parse_line(RAW_AUDIO)
        self.assertEqual(record['kind'], 'frame_audio_status')
        self.assertIs(record['fields']['muted'], False)
        self.assertEqual(record['fields']['accepted_frames'], 2782617)
        self.assertEqual(record['fields']['dma_frames'], 2981120)
        self.assertNotIn('fps', record['fields'])

    def test_actual_game_crc_and_input_ids_text(self):
        """CRC и контактные ids сохраняют точное hex-написание."""
        self.assertEqual(metrics.parse_line(RAW_GAME)['fields']['crc'], 'c9ffbbdb')
        self.assertEqual(metrics.parse_line(RAW_INPUT)['fields']['ids'], '0000')

    def test_actual_settings_labels(self):
        """UI region label содержит пробел, а paused/dirty остаются boolean."""
        fields = metrics.parse_line(RAW_SETTINGS)['fields']
        self.assertEqual(fields['region'], 'NTSC 60')
        self.assertIs(fields['paused'], True)
        self.assertIs(fields['dirty'], True)

    def test_runtime_zero_and_reset_do_not_inherit_generation(self):
        """Нулевое поколение реально допустимо; парсер не сшивает разные сессии."""
        line = ('NES_OUTPUT_FRAME_PIPELINE_STATUS {"playing":false,"frames":0,'
                '"generation":0,"owner":"unbound","image_available":false,"game_fps_claim":false}')
        self.assertEqual(metrics.parse_line(line)['fields']['generation'], 0)
        ready = metrics.parse_line('I (2655) nes_app: NES_RUNTIME_READY games=19 psram=1 lcd=1 touch=1 audio=1 sd=1')
        self.assertEqual(ready['kind'], 'runtime_ready')
        self.assertNotIn('generation', ready['fields'])
        self.assertEqual(metrics.parse_line(line)['fields']['frames'], 0)

    def test_ansi_prefix_and_error_timestamp(self):
        """ANSI CSI снимается, фактическая метка сохраняется также при ошибке."""
        line = '\x1b[31mW (0012) nes_app: NES_STATUS {broken}\x1b[0m\r\n'
        record = self.assert_error(line, 'invalid_json')
        self.assertEqual(record['device_timestamp_ms'], 12)
        self.assertEqual(record['raw_type'], 'NES_STATUS')

    def test_game_name_fixed_suffix(self):
        """Настоящее unquoted имя может содержать пробелы, = и начальный апостроф."""
        name = "'Kid's ROM mapper=99 region=PAL test=1"
        line = ('NES_GAME_START index=0 name=' + name +
                ' mapper=4 region=NTSC crc=00123456 source=sd zip=0 size=16384')
        fields = metrics.parse_line(line)['fields']
        self.assertEqual(fields['name'], name)
        self.assertEqual(fields['crc'], '00123456')
        self.assertEqual(fields['mapper'], 4)

    def test_quoted_json_rom_and_hash_identity(self):
        """JSON корректно передаёт кавычки ROM; нулевой хеш не становится числом."""
        record = metrics.parse_line(r'NES_ROM {"index":0,"title":"ROM \"2\" = Test","crc32":"00001234","read_hash":"0000000000000000"}')
        self.assertEqual(record['fields']['title'], 'ROM "2" = Test')
        self.assertEqual(record['fields']['crc32'], '00001234')
        self.assertEqual(record['fields']['read_hash'], '0000000000000000')

    def test_quoted_kv_and_windows_path(self):
        """Кавыченный KV сохраняет ключеподобный текст и Windows backslashes."""
        result = metrics.parse_line(r'NES_TASK id=1 name="free task=other" path="C:\nes\rom.nes" run_us=0')
        self.assertEqual(result['fields']['name'], 'free task=other')
        self.assertEqual(result['fields']['path'], 'C:\\nes\\rom.nes')

    def test_resource_records(self):
        """Heap/task поля остаются фактами ресурсов с прежними именами/единицами."""
        record = metrics.parse_line('NES_HEAP largest_internal_dma=38912 largest_internal=38912 minimum_internal=80384')
        self.assertEqual(record['kind'], 'heap')
        self.assertEqual(record['fields']['minimum_internal'], 80384)
        self.assertEqual(metrics.parse_line('NES_TASKS total_us=1651673751 count=15')['fields']['count'], 15)

    def test_ordinary_unknown_and_invented_tags(self):
        """Случайное упоминание тега и несуществующие события не становятся метриками."""
        for line in ('', 'booting...', 'I (1) other: hello NES_STATUS {}',
                     'NES_UNKNOWN x=1', 'NES_FRAME_PERF fps=60', 'NES_PAUSE', 'NES_RESET'):
            with self.subTest(line=line):
                self.assertIsNone(metrics.parse_line(line))

    def test_generic_perf_only_preserves_existing_wire_fps(self):
        """Редкий generic путь имеет собственное fps, не создаваемое из TIMES."""
        fields = metrics.parse_line('NES_PERF game=0 frames=120 fps=59.25 core_us=100 render_us=200')['fields']
        self.assertEqual(fields['fps'], 59.25)

    def test_numeric_bool_and_hex_types(self):
        """0/1 не bool, explicit true/false bool, hex с0x обычный int."""
        fields = metrics.parse_line('NES_EXECUTION mode=worker n=0 one=1 enabled=true other=false ptr=0x00008000')['fields']
        self.assertEqual(type(fields['n']), int)
        self.assertEqual(type(fields['one']), int)
        self.assertIs(fields['enabled'], True)
        self.assertIs(fields['other'], False)
        self.assertEqual(fields['ptr'], 32768)

    def test_signed_microseconds_preserved(self):
        """Signed printf-поля не исправляются на нуль."""
        record = metrics.parse_line(RAW_TIMES.replace('wait_us=9435', 'wait_us=-3'))
        self.assertEqual(record['fields']['wait_us'], -3)

    def test_malformed_or_truncated(self):
        """Узнаваемая неполная запись отличается от неизвестной строки."""
        for line, code in (('NES_STATUS {', 'invalid_json'),
                           ('NES_STATUS []', 'invalid_json'),
                           ('NES_OUTPUT_FRAME_PIPELINE_TIMES game=1', 'missing_field'),
                           ('NES_RUNTIME_READY', 'missing_field'),
                           ('NES_GAME_START index=1 name=truncated', 'invalid_kv'),
                           ('NES_EXECUTION mode="broken', 'invalid_kv'),
                           ('NES_EXECUTION mode="worker"oops', 'invalid_kv')):
            with self.subTest(line=line):
                self.assert_error(line, code)

    def test_duplicate_fields(self):
        """JSON и KV не скрывают второе значение identity."""
        self.assert_error('NES_EXECUTION mode=worker generation=1 generation=2', 'duplicate_field')
        self.assert_error('NES_STATUS {"generation":1,"generation":2}', 'duplicate_field')

    def test_json_boolean_schema(self):
        """Строка false или число0 не выдаются за настоящий JSON boolean."""
        for value in ('"false"', '0', 'null'):
            self.assert_error('NES_STATUS {"playing":' + value + '}', 'invalid_boolean')

    def test_invalid_number_and_nonfinite(self):
        """Повреждённые numeric identity, NaN/Infinity и большие числа отвергаются."""
        for line in ('NES_EXECUTION generation=nope', 'NES_EXECUTION generation=-1',
                     'NES_STATUS {"generation":true}', 'NES_STATUS {"n":NaN}',
                     'NES_STATUS {"n":1e999}', 'NES_EXECUTION x=' + '9' * 100,
                     'NES_EXECUTION ptr=0x' + 'f' * 17):
            self.assert_error(line, 'invalid_number')

    def test_line_byte_limit(self):
        """Лимит применяется к UTF-8 bytes, не только числу символов."""
        self.assert_error('NES_EXECUTION name=' + 'x' * 16384, 'line_too_long')
        self.assert_error('NES_EXECUTION name=' + 'я' * 8192, 'line_too_long')
        allowed = 'NES_EXECUTION name=' + 'x' * (16384 - len('NES_EXECUTION name='))
        self.assertEqual(metrics.parse_line(allowed)['kind'], 'execution')

    def test_field_count_limit(self):
        """Ровно256 KV/JSON полей допустимы,257 не расширяют запись."""
        kv = 'NES_EXECUTION ' + ' '.join('v%d=1' % n for n in range(256))
        self.assertEqual(len(metrics.parse_line(kv)['fields']), 256)
        self.assert_error(kv + ' v256=1', 'field_limit')
        fields = {'v%d' % n: 1 for n in range(256)}
        self.assertEqual(len(metrics.parse_line('NES_STATUS ' + json.dumps(fields))['fields']), 256)
        fields['v256'] = 1
        self.assert_error('NES_STATUS ' + json.dumps(fields), 'field_limit')

    def test_nested_json_limit(self):
        """Глубина и общее число вложенных элементов ограничены."""
        self.assert_error('NES_STATUS {"x":' + '[' * 10 + '0' + ']' * 10 + '}', 'nested_limit')
        self.assert_error('NES_STATUS ' + json.dumps({'values': [0] * 256}), 'field_limit')

    def test_multiline_nul_and_invalid_unicode(self):
        """Склеенные строки/управляющие данные не превращаются в одну метрику."""
        self.assert_error('NES_EXECUTION mode=worker\nNES_STATUS {}', 'invalid_value')
        self.assert_error('NES_EXECUTION mode=worker\x00', 'invalid_value')
        self.assert_error('NES_EXECUTION name=\ud800', 'invalid_unicode')
        self.assert_error(r'NES_ROM {"title":"\ud800"}', 'invalid_unicode')

    def test_wrong_input_type(self):
        """Ошибочный caller получает ограниченный отчёт вместо аварии callback."""
        self.assert_error(b'NES_STATUS {}', 'input_type')

    def test_empty_library_error_is_valid(self):
        """Реальный error= при успешном сканировании SD не считается усечением."""
        record = metrics.parse_line('I (2645) nes_app: NES_LIBRARY source=sd root=/sdcard/nes_rom games=19 skipped=0 error=')
        self.assertEqual(record['fields']['error'], '')


if __name__ == '__main__':
    unittest.main()
