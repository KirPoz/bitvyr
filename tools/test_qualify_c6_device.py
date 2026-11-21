"""Проверки collector на fake UART без COM, SDK, сборки и оборудования."""
import importlib.util, io, pathlib, unittest
SPEC=importlib.util.spec_from_file_location("collector",pathlib.Path(__file__).with_name("qualify_c6_device.py"))
c=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(c)

# Хранит только заданные RX-chunks и отправленные байты, без настоящего serial.
class FakeUART:
    # Разделяет входные chunks и журнал записей для проверки частичных ответов.
    def __init__(self,chunks=()):self.chunks=list(chunks);self.writes=[]
    # Возвращает следующий подготовленный chunk; отсутствие данных не означает success.
    def read(self,n):return self.chunks.pop(0) if self.chunks else b""
    # Фиксирует исходные байты и моделирует полный write.
    def write(self,b):self.writes.append(b);return len(b)
    # Синхронная модель не создаёт дополнительных ACK или входных строк.
    def flush(self):pass

# Создаёт настоящий Link parser с fake транспортом, обходя конструктор аппаратного порта.
def link(chunks=()):
    obj=c.Link.__new__(c.Link);obj.port=FakeUART(chunks);obj.log=io.BytesIO()
    obj.pending=b"";obj.rows=[];obj.packets=[];obj.commands=[];obj.faults=[];obj.consumed=0
    return obj

