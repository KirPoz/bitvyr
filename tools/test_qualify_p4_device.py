"""Offline observer/input/endpoint guards; без SDK, COM и повторения Core physics."""
import copy
import json
import pathlib
import sys
import unittest
import qualify_p4_device as q
from test_pad_movie_queue import QueueUART, process


def report(n, t):
    """Две принятые границы120; fake время явно не является измерением hardware."""
    return dict(game=3,generation=7,frames=n,produced_frames=n,submitted_frames=n,
                samples=120,schema=2,slots=2,device_us=t,mapper=4,region='NTSC',
                producer='timing_fetch',consumer='output_compositor')


def hart(n):
    """Пара HART соответствует принятому окну, а не порядку RX строк."""
    return dict(schema=1,game=3,generation=7,first_sequence=n-120,last_sequence=n-1,
                samples=120,valid=120,invalid=0,overflow=0,cycles=800000000,retired=400000000,
                guest_frame_cycles=3573680,scope='stepFrame_hart',task_exclusive=0)


def movie():
    """Короткий fake movie с ненулевым действием внутри measured window."""
    return dict(crc32='c9ffbbdb',mapper=4,baseline_report=120,roi_reports=[240,360],
                roi_input0=[120,359],edges=[dict(frame=60,mask=1),dict(frame=320,mask=0)])


def scene():
    """Actual-cut shape включает отдельно принятый RGB/PPA и локальный audio owner."""
    s=dict(playing=True,image_available=True,input_valid=True,owner='timing_fetch',consumer='output_compositor',
           cpu_snapshot='actual_primary_cut',crc32='c9ffbbdb',mapper=4,game=3,region='NTSC',generation=7,
           frames=362,input_frame=361,input_mask=0,cpu_cycles=10800000,cpu_state_cycles=10800000,
           pc=32768,a=1,x=2,y=3,sp=255,p=36,pcm_count=800,pcm_hash='0123456789abcdef',
           rgb_hash='123456789abcdef0',schema_version=3,ram_policy='sparse120',ram_valid=False,
           ram_requested=False,ram_status='not_requested',ram_hash=None,ram_key=None,submitted_frames=362)
    p=dict(generation=7,accepted_frame=362,pending=False,completed_frame=362,completed_generation=7,
           started_frames=362,completed_frames=362,lcd_submitted_frames=362)
    a=dict(generation=7,accepted_frame=362,playing=True,muted=False,volume=28,active=True,source='board_audio_stats')
    return s,p,a


