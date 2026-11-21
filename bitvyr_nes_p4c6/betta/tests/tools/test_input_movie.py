"""Регрессии общего movie parser, фактического probe CLI и input trace audit."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from tools.input_movie import parse_edges, mask_at, changes

ROOT=Path(__file__).resolve().parents[2]
def load(name):
    """Загружает явно именованный current tool для offline проверки без выполнения его CLI."""
    spec=importlib.util.spec_from_file_location(name,ROOT/'tools'/f'{name}.py')
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module
audit=load('audit_input_trace')
probe=load('device_probe')

class InputMovieTests(unittest.TestCase):
    """Проверяет внешние input contracts, не эмуляцию CPU/видео/PCM."""
    def test_guest_index_and_held_masks(self):
        """Проверяет нулевой input index, удержание масок между edges и подавление повторного значения."""
        edges=parse_edges(['100:8','103:0','400:0x80','500:131','510:128','600:0'])
        self.assertEqual([mask_at(edges,n) for n in [0,99,100,102,103,399,400,499,500,509,510,600]],
                         [0,0,8,8,0,0,128,128,131,131,128,0])
        self.assertEqual(list(changes([(0,0),(10,128),(11,128),(20,0)])),[10,20])

    def test_invalid_movies(self):
        """Отвергает неоднозначный синтаксис, переполнение, неверный порядок и отсутствие release0."""
        for specs in (['1'],['1:0:0'],['-1:0'],['360001:0'],['1:-1'],['1:256'],
                      ['1:1','1:0'],['2:1','1:0'],['x:0'],['1:128'],['1:+0'],['+1:0'],['1:00'],
                      [f'{n}:0' for n in range(17)]):
            with self.assertRaises(ValueError):parse_edges(specs)


    def test_cli_mixing_and_queue_bound(self):
        """Проверяет actual probe argument parser до UART, запрещая смешанные источники и поздний edge."""
        valid=probe.parse_args(['--game','0','--pad-frame','100:128','--pad-frame','200:0','--until-frame','300'])
        self.assertEqual(valid.pad_edges,[(100,128),(200,0)])
        for args in (['--pad-frame','1:0'],['--game','0','--pad-frame','1:0','--start-frame','100'],
                     ['--game','0','--pad-frame','1:0','--start-at','1'],
                     ['--game','0','--pad-frame','300:0','--until-frame','300']):
            with self.assertRaises(SystemExit):probe.parse_args(args)

    def test_audit_detects_missing_movement_and_release(self):
        """Проверяет trace аудит независимыми строками и отрицательными отсутствующими edges."""
        edges=[(10,128),(20,129),(23,128),(30,0)]
        def row(n,mask):
            """Создаёт literal UART input строку с известной маской, без использования парсера как oracle."""
            return (f'NES_INPUT generation=2 frame={n} xbox_raw=0 xbox_effective=0 serial={mask} '
                    f'touch=0 applied={mask} contacts=0 ids=0000 poll_ok=1 suppressed=0 action=none')
        with tempfile.TemporaryDirectory() as folder:
            log=Path(folder)/'movie.log'
            def check(records):
                """Пишет synthetic RAW и запускает настоящий offline auditor; аппаратного порта нет."""
                log.write_text('\n'.join(['NES_GAME_START index=0']+records+['NES_STATUS {"frames":40}'])+'\n',encoding='utf-8')
                return audit.audit(log,[],edges)
            rows=[row(0,0)]+[row(n,mask) for n,mask in edges]
            self.assertTrue(check(rows)['passed'])
            for bad in (rows[:1]+rows[2:],rows[:-1],rows[:-1]+[row(30,128)]):
                with self.assertRaises(ValueError):check(bad)

if __name__=='__main__':unittest.main()
