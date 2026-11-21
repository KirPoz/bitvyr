"""Offline-проверки пассивной записи: serial подменён и реальный порт не открывается."""
from pathlib import Path
import argparse
import importlib.util
import json
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

SPEC=importlib.util.spec_from_file_location("passive_capture_under_test",Path(__file__).with_name("record_playthrough.py"))
recorder=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(recorder)


class FakeSerial:
    """Только модель транспорта: любая запись или очистка UART немедленно проваливает тест."""
    # Создаёт закрытый fake-дескриптор и журнал порядка управляющих действий.
    def __init__(self,owner,**kwargs):
        self.owner=owner;self.kwargs=kwargs;self.port=kwargs.get("port")
        self._dtr=True;self._rts=True;self.is_open=False;self.closed=False
        self.reads=0;self.write_calls=0;owner.events.append(("construct",kwargs))
    # Читает или изменяет только модель DTR; порядок фиксируется без аппаратного reset.
    @property
    def dtr(self):return self._dtr
    # Читает или изменяет только модель DTR; порядок фиксируется без аппаратного reset.
    @dtr.setter
    def dtr(self,value):self.owner.events.append(("dtr",value));self._dtr=value
    # Читает или изменяет только модель RTS; реальный UART не используется.
    @property
    def rts(self):return self._rts
    # Читает или изменяет только модель RTS; реальный UART не используется.
    @rts.setter
    def rts(self,value):self.owner.events.append(("rts",value));self._rts=value
    # Фиксирует DTR/RTS до открытия и воспроизводит явно заданный open fault.
    def open(self):
        self.owner.events.append(("open",self.port,self.dtr,self.rts))
        if self.owner.open_error:raise self.owner.open_error
        self.is_open=True
    # Возвращает подготовленный RX или заданную ошибку; конец script запрашивает stop.
    def read(self,size):
        self.owner.events.append(("read",size));self.reads+=1
        if not self.owner.script:raise AssertionError("Fake script exhausted without a stop/error")
        item=self.owner.script.pop(0)
        if isinstance(item,BaseException):raise item
        if not self.owner.script:recorder.request_stop(self.owner.folder)
        return item
    # Пассивному recorder запрещён любой write, даже если fake мог бы его принять.
    def write(self,payload):
        self.write_calls+=1
        raise AssertionError("PASSIVE recorder attempted UART write")
    # Любой сброс входного буфера проваливает проверку сохранности RAW.
    def reset_input_buffer(self):raise AssertionError("PASSIVE recorder discarded UART input")
    # Запрещает скрытую очистку выходного UART у пассивного recorder.
    def reset_output_buffer(self):raise AssertionError("PASSIVE recorder reset UART output")
    # Запрещает break/reset через транспорт пассивной записи.
    def send_break(self,*args,**kwargs):raise AssertionError("PASSIVE recorder sent UART break/reset")
    # Закрывает модель даже при заданной ошибке close, сохраняя событие для проверки.
    def close(self):
        self.owner.events.append(("close",));self.closed=True;self.is_open=False
        if self.owner.close_error:raise self.owner.close_error


# Подменяет serial и USB inventory целиком, чтобы тест не зависел от оборудования.
class Transport:
    # Хранит сценарий RX, USB identity и явно заданные ошибки открытия/закрытия.
    def __init__(self,folder,script,*,vid=0x1a86,pid=0x55d3,serial_number="TEST-USB",open_error=None,close_error=None):
        self.folder=folder;self.script=list(script);self.events=[];self.instances=[]
        self.open_error=open_error;self.close_error=close_error
        self.descriptor=types.SimpleNamespace(device="COM4",vid=vid,pid=pid,serial_number=serial_number)
    # Создаёт fake-дескриптор и сохраняет его для проверки освобождения.
    def Serial(self,**kwargs):
        port=FakeSerial(self,**kwargs);self.instances.append(port);return port
    # Возвращает изолированные fake-модули serial и list_ports.
    def modules(self):
        serial=types.ModuleType("serial");serial.Serial=self.Serial
        tools=types.ModuleType("serial.tools");ports=types.ModuleType("serial.tools.list_ports")
        ports.comports=lambda:[self.descriptor]
        serial.tools=tools;tools.list_ports=ports
        return {"serial":serial,"serial.tools":tools,"serial.tools.list_ports":ports}


# Формирует явные параметры записи без CLI и реального открытия COM.
def arguments(folder,**changes):
    values=dict(output=folder,port="COM4",vid=0x1a86,pid=0x55d3,serial="TEST-USB",baud=115200,max_seconds=None)
    values.update(changes);return argparse.Namespace(**values)