# Проверяет строгий допуск, input/observer boundaries и фактические ответы cleanup.
class Guards(unittest.TestCase):
    def test_default_catalog_uses_loaded_measured_windows(self):
        """Обычный запуск использует боевой TMNT II и обе боевые версии Just Breed."""
        import json
        from contextlib import redirect_stdout
        from unittest.mock import patch

        output = io.StringIO()
        with patch.object(c, "Link", side_effect=AssertionError("COM opened")), redirect_stdout(output):
            self.assertEqual(c.main(["--expected-volume", "28"]), 0)
        summary = json.loads(output.getvalue())
        self.assertEqual(len(summary["games"]), 26)
        self.assertEqual(summary["movies"]["sha256"], c.MEASURED_CATALOG_SHA256)
        for crc, interval in {"c9ffbbdb": [3480, 3839], "fcf46dbb": [46440, 46799],
                              "131b9946": [28080, 28439]}.items():
            self.assertEqual(summary["roi_inputs"][crc], interval)
        self.assertTrue(all(end - start + 1 == 360 for start, end in summary["roi_inputs"].values()))

    def test_archived_elf_is_checked_before_opening_uart(self):
        """Для сохранённого образа обязательна сверка SHA256, даже в режиме без UART."""
        import hashlib
        import json
        from contextlib import redirect_stdout
        from tempfile import TemporaryDirectory
        from unittest.mock import patch

        with TemporaryDirectory() as temp:
            image = pathlib.Path(temp) / "candidate.elf"
            image.write_bytes(b"ELF fixture")
            expected = hashlib.sha256(image.read_bytes()).hexdigest()
            args = ["--movies", str(pathlib.Path(__file__).with_name("qualification-movies.json")),
                    "--expected-volume", "28", "--local-elf", str(image)]
            with patch.object(c, "Link", side_effect=AssertionError("COM opened")) as uart:
                with self.assertRaisesRegex(ValueError, "requires --expected-elf"):
                    c.main(args)
                with self.assertRaisesRegex(ValueError, "ELF SHA256 mismatch"):
                    c.main(args + ["--expected-elf", "0" * 64, "--execute-root"])
                output = io.StringIO()
                with redirect_stdout(output):
                    self.assertEqual(c.main(args + ["--expected-elf", expected]), 0)
                summary = json.loads(output.getvalue())
                self.assertFalse(summary["COM_opened"])
                self.assertEqual(summary["local_P4_image"]["sha256"], expected)
                self.assertEqual(pathlib.Path(summary["local_P4_image"]["path"]), image.resolve())
                uart.assert_not_called()

    # Проверяет загрузочный журнал больше блока чтения и сохранение незаконченной строки.
    def test_startup_above_32kb_preserved_and_partial_not_dropped(self):
        data=b"x"*32000+b"\nNES_C6_AUDIO_BACKEND ready=1 qualified=0\n"
        obj=link([data[:32768],data[32768:]+b"NES_INPUT frame=",b"0 serial=0\n"])
        obj.read();obj.read();obj.read()
        self.assertEqual(obj.log.getvalue(),data+b"NES_INPUT frame=0 serial=0\n")
        self.assertEqual(obj.packets[-1]["fields"]["frame"],0)
        self.assertEqual(obj.faults,[])
    # Различает запись ошибки платы и диагностическое поле с нулевым числом ошибок.
    def test_known_fault_preserved_but_qualified_zero_not_fault(self):
        obj=link([b"NES_C6_AUDIO_WINDOW qualified=0 observer_clock_errors=0\nNES_C6_AUDIO_DRAIN success=0 owner_retained=1\n"])
        obj.read()
        self.assertEqual(len(obj.faults),1)
        self.assertEqual(len(obj.packets),2)
        self.assertIn(b"success=0",obj.log.getvalue())
    # Проверяет фильтр команд на записи постоянных настроек и сброс платы.
    def test_no_persistent_or_reset_commands_allowed(self):
        obj=link()
        for command in ("save_settings","settings","volume 1","reset","region ntsc"):
            with self.assertRaises(ValueError):obj.send([command],"test")
        obj.send(["pad 0","menu","catalog 0 64"],"test")
        self.assertEqual(obj.port.writes,[b"pad 0\n",b"menu\n",b"catalog 0 64\n"])
    # Сверяет признаки выбранного звукового тракта C6 перед началом замера.
    def test_c6_backend_cannot_accept_local_or_nonbatch(self):
        row=dict(ready=1,warm_unbind=1,backend_batch=1,local_worker_dsp=0,stream_guard=1,mapper=5,qualified=0)
        self.assertEqual(c.validate_backend([dict(raw_type="NES_C6_AUDIO_BACKEND",fields=row)],5),row)
        for key in ("ready","backend_batch","stream_guard"):
            bad=dict(row);bad[key]=0
            with self.assertRaises(ValueError):c.validate_backend([dict(raw_type="NES_C6_AUDIO_BACKEND",fields=bad)],5)
    # Посторонний ввод или смена поколения сеанса останавливают зачёт сцены.
    def test_input_contamination_and_generation_drift_hold(self):
        movie=dict(edges=[dict(frame=120,mask=8),dict(frame=123,mask=0)])
        row=dict(frame=120,xbox_raw=0,xbox_effective=0,touch=0,contacts=0,suppressed=0,ids="0000",poll_ok=1,action="none",serial=8,applied=8,generation=7)
        c.input_trace(row,movie)
        with self.assertRaises(ValueError):c.input_trace(dict(row,touch=1),movie)
        traces=[dict(row,frame=0,serial=0,applied=0),row,dict(row,frame=123,generation=8,serial=0,applied=0)]
        with self.assertRaises(ValueError):c.check_input_coverage(traces,movie,[dict(generation=7)])
    # Оба пути измерения требуют непрерывных интервалов со временем платы в микросекундах.
    def test_both_backends_require_device_us_and_continuity(self):
        for mapper,tag in ((0,"NES_OUTPUT_FRAME_PIPELINE_TIMES"),(69,"NES_OUTPUT_FRAME_PIPELINE_TIMES"),(85,"NES_OUTPUT_FRAME_PIPELINE_TIMES"),(5,"NES_COUPLED_BENCH")):
            a=dict(raw_type=tag,game=3,mapper=mapper,generation=8,region="NTSC",samples=120,device_us=1000000,frames=120)
            b=dict(a,device_us=3000000,frames=240)
            c.validate_reports([a,b],dict(index=3),dict(mapper=mapper,region="NTSC"))
            with self.assertRaises(ValueError):c.validate_reports([a,dict(b,device_us=1000000)],dict(index=3),dict(mapper=mapper,region="NTSC"))
            with self.assertRaises(ValueError):c.validate_reports([a,dict(b,frames=360)],dict(index=3),dict(mapper=mapper,region="NTSC"))


    # Обе редакции сохраняют фактическую конечную границу и исходный пакет телеметрии.
    def test_end_scene_keeps_exact_actual_cut_and_raw_packet_both_backends(self):
        import json
        for mapper,tag in ((0,"NES_OUTPUT_FRAME_PIPELINE_STATUS"),(69,"NES_OUTPUT_FRAME_PIPELINE_STATUS"),(85,"NES_OUTPUT_FRAME_PIPELINE_STATUS"),(5,"NES_COUPLED_BENCH_STATUS")):
            fields=dict(playing=True,game=3,generation=8,mapper=mapper,region="NTSC",crc32="12345678",
                        frames=241,image_available=True,input_valid=True,input_frame=240,rgb_hash="1234567890abcdef",owner="thin_signals" if mapper==5 else "timing_fetch")
            raw=(tag+" "+json.dumps(fields)+"\nNES_UI_STATUS "+json.dumps(dict(video=True,source="software_lcd_submit"))+"\n").encode()
            obj=link([raw])
            result=c.end_scene_snapshot(obj,dict(index=3),dict(mapper=mapper,region="NTSC",crc32="12345678",roi_reports=[240]),[dict(frames=240,generation=8)])
            self.assertEqual(result["status"]["line"],raw.decode().splitlines()[0])
            self.assertEqual(result["snapshot_input0"],240)
            self.assertEqual(result["accepted_frames_after_report"],1)
            self.assertEqual(obj.port.writes,[b"status\n",b"ui_status\n"])
            self.assertEqual(result["scene_verified"],False)
    # Проверяет поколение ответа и положение запроса относительно начала измерения.
    def test_end_scene_rejects_stale_generation_and_pre_roi_request(self):
        import json
        fields=dict(playing=True,game=3,generation=7,mapper=5,region="NTSC",crc32="12345678",
                    frames=241,image_available=True,input_valid=True,input_frame=240,rgb_hash="1234567890abcdef")
        raw=("NES_COUPLED_BENCH_STATUS "+json.dumps(fields)+"\nNES_UI_STATUS {}\n").encode()
        movie=dict(mapper=5,region="NTSC",crc32="12345678",roi_reports=[240])
        with self.assertRaises(ValueError):c.end_scene_snapshot(link([raw]),dict(index=3),movie,[dict(frames=240,generation=8)])
        obj=link()
        with self.assertRaises(ValueError):c.end_scene_snapshot(obj,dict(index=3),movie,[dict(frames=120,generation=8)])
        self.assertEqual(obj.port.writes,[])
    # Завершение прогона ожидает полный ответ со списком задач.
    def test_cleanup_waits_complete_tasks_payload(self):
        header=dict(raw_type="NES_TASKS",fields=dict(count=2),line="NES_TASKS count=2",rx_offset=10)
        one=dict(raw_type="NES_TASK",fields=dict(name="main"),line="NES_TASK name=main",rx_offset=20)
        two=dict(one,rx_offset=40)
        self.assertFalse(c.task_response_complete([header,one]))
        self.assertTrue(c.task_response_complete([header,one,two]))


    # Формирует согласованный Library-снимок; overrides задают отрицательные случаи.
    @staticmethod
    def quiet_fields(**changes):
        row=dict(volume=1,hardware_volume=1,muted=False,dirty=False,paused=False,
                 screen="library",region="NTSC 60",active_region="none")
        row.update(changes)
        return row
    def test_explicit_saved_volume_requires_exact_match(self):
        """Сохранённая громкость60 принимается только явным ожиданием, остальные guards прежние."""
        actual=self.quiet_fields(volume=60,hardware_volume=60)
        with self.assertRaises(ValueError):c.validate_quiet_settings(actual)
        c.validate_quiet_settings(actual,expected_volume=60)
        for bad in (dict(actual,hardware_volume=1),dict(actual,muted=True),dict(actual,dirty=True)):
            with self.assertRaises(ValueError):c.validate_quiet_settings(bad,expected_volume=60)
        for invalid in (True,0,101):
            with self.assertRaises(ValueError):c.validate_quiet_settings(actual,expected_volume=invalid)

    # Подготовка прогона ожидает актуальные настройки и состояние интерфейса.
    def test_quiet_preflight_waits_fresh_settings_and_UI(self):
        import json
        fresh=self.quiet_fields()
        raw1=("NES_SETTINGS "+json.dumps(fresh)+"\n").encode()
        raw2=b'NES_UI_STATUS {"video":false,"source":"software_lcd_submit"}\n'
        obj=link([raw1,raw2])
        obj.packets.append(dict(raw_type="NES_SETTINGS",fields=self.quiet_fields(volume=99),line="old",rx_offset=-1))
        proof=c.quiet_preflight(obj)
        self.assertEqual(proof["settings"]["fields"],fresh)
        self.assertEqual(obj.port.chunks,[])
        self.assertEqual(obj.port.writes,[b"ui_status\n"])
    # Сверяет громкость, mute, регион, владельца и отсутствие несохранённых настроек.
    def test_quiet_settings_rejects_volume_mute_dirty_region_and_owner(self):
        c.validate_quiet_settings(self.quiet_fields())
        c.validate_quiet_settings(self.quiet_fields(screen="game",active_region="NTSC"),True)
        for changes in (dict(volume=2),dict(hardware_volume=0),dict(muted=True),dict(dirty=True),
                        dict(region="AUTO"),dict(region="PAL 50"),dict(screen="pause"),
                        dict(paused=True),dict(active_region="NTSC"),dict(volume=True)):
            with self.subTest(changes=changes),self.assertRaises(ValueError):
                c.validate_quiet_settings(self.quiet_fields(**changes))
        with self.assertRaises(ValueError):
            c.validate_quiet_settings(self.quiet_fields(screen="game",active_region="PAL"),True)
    # Решение о готовности принимается по последнему ответу устройства.
    def test_quiet_preflight_rejects_fresh_bad_state_despite_stale_good(self):
        import json
        bad=self.quiet_fields(volume=2,hardware_volume=2)
        raw=("NES_SETTINGS "+json.dumps(bad)+'\nNES_UI_STATUS {"video":false,"source":"software_lcd_submit"}\n').encode()
        obj=link([raw])
        obj.packets.append(dict(raw_type="NES_SETTINGS",fields=self.quiet_fields(),line="stale good",rx_offset=-1))
        with self.assertRaises(ValueError):c.quiet_preflight(obj)
        self.assertEqual(obj.port.writes,[b"ui_status\n"])




if __name__ == "__main__":
    unittest.main()
