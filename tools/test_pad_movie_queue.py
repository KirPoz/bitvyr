"""Модель UART проверяет порядок длинных маршрутов ввода и подтверждение каждой команды."""
import importlib.util
import io
import json
import pathlib
import unittest
from pad_movie_queue import RollingPadQueue,rolling_queue_for

def movie(count=80,initial=14,spacing=9):
    """Полный строго упорядоченный поток, пересекающий ROI, без сокращения до16 событий."""
    edges=[dict(frame=60+i*spacing,mask=(1 if i%2==0 else 0))for i in range(count)]
    if edges:edges[-1]["mask"]=0
    text=[f"{e['frame']}:{e['mask']}"for e in edges]
    end=max(960,((edges[-1]["frame"]+119)//120+1)*120)if edges else 960
    return dict(crc32="12345678",mapper=4,region="NTSC",edges=edges,end_frame=end,
                baseline_report=480,roi_input0=[480,839],roi_reports=[600,720,840],
                queue_plan=dict(mode="rolling",queue_capacity=16,initial=text[:initial],deferred=text[initial:]))

# Формирует один wire-record для отдельной модели ACK/APPLY и progress.
def packet(tag,**fields):
    return dict(raw_type=tag,fields=fields)

class QueueUART:
    """Очередь устройства и RX независимы; чтение не выдаёт дополнительный send credit."""
    # Разделяет очередь устройства, непрочитанный RX и журнал write-attempts.
    def __init__(self):
        self.hardware=[];self.rx=[];self.writes=[];self.peak=0;self.short=False
    # Моделирует bounded FIFO и ACK; отправка сама не возвращает credit.
    def send(self,commands,phase):
        for command in commands:
            self.writes.append(command)
            if self.short:raise ValueError("Short UART command write")
            _,frame,mask=command.split();frame,mask=int(frame),int(mask)
            if len(self.hardware)>=16:raise ValueError("Fake device overflow")
            self.hardware.append((frame,mask));self.peak=max(self.peak,len(self.hardware))
            self.rx.append(packet("NES_PAD_SCHEDULE",frame=frame,mask=mask,queued=len(self.hardware)))
    # Сначала отдаёт накопленный RX, затем один настоящий по модели APPLY.
    def read(self):
        if self.rx:
            batch,self.rx=self.rx,[];return batch
        if self.hardware:
            frame,mask=self.hardware.pop(0)
            return [packet("NES_PAD_APPLY",frame=frame,scheduled=frame,mask=mask)]
        return []

def process(queue,uart,packets=None):
    """Все уже прочитанные progress учитываются до первого refill старого ACK."""
    data=uart.read()if packets is None else packets
    queue.observe_batch(data)
    for p in data:
        queue.on_packet(p)
        if p["raw_type"]in("NES_PAD_SCHEDULE","NES_PAD_APPLY"):queue.refill(uart.send)
    queue.finish_batch()

# Проверки порядка выдачи, подтверждения и применения событий при ограниченной очереди.
class RollingGuards(unittest.TestCase):
    # Плотный поток из 601 события сохраняет порядок и укладывается в 16 зарезервированных мест.
    def test_dense_601_edges_exact_no_drop_and_16_reservation_bound(self):
        q=RollingPadQueue(movie(601,spacing=1));u=QueueUART();q.start(u.send)
        self.assertEqual(len(u.writes),14)
        q.observe_batch([packet("NES_INPUT",generation=9,frame=0,action="none")])
        for _ in range(4000):
            process(q,u)
            if q.applied==601:break
        q.finish();proof=q.snapshot()
        self.assertEqual(proof["ACKs"],601);self.assertEqual(proof["APPLYs"],601)
        self.assertEqual(proof["peak_reserved"],16);self.assertEqual(u.peak,16)
        self.assertEqual(u.writes,[f"padat {e['frame']} {e['mask']}"for e in q.movie["edges"]])
        self.assertEqual(proof["reserved"],0)

    # До подтверждения применения зарезервированные места остаются занятыми.
    def test_unACKed_reservations_do_not_create_credit(self):
        q=RollingPadQueue(movie(40,initial=16));u=QueueUART();q.start(u.send)
        q.refill(u.send);self.assertEqual(len(u.writes),16)
        q.observe_batch(u.rx[:1]);q.on_packet(u.rx[0]);q.refill(u.send)
        self.assertEqual(len(u.writes),16)
        self.assertEqual(q.snapshot()["reserved"],16)
        self.assertEqual(q.snapshot()["ACKs"],1)

    # Повторный или преждевременный ACK и неверная глубина очереди прерывают воспроизведение.
    def test_duplicate_out_of_order_ACK_and_depth_fail_closed(self):
        for change in("duplicate","out_of_order","depth"):
            q=RollingPadQueue(movie());u=QueueUART();q.start(u.send);ack=u.rx[0]
            if change=="duplicate":
                q.on_packet(ack)
                wrong=ack
            elif change=="out_of_order":wrong=u.rx[1]
            else:wrong=packet("NES_PAD_SCHEDULE",frame=60,mask=1,queued=16)
            with self.subTest(change=change),self.assertRaises(ValueError):q.on_packet(wrong)
            before=len(u.writes)
            with self.assertRaises(ValueError):q.refill(u.send)
            self.assertEqual(len(u.writes),before)

    # Применение сверяется с ACK, кадром и порядком выдачи каждого события.
    def test_apply_requires_ack_order_exact_frame_and_no_duplicate(self):
        for change in("unacked","late","wrongmask","duplicate","out_of_order"):
            q=RollingPadQueue(movie());u=QueueUART();q.start(u.send)
            if change!="unacked":
                for ack in u.rx:q.on_packet(ack)
            valid=packet("NES_PAD_APPLY",frame=60,scheduled=60,mask=1)
            if change=="late":p=packet("NES_PAD_APPLY",frame=61,scheduled=60,mask=1)
            elif change=="wrongmask":p=packet("NES_PAD_APPLY",frame=60,scheduled=60,mask=0)
            elif change=="out_of_order":p=packet("NES_PAD_APPLY",frame=69,scheduled=69,mask=0)
            elif change=="duplicate":q.on_packet(valid);p=valid
            else:p=valid
            with self.subTest(change=change),self.assertRaises(ValueError):q.on_packet(p)

    # Смена поколения и пропуск подтверждения применения делают последовательность недостоверной.
    def test_generation_change_and_missing_apply_progress_rejected(self):
        q=RollingPadQueue(movie());u=QueueUART();q.start(u.send)
        q.observe_batch([packet("NES_INPUT",frame=0,generation=9,action="none")])
        with self.assertRaises(ValueError):q.observe_batch([packet("NES_INPUT",frame=1,generation=10,action="none")])
        q=RollingPadQueue(movie());u=QueueUART();q.start(u.send)
        for ack in u.rx:q.on_packet(ack)
        q.observe_batch([packet("NES_INPUT",frame=60,generation=9,action="none")])
        with self.assertRaises(ValueError):q.finish_batch()

    # После получения нового номера кадра просроченное событие уже нельзя подавать.
    def test_received_tail_progress_prevents_refill_late_unsent_edge(self):
        q=RollingPadQueue(movie(40,initial=1,spacing=1));u=QueueUART();q.start(u.send)
        batch=[u.rx[0],packet("NES_INPUT",frame=100,generation=9,action="none")]
        q.observe_batch(batch);q.on_packet(batch[0])
        with self.assertRaises(ValueError):q.refill(u.send)
        self.assertEqual(len(u.writes),1)

    # При короткой записи место сохраняется, поскольку часть команды могла дойти до платы.
    def test_short_write_keeps_uncertain_reservation_and_no_retry(self):
        q=RollingPadQueue(movie());u=QueueUART();u.short=True
        with self.assertRaises(ValueError):q.start(u.send)
        proof=q.snapshot()
        self.assertEqual((proof["attempted"],proof["completed_writes"],proof["reserved"]),(1,0,1))
        for action in(lambda:q.start(u.send),lambda:q.refill(u.send),lambda:q.finish()):
            with self.assertRaises(ValueError):action()
        self.assertEqual(len(u.writes),1)

    # Проверяет явный выбор режима, совпадение плана с вводом и исходное состояние кнопок.
    def test_optin_plan_exactness_and_nonzero_frame0_guard(self):
        self.assertIsNone(rolling_queue_for(dict(queue_plan={})))
        bad=movie();bad["queue_plan"]["deferred"]=bad["queue_plan"]["deferred"][:-1]
        with self.assertRaises(ValueError):RollingPadQueue(bad)
        bad=movie();bad["edges"][0]=dict(frame=0,mask=1);bad["queue_plan"]["initial"][0]="0:1"
        with self.assertRaisesRegex(ValueError,"frame0"):RollingPadQueue(bad)
        bad=movie();bad["edges"][1]["frame"]=bad["edges"][0]["frame"]
        with self.assertRaises(ValueError):RollingPadQueue(bad)

class CollectorUART:
    """Реальный Link parser получает synthetic bytes; PAD ACK/APPLY и status генерирует отдельная модель."""
    # Создаёт независимые счётчики guest input, отчётов и bounded PAD FIFO.
    def __init__(self,movie,c6=False,fault=None):
        self.movie=movie;self.c6=c6;self.fault=fault;self.lines=[];self.writes=[];self.hardware=[]
        self.playing=False;self.frame=0;self.report=0;self.mask=0;self.generation=9;self.peak=0;self.end_observer=None;self.initial_input_done=False
    # Кодирует synthetic строку в тех же JSON/KV формах, которые читает настоящий parser.
    def emit(self,tag,fields,json_object=False):
        text=json.dumps(fields)if json_object else " ".join(f"{k}={v}"for k,v in fields.items())
        if tag=="NES_OUTPUT_FRAME_PIPELINE_TIMES":text=f"I ({fields['frames']*17}) nes_app: "+tag+" "+text
        else:text=tag+" "+text
        self.lines.append((text+"\n").encode())
    # Возвращает согласованные настройки/UI текущего fake-сеанса без записи NVS.
    def settings(self):
        self.emit("NES_SETTINGS",dict(volume=60,hardware_volume=60,muted=False,dirty=False,paused=False,screen="game"if self.playing else"library",region="NTSC 60",active_region="NTSC"if self.playing else"none"),True)
        self.emit("NES_UI_STATUS",dict(video=self.playing,source="software_lcd_submit"),True)
    # Снимок модели содержит отдельный accepted input/image cut либо состояние unbound.
    def status(self):
        if self.playing:
            f=max(1,self.frame)
            self.emit("NES_OUTPUT_FRAME_PIPELINE_STATUS",dict(playing=True,game=3,generation=9,mapper=self.movie["mapper"],region=self.movie["region"],crc32=self.movie["crc32"],frames=f,produced_frames=f,submitted_frames=f,
                owner="timing_fetch",consumer="output_compositor",cpu_snapshot="actual_primary_cut",read_witness="unavailable",effective_tail="off",game_fps_claim=False,
                schema_version=3,read_count=0,read_hash="0000000000000000",ram_policy="sparse120",ram_valid=False,ram_hash=None,
                pcm_count=800,pcm_hash="1234567890abcdef",rgb_hash="234567890abcdef1",image_available=True,input_valid=True,input_frame=f-1,input_mask=self.mask),True)
        else:self.emit("NES_OUTPUT_FRAME_PIPELINE_STATUS",dict(playing=False,owner="unbound",frames=0,generation=10,image_available=False),True)
    # Выполняет только модель команд collector; short/drop faults задаются явно.
    def write(self,payload):
        command=payload.decode().strip();self.writes.append((command,self.frame))
        if command=="ui_status":self.settings()
        elif command=="status":self.status()
        elif command=="tasks":
            self.emit("NES_TASKS",dict(total_us=1000000,count=2))
            for i,name in enumerate(["main","nes_emulation"]):self.emit("NES_TASK",dict(id=i+4,name=name,run_us=500,priority=1,core=i,stack_min=6000))
        elif command.startswith("run "):
            self.playing=True
            self.emit("NES_OUTPUT_FRAME_PIPELINE_MODE",dict(schema=2,mapper=self.movie["mapper"],producer="timing_fetch",consumer="output_compositor",read_witness="unavailable",slots=2,borrowed=1,copy=0,early_dsp=1,effective_tail="off",game_fps_claim=0))
            if self.c6:self.emit("NES_C6_AUDIO_BACKEND",dict(ready=1,warm_unbind=1,backend_batch=1,local_worker_dsp=0,stream_guard=1,mapper=self.movie["mapper"],qualified=0))
            self.emit("NES_GAME_START",dict(index=3,name="Fake",mapper=self.movie["mapper"],region=self.movie["region"],crc=self.movie["crc32"],source="sd",zip=1,size=32784))
        elif command.startswith("padat "):
            _,frame,mask=command.split();frame,mask=int(frame),int(mask)
            if self.fault=="short"and frame==self.movie["edges"][16]["frame"]:return len(payload)-1
            if len(self.hardware)>=16:raise AssertionError("Collector overflowed16 hardware slots")
            self.hardware.append((frame,mask));self.peak=max(self.peak,len(self.hardware))
            if not(self.fault=="drop_ack"and frame==self.movie["edges"][-1]["frame"]):
                self.emit("NES_PAD_SCHEDULE",dict(frame=frame,mask=mask,queued=len(self.hardware)))
        elif command=="pad 0":self.mask=0;self.hardware=[]
        elif command=="menu":
            self.playing=False
            if self.c6:
                self.emit("NES_C6_AUDIO_DRAIN",dict(success=1,guest_frames=self.frame,remote_frames=self.frame,stream_verified_frames=self.frame,pending_pcm=0,publication_held=0,actual_join=1,local_renderer_used=0))
                self.emit("NES_C6_AUDIO_UNBOUND",dict(ready=1,old_generation=9,no_job=1,source_held=0))
                self.emit("NES_FRAME_C6_FINAL_DRAIN",dict(in_flight=0,c6_unbound=1,audio_source_held=0))
        else:raise AssertionError("Unexpected collector command "+command)
        return len(payload)
    # Flush модели не подтверждает применение PAD или завершение Worker.
    def flush(self):pass
    # Публикует текущую маску без примеси физического ввода.
    def input(self,frame):
        self.emit("NES_INPUT",dict(generation=9,frame=frame,xbox_raw=0,xbox_effective=0,serial=self.mask,touch=0,applied=self.mask,contacts=0,ids="0000",poll_ok=1,suppressed=0,action="none"))
    # Достраивает только полные 120-frame отчёты; C6 получает модель device_us.
    def reports_to(self,frame):
        while self.report+120<=frame:
            self.report+=120
            f=dict(game=3,frames=self.report,produced_frames=self.report,submitted_frames=self.report,samples=120,generation=9,mapper=self.movie["mapper"],region=self.movie["region"],core_us=1000,producer_wall_us=1100,consumer_active_us=500,dsp_us=100)
            if self.c6:f["device_us"]=1000000+self.report*16667
            self.emit("NES_OUTPUT_FRAME_PIPELINE_TIMES",f)
    # Разделяет приём PAD, его применение и последующий report progress.
    def read(self,n):
        # Сначала Main принимает уже переданный initial batch; frame0 ещё не dispatch.
        if self.playing and not self.initial_input_done:
            if self.hardware and self.hardware[0][0]==0:
                frame,mask=self.hardware.pop(0);self.mask=mask
                self.emit("NES_PAD_APPLY",dict(frame=0,scheduled=0,mask=mask))
            self.input(0);self.initial_input_done=True
        if not self.lines and self.playing:
            if self.hardware:
                frame,mask=self.hardware.pop(0);self.reports_to(frame);self.frame=frame;self.mask=mask
                if not(self.fault=="drop_apply"and frame==self.movie["edges"][-1]["frame"]):
                    self.emit("NES_PAD_APPLY",dict(frame=frame,scheduled=frame,mask=mask))
                self.input(frame)
            else:self.frame=self.movie["end_frame"];self.reports_to(self.frame)
        if not self.lines:return b""
        result=b"".join(self.lines);self.lines=[];return result

def collector(name):
    """Загружает настоящий collector, не вызывая его main и не импортируя serial."""
    path=pathlib.Path(__file__).with_name(name)
    spec=importlib.util.spec_from_file_location(name.replace(".","_"),path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

# Проверяет настоящий collector с отдельной моделью устройства и всеми edges.
class CollectorRollingIntegration(unittest.TestCase):
    # Подменяет только транспорт; parser и run_game остаются настоящими.
    def link(self,module,route,c6,fault=None):
        obj=module.Link.__new__(module.Link);obj.port=CollectorUART(route,c6,fault);obj.log=io.BytesIO();obj.pending=b""
        obj.rows=[];obj.packets=[];obj.commands=[];obj.faults=[];obj.consumed=0;obj.expected_volume=60
        return obj
    # Сборщик подаёт полный поток и откладывает служебные запросы до конца измеряемого участка.
    def test_c6_collector_full_stream_and_no_status_tasks_in_ROI(self):
        for name,c6 in[("qualify_c6_device.py",True)]:
            with self.subTest(collector=name):
                module=collector(name);route=movie(90);link=self.link(module,route,c6)
                result=module.run_game(link,route,dict(index=3))
                self.assertEqual(result["errors"],[])
                self.assertTrue(result["status"].startswith("PASS"))
                expected=[(e["frame"],e["mask"])for e in route["edges"]]
                self.assertEqual(result["applied_edges"],expected);self.assertEqual(result["schedule_ACKs"],expected)
                self.assertEqual(result["rolling_queue"]["peak_reserved"],16);self.assertEqual(result["rolling_queue"]["reserved"],0)
                self.assertEqual(link.port.peak,16)
                self.assertEqual(len([s for s,f in link.port.writes if s.startswith("padat ")]),90)
                for command,frame in link.port.writes:
                    if route["baseline_report"]<=frame<route["roi_reports"][-1]:
                        self.assertTrue(command.startswith("padat "),command)
    # Имитирует короткую запись и потерю ACK или APPLY и проверяет остановку сеанса.
    def test_short_write_and_dropped_ACK_APPLY_hold_without_retry(self):
        for fault in["short","drop_ack","drop_apply"]:
            with self.subTest(fault=fault):
                module=collector("qualify_c6_device.py");route=movie(40);link=self.link(module,route,True,fault)
                result=module.run_game(link,route,dict(index=3))
                self.assertEqual(result["status"],"HOLD")
                self.assertTrue(result["errors"])
                actual=[s for s,f in link.port.writes if s.startswith("padat ")]
                self.assertEqual(len(actual),len(set(actual)))
                self.assertFalse(any(s=="reset"for s,f in link.port.writes))
    def test_single_deferred_batch_passes(self):
        """Режим без rolling не получает новые требования или бесконечную deferred логику."""
        for name,c6 in[("qualify_c6_device.py",True)]:
            module=collector(name);route=movie(18,initial=16);route["queue_plan"].pop("mode");route["queue_plan"]["latest_safe_progress_frame"]=200
            link=self.link(module,route,c6);result=module.run_game(link,route,dict(index=3))
            self.assertEqual(result["errors"],[])
            self.assertIsNone(result["rolling_queue"])
            self.assertEqual(len(result["applied_edges"]),18)

    def test_actual_new_mapper_manifests_full_stream_if_available(self):
        """Реальные347/347/468 immutable edges проверяются целиком, без требований к существованию build послеClean."""
        root=pathlib.Path(__file__).resolve().parent.parent
        path = root / "tools/qualification-movies.json"
        routes = [g for g in json.loads(path.read_text(encoding="utf-8"))["games"] if g["mapper"] in (69, 85)]
        self.assertEqual({g["mapper"]for g in routes},{69,85})
        for route in routes:
            expected=[(e["frame"],e["mask"])for e in route["edges"]]
            self.assertGreater(len(expected),300)
            for name,c6 in[("qualify_c6_device.py",True)]:
                with self.subTest(crc=route["crc32"],collector=name):
                    module=collector(name);link=self.link(module,route,c6);result=module.run_game(link,route,dict(index=3))
                    self.assertEqual(result["errors"],[])
                    self.assertEqual(result["applied_edges"],expected)
                    self.assertEqual(result["schedule_ACKs"],expected)
                    self.assertLessEqual(result["rolling_queue"]["peak_reserved"],16)
                    self.assertEqual(len([s for s,f in link.port.writes if s.startswith("padat ")]),len(expected))

    # Ненулевая начальная маска выявляется до записи первой команды в порт.
    def test_frame0_nonzero_refused_before_any_UART_write(self):
        for name,c6 in[("qualify_c6_device.py",True)]:
            module=collector(name);route=movie(40);route["edges"][0]=dict(frame=0,mask=1);route["queue_plan"]["initial"][0]="0:1";link=self.link(module,route,c6)
            with self.assertRaisesRegex(ValueError,"frame0"):module.run_game(link,route,dict(index=3))
            self.assertEqual(link.port.writes,[])

if __name__=="__main__":unittest.main()