# Читает сохранённые JSONL-записи без изменения исходного журнала.
def load_lines(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


# Проверяет RAW durability, пассивность и освобождение транспорта при отказах.
class CaptureGuards(unittest.TestCase):
    # Исполняет настоящий recorder только внутри подменённых serial-модулей.
    def run_capture(self,folder,script,**transport_options):
        transport=Transport(folder,script,**transport_options)
        with patch.dict(sys.modules,transport.modules()):
            state=recorder.record(arguments(folder))
        return state,transport
    # Требует закрытый порт, отсутствие write/reset и безопасный порядок DTR/RTS.
    def assert_passive_closed(self,state,transport):
        self.assertFalse(state["COM_open"]);self.assertEqual(state["commands_sent"],0)
        self.assertFalse(state["reset_requested"]);self.assertFalse(state["flash"])
        self.assertEqual(len(transport.instances),1)
        self.assertTrue(transport.instances[0].closed)
        self.assertEqual(transport.instances[0].write_calls,0)
        self.assertEqual(transport.events[0][0],"construct")
        opened=next(event for event in transport.events if event[0]=="open")
        self.assertEqual(opened,("open","COM4",False,False))
        self.assertLess(transport.events.index(("dtr",False)),transport.events.index(opened))
        self.assertLess(transport.events.index(("rts",False)),transport.events.index(opened))
    # Проверяет уровни DTR и RTS при открытии и закрытие записи по файлу остановки.
    def test_open_control_lines_before_open_and_stop_file_clean_closure(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)/"capture"
            state,transport=self.run_capture(folder,[b"NES_INPUT generation=2 frame=0 serial=0 applied=0 action=none\r\n"])
            self.assertEqual(state["status"],"stopped");self.assert_passive_closed(state,transport)
            self.assertTrue((folder/"stop.request").is_file())
            self.assertEqual(json.loads((folder/"state.json").read_text(encoding="utf-8")),state)
            events=load_lines(folder/"events.jsonl")
            self.assertEqual(events[0]["kind"],"opened");self.assertEqual(events[-1]["kind"],"stopped")
            self.assertIn("ended_utc",state)
    # Сверяет исходный поток побайтно при обрыве строки, ANSI-кодах и повреждённом UTF-8.
    def test_raw_partial_ANSI_malformed_and_non_UTF8_bytes_are_never_repaired(self):
        blocks=[b'\x1b[32mI (100) nes_app: NES_SETTINGS {"volume":1}\x1b[0m\r\nNES_SETT',
                b'INGS {"paused":broken}\nunknown byte: \xff\nNES_INPUT generation=2 frame=0 serial=0 applied=0 action=none\npartial-tail\x00\xfe']
        raw=b"".join(blocks)
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)/"capture";state,transport=self.run_capture(folder,blocks)
            self.assertEqual(state["status"],"stopped");self.assert_passive_closed(state,transport)
            self.assertEqual((folder/"raw.log").read_bytes(),raw)
            self.assertEqual(state["raw_bytes"],len(raw));self.assertEqual(state["parse_errors"],1)
            self.assertEqual(state["complete_lines"],4)
            events=load_lines(folder/"events.jsonl")
            self.assertEqual([r["kind"] for r in events].count("parse_error"),1)
            self.assertEqual(events[-1]["partial_tail_bytes"],len(b"partial-tail\x00\xfe"))
            chunks=load_lines(folder/"chunks.jsonl")
            self.assertEqual([(r["begin"],r["end"]) for r in chunks],[(0,len(blocks[0])),(len(blocks[0]),len(raw))])
            self.assertEqual(state["firmware_identity"],"unknown_until_boot_ELF_is_observed")
    # Ошибка прошивки остаётся в журнале вместе с последующим запуском игры.
    def test_firmware_faults_do_not_end_capture_and_later_launch_is_recorded(self):
        blocks=[b"Guru Meditation Error: test panic\r\nNES_WORKER_FAIL reason=test\n",
                b"I (2000) nes_app: NES_GAME_START index=4 name=Castlevania III - Dracula's Curse (USA) mapper=5 region=NTSC crc=7cf70cde source=sd zip=1 size=393232\n",
                b"I (6000) nes_app: NES_OUTPUT_FRAME_PIPELINE_TIMES game=4 frames=120 generation=9 mapper=5 region=NTSC samples=120\n"]
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)/"capture";state,transport=self.run_capture(folder,blocks)
            self.assertEqual(state["status"],"stopped");self.assert_passive_closed(state,transport)
            self.assertEqual(state["firmware_faults"],2);self.assertEqual(state["game_launches"],1)
            self.assertEqual(state["last_frame"]["frames"],120)
            self.assertEqual((folder/"raw.log").read_bytes(),b"".join(blocks))
            self.assertEqual(sum(e["kind"]=="device_fault" for e in load_lines(folder/"events.jsonl")),2)
    # Повторный старт ROM с тем же CRC сохраняет полное имя игры с пробелами.
    def test_repeated_sameCRC_GAME_START_preserves_names_with_spaces(self):
        title="Castlevania III - Dracula's Curse (USA)"
        line=f"I (1234) nes_app: NES_GAME_START index=4 name={title} mapper=5 region=NTSC crc=7cf70cde source=sd zip=1 size=393232\n"
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)/"capture";state,transport=self.run_capture(folder,[line.encode(),line.encode()])
            self.assertEqual(state["status"],"stopped");self.assert_passive_closed(state,transport)
            self.assertEqual(state["game_launches"],2)
            self.assertEqual(state["latest_game"]["name"],title)
            self.assertEqual(state["latest_game"]["index"],4)
            starts=[e for e in load_lines(folder/"events.jsonl") if e["kind"]=="game_start"]
            self.assertEqual(len(starts),2);self.assertEqual([e["fields"]["name"] for e in starts],[title,title])
    # Для сокращённого хеша из UART проверяется отдельная метка наблюдаемого префикса.
    def test_observed_ELF_prefix_is_not_a_full_or_expected_image_claim(self):
        prefix="028b41e0ecb66377"
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)/"capture"
            state,transport=self.run_capture(folder,[b"ESP-ROM:esp32p4-eco5-test\n",f"I (100) cpu_start: ELF file SHA256: {prefix}\n".encode()])
            self.assertEqual(state["status"],"stopped");self.assert_passive_closed(state,transport)
            self.assertEqual(state["observed_boots"],1)
            self.assertEqual(state["firmware_identity"],{"ELF_sha256_prefix":prefix})
            self.assertNotIn("expected_elf",state);self.assertNotIn("full_sha256_verified",state)
            self.assertEqual(len(state["firmware_identity"]["ELF_sha256_prefix"]),16)
    # При несовпадении USB-идентификатора дескриптор порта остаётся закрытым.
    def test_USB_identity_rejected_before_any_port_open(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)/"capture";transport=Transport(folder,[b"unused"],serial_number="OTHER")
            with patch.dict(sys.modules,transport.modules()),self.assertRaisesRegex(ValueError,"USB"):
                recorder.record(arguments(folder))
            self.assertEqual(transport.instances,[])
            self.assertFalse((folder/"raw.log").exists())
    # Ошибка чтения сохраняет уже записанные байты, итоговый статус и закрывает порт.
    def test_read_error_keeps_written_raw_and_closes_COM_with_failed_metadata(self):
        block=b"I (100) nes_app: NES_INPUT generation=2 frame=0 applied=0 action=none\n"
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)/"capture";state,transport=self.run_capture(folder,[block,OSError("fake USB disconnect")])
            self.assertEqual(state["status"],"failed");self.assert_passive_closed(state,transport)
            self.assertIn("fake USB disconnect",state["error"])
            self.assertEqual((folder/"raw.log").read_bytes(),block)
            self.assertEqual(load_lines(folder/"events.jsonl")[-1]["kind"],"failure")
            self.assertEqual(json.loads((folder/"state.json").read_text(encoding="utf-8"))["status"],"failed")
    # Неудачное открытие освобождает дескриптор и записывает ошибку в итог сеанса.
    def test_open_error_still_closes_fake_handle_and_publishes_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)/"capture";state,transport=self.run_capture(folder,[b"unused"],open_error=OSError("fake open error"))
            self.assertEqual(state["status"],"failed");self.assert_passive_closed(state,transport)
            self.assertEqual((folder/"raw.log").read_bytes(),b"")
            self.assertIn("fake open error",state["error"])
    # При переполнении буфера незаконченной строки исходные байты остаются в RAW.
    def test_oversized_partial_drops_only_diagnostics_and_preserves_all_raw(self):
        blocks=[b"x"*32768 for _ in range(9)]+[b"\nI (5) nes_app: NES_GAME_START index=1 name=Battle City (Japan) mapper=0 region=NTSC crc=f599a07e source=sd zip=1 size=24592\n"]
        with tempfile.TemporaryDirectory() as temporary:
            folder=Path(temporary)/"capture";state,transport=self.run_capture(folder,blocks)
            self.assertEqual(state["status"],"stopped");self.assert_passive_closed(state,transport)
            self.assertEqual(state["oversized_partial_lines"],1)
            self.assertEqual((folder/"raw.log").read_bytes(),b"".join(blocks))
            self.assertEqual(state["game_launches"],1)
            self.assertEqual(state["latest_game"]["name"],"Battle City (Japan)")

if __name__=="__main__":unittest.main()