class ObserverGuards(unittest.TestCase):
    """Ломает границы/идентичность отдельно от корректности самого emulator Core."""
    def test_device_us_is_required_no_logger_ms_fallback(self):
        f=report(120,2000000)
        wire='I (2000) nes_app: NES_OUTPUT_FRAME_PIPELINE_TIMES '+' '.join(f'{k}={v}' for k,v in f.items())
        self.assertEqual(q.perf_row(wire)['device_us'],2000000)
        with self.assertRaisesRegex(ValueError,'device_us'):
            q.perf_row(wire.replace('device_us=2000000 ',''))
        self.assertIsNone(q.perf_row('I (2000) NES_PERF frames=120'))

    def test_device_us_bool_and_schema_rejected(self):
        for key,value in [('device_us',True),('schema',1),('samples',119),('slots',1),('region','PAL')]:
            f=report(120,2000000);f[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):
                q.perf_row('NES_OUTPUT_FRAME_PIPELINE_TIMES '+' '.join(f'{k}={v}' for k,v in f.items()))

    def test_weighted_fps_minmax_exact120_and_hart_out_of_rx_order(self):
        rs=[report(120,2000000),report(240,4000000),report(360,6400000)]
        out=q.statistics(rs,[hart(360),hart(240)],movie())
        self.assertAlmostEqual(out['FPS'],240000000/4400000)
        self.assertEqual((out['min120'],out['max120']),(50,60))
        self.assertEqual(out['windows'][0]['HART']['CPI'],2)
        self.assertFalse(out['logger_ms_fallback'])

    def test_report_duplicate_gap_regression_and_counter_loss_reject(self):
        for mode in ('duplicate','gap','clock','counter','generation'):
            rs=[report(120,2000000),report(240,4000000),report(360,6000000)]
            if mode=='duplicate':rs.append(copy.deepcopy(rs[-1]))
            elif mode=='gap':rs[1]['frames']=241
            elif mode=='clock':rs[1]['device_us']=1000000
            elif mode=='counter':rs[1]['produced_frames']-=1
            else:rs[1]['generation']+=1
            with self.subTest(mode=mode),self.assertRaises(ValueError):q.statistics(rs,[hart(240),hart(360)],movie())

    def test_hart_wrong_scope_sequence_or_invalid_cannot_qualify(self):
        for key,value in [('first_sequence',121),('generation',8),('invalid',1),('overflow',1),('valid',119),('cycles',0),('task_exclusive',1)]:
            h=hart(240);h[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):q.validate_hart(h,report(240,4_000_000))

    def test_hart_missing_duplicate_reject(self):
        rs=[report(120,2_000_000),report(240,4_000_000),report(360,6_000_000)]
        for hs in ([hart(240)],[hart(240),hart(240),hart(360)]):
            with self.assertRaises(ValueError):q.statistics(rs,hs,movie())

    def test_scene_actual_cut_sparse_ram_na_is_explicit_not_match(self):
        s,p,a=scene();v=q.validate_scene(s,p,a,movie(),{'index':3},7,360,28)
        self.assertEqual(v['actual_cut'],362);self.assertFalse(v['RAM_available']);self.assertIsNone(v['RAM_value_match'])
        self.assertFalse(v['scene_verified'])
        # Реальный P4 status может принять N без publish; retirement state предшествует sealed tail.
        s['cpu_state_cycles']-=1027;p['completed_frame']-=1
        p['started_frames']-=1;p['completed_frames']-=1;p['lcd_submitted_frames']-=1;s['submitted_frames']-=1
        self.assertEqual(q.validate_scene(s,p,a,movie(),{'index':3},7,360,28)['actual_cut'],362)

    def test_scene_input_cycles_rgb_identity_presentation_guard(self):
        for key,value in [('input_mask',1),('cpu_state_cycles',10800001),('rgb_hash','0000000000000000'),('generation',8),('image_available',False),('frames',359)]:
            s,p,a=scene();s[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):q.validate_scene(s,p,a,movie(),{'index':3},7,360,28)
        s,p,a=scene();p['pending']=True
        with self.assertRaises(ValueError):q.validate_scene(s,p,a,movie(),{'index':3},7,360,28)

    def test_scene_local_audio_active_volume_and_ram_null_guards(self):
        for key,value in [('active',False),('volume',60),('generation',8)]:
            s,p,a=scene();a[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):q.validate_scene(s,p,a,movie(),{'index':3},7,360,28)
        s,p,a=scene();s['ram_hash']='0123456789abcdef'
        with self.assertRaises(ValueError):q.validate_scene(s,p,a,movie(),{'index':3},7,360,28)

    def test_image_project_last_boot_and_ready(self):
        """Сверяет identity выбранной редакции с успешной загрузкой."""
        elf = "9" * 64
        text = ("Project name: bitvyr_nes_p4\nELF file SHA256: " + elf[:16]
                + "\nBOARD_READY display=1 touch=1 audio=1 sd=1 psram=1\n")
        self.assertEqual(q.admission(text, elf)["known_full_ELF"], elf)
        for bad in ("0" * 64, elf[:8]):
            with self.assertRaises(ValueError):
                q.admission(text, bad)
        for bad in (text.replace("bitvyr_nes_p4", "foreign"), text.replace("BOARD_READY", "NOT_READY"),
                    text + "ELF file SHA256: 00000000\n", text + "NES_C6_AUDIO_BACKEND ready=1"):
            with self.assertRaises(ValueError):
                q.admission(bad, elf)

    def test_volume_28_is_readonly_and_no_implicit_default(self):
        f=dict(volume=28,hardware_volume=28,muted=False,dirty=False,screen='library')
        q.base.quiet(f,expected_volume=28)
        for value in (None,1,60):
            with self.assertRaises(ValueError):q.base.quiet(f,expected_volume=value)
        f['dirty']=True
        with self.assertRaises(ValueError):q.base.quiet(f,expected_volume=28)

    def test_complete_shared_bank_edges_and_queue_plans_preserved(self):
        path=q.ROOT/'tools/qualification-movies.json';data=json.loads(path.read_text(encoding='utf8'))
        games=q.check_bank(data,path)
        self.assertEqual(len(games),26)
        self.assertEqual({g['crc32']:len(g['edges']) for g in games if len(g['edges'])>100},
                         {'bbf010e3':468,'9e13caa2':347,'a1e48d00':347})

    def test_actual347_468_full_rolling_input_no_truncation(self):
        games=json.loads((q.ROOT/'tools/qualification-movies.json').read_text(encoding='utf8'))['games']
        for g in games:
            if g['queue_plan'].get('mode')!='rolling':continue
            queue=q.rolling_queue_for(g);uart=QueueUART();queue.start(uart.send)
            # Frame0 APPLY обязан предшествовать INPUT0; до него задаётся только identity без progress.
            queue.observe_batch([dict(raw_type='NES_OUTPUT_FRAME_PIPELINE_TIMES',fields=dict(frames=0,generation=7))])
            for _ in range(4000):
                process(queue,uart)
                if queue.applied==len(g['edges']):break
            queue.finish()
            self.assertEqual(len(uart.writes),len(g['edges']))
            self.assertEqual(uart.writes,[f"padat {e['frame']} {e['mask']}" for e in g['edges']])
            self.assertLessEqual(queue.snapshot()['peak_reserved'],16)

    def test_rolling_ack_does_not_free_and_late_apply_holds(self):
        games=json.loads((q.ROOT/'tools/qualification-movies.json').read_text(encoding='utf8'))['games']
        g=next(g for g in games if g['crc32']=='9e13caa2')
        queue=q.rolling_queue_for(g);uart=QueueUART();queue.start(uart.send)
        queue.on_packet(uart.rx[0]);queue.refill(uart.send)
        self.assertEqual(len(uart.writes),16)
        frame,mask=queue.edges[0]
        with self.assertRaises(ValueError):queue.on_packet(dict(raw_type='NES_PAD_APPLY',fields=dict(frame=frame+1,scheduled=frame,mask=mask)))
        with self.assertRaises(ValueError):queue.refill(uart.send)

    def test_actual_parser_keeps_json_null_and_numeric_crc_string(self):
        p=q.packet('I (12) NES_OUTPUT_FRAME_PIPELINE_STATUS {"ram_hash":null,"frames":362}')
        self.assertIsNone(p['fields']['ram_hash'])
        p=q.packet('I (12) NES_GAME_START index=3 mapper=4 crc=131b9946')
        self.assertEqual(p['fields']['crc'],'131b9946')

if __name__=='__main__':unittest.main(verbosity=2)
