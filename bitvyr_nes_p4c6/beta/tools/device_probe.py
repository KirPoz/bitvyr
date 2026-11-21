"""Отправляет ограниченные runtime-команды и сохраняет исходную UART-телеметрию."""
from pathlib import Path
import argparse
import hashlib
import json
import math
import re
import sys
import time
try:
    from input_movie import MAX_FRAME, parse_edges
except ModuleNotFoundError:
    from tools.input_movie import MAX_FRAME, parse_edges


def parse_args(argv=None):
    """Проверяет CLI до открытия порта, сохраняя нулевую нумерацию ввода.

    argv — список аргументов либо None для sys.argv. --until-frame обязан быть
    в1..MAX_FRAME, тогда как input movie может содержать edge кадра0. Возвращает
    Namespace; несовместимые варианты ввода завершаются argparse SystemExit.
    """
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',default='COM4')
    p.add_argument('--game',type=int)
    p.add_argument('--seconds',type=float,default=25)
    p.add_argument('--status-interval',type=float,default=5,
                   help='Seconds between state snapshots; use a longer interval for long deterministic intros')
    p.add_argument('--quiet-fps-endpoint',type=int,action='append',default=[],
                   help='Avoid state snapshots during the 120-frame FPS window ending here, plus one preceding report block')
    p.add_argument('--until-frame',type=int,
                   help='Stop after this completed guest frame; --seconds remains a hard timeout; requires --game')
    p.add_argument('--start-at',type=float,action='append',default=[])
    p.add_argument('--start-frame',type=int,action='append',default=[],
                   help='Schedule a three-frame Start pulse at emulated input frame N; requires --game (up to 8)')
    p.add_argument('--pad-frame',action='append',default=[],metavar='FRAME:MASK',
                   help='Exact held-mask input edge at zero-based guest frame; max16; exclusive with Start options')
    p.add_argument('--log',type=Path,default=Path('build/validation/device-probe.log'))
    p.add_argument('--leave-playing',action='store_true')
    p.add_argument('--task-stats',action='store_true',help='Record FreeRTOS task counters before and after the game')
    a = p.parse_args(argv)
    if not math.isfinite(a.seconds) or a.seconds <= 0:p.error('--seconds must be finite and greater than zero')
    if not math.isfinite(a.status_interval) or not 1 <= a.status_interval <= 120:
        p.error('--status-interval must be finite and in 1..120 seconds')
    if a.game is not None and a.game < 0:p.error('--game must be zero or greater')
    if a.start_frame and a.game is None:p.error('--start-frame requires --game')
    try:a.pad_edges=parse_edges(a.pad_frame)
    except ValueError as error:p.error(str(error))
    if a.pad_frame and (a.game is None or a.start_frame or a.start_at):
        p.error('--pad-frame requires --game and cannot mix with Start options')
    if a.until_frame is not None:
        if a.game is None:p.error('--until-frame requires --game')
        if not 1 <= a.until_frame <= MAX_FRAME:p.error(f'--until-frame must be in 1..{MAX_FRAME}')
        if any(n+3 >= a.until_frame for n in a.start_frame):p.error('--until-frame must follow every scheduled Start release')
        if any(n >= a.until_frame for n, _ in a.pad_edges):p.error('--until-frame must follow every input edge')
    if len(a.start_frame)>8:p.error('--start-frame accepts at most 8 pulses (16 queued events)')
    if any(n<0 or n>(1<<64)-4 for n in a.start_frame):p.error('--start-frame must fit unsigned 64-bit N and N+3')
    if a.quiet_fps_endpoint:
        if a.until_frame is None:p.error('--quiet-fps-endpoint requires --until-frame and --game')
        if len(a.quiet_fps_endpoint)>64 or len(set(a.quiet_fps_endpoint))!=len(a.quiet_fps_endpoint):
            p.error('--quiet-fps-endpoint accepts at most64 distinct endpoints')
        if any(n<120 or n%120 or n>a.until_frame for n in a.quiet_fps_endpoint):
            p.error('--quiet-fps-endpoint must be a multiple of120 within120..until-frame')
    return a


def in_quiet_fps_window(frame, endpoints):
    # PERF is issued every120 frames. Exclude the preceding block as well,
    # so a queued snapshot can finish before the measured block starts.
    """Исключает measured120 и предыдущее120-frame окно, чтобы queued snapshot успел завершиться."""
    return any(end-240 <= frame < end for end in endpoints)


class FrameGoal:
    """Принимает завершённый status текущего запуска; счётчики progress не означают завершение/FPS."""
    def __init__(self, game, target, *, track_performance=False):
        """Создаёт новый launch/frame-goal scope с пустым буфером, без открытия UART."""
        self.game, self.target = game, target
        self.pending = bytearray()
        self.launched = self.reached = False
        self.track_performance = track_performance
        self.progress_frames = 0

    def feed(self, data):
        """Принимает только текущий launch/status; progress не означает completion, длинная строка ограничена64KiB."""
        self.pending.extend(data)
        while b'\n' in self.pending:
            line, _, rest = self.pending.partition(b'\n')
            self.pending = bytearray(rest)
            # Явный отказ загрузки/исполнения не может достичь frame goal.
            # Остановить ожидание сразу и оставить caller штатный emergency cleanup.
            failure = re.search(rb'\b(?:ROM_READ_FAIL|NES_RUNTIME_FAIL|NES_WORKER_FAIL)\b', line)
            if failure:
                raise RuntimeError('device failure while awaiting frame goal: ' +
                                   line.decode('utf8', errors='replace').strip())
            launch = re.search(rb'NES_GAME_START index=(\d+)', line)
            if launch:
                if self.launched or int(launch[1]) != self.game:
                    raise RuntimeError('unexpected game launch while awaiting frame goal')
                self.launched = True
            if self.launched and self.track_performance:
                perf = re.search(rb'NES_(?:PERF|OUTPUT_FRAME_PIPELINE_TIMES) game=(\d+) frames=(\d+) ', line)
                if perf:
                    if int(perf[1]) != self.game:
                        raise RuntimeError('performance game changed while awaiting frame goal')
                    self.progress_frames=max(self.progress_frames,int(perf[2]))
            marker = next((value for value in (b'NES_STATUS ', b'NES_OUTPUT_FRAME_PIPELINE_STATUS ')
                           if value in line), None)
            if self.launched and marker is not None:
                state = json.loads(line.split(marker, 1)[1])
                # После menu Output-контекст уже unbound и штатно не содержит game.
                if state.get('playing') is False and 'game' not in state:
                    continue
                if 'game' not in state:
                    raise RuntimeError('active status lacks game while awaiting frame goal')
                if state['game'] != self.game:
                    raise RuntimeError('game changed while awaiting frame goal')
                if state['playing']:
                    self.progress_frames=max(self.progress_frames,state['frames'])
                if state['playing'] and state['frames'] >= self.target:
                    self.reached = True
        if len(self.pending) > 65536:
            raise RuntimeError('unterminated UART line while awaiting frame goal')


def capture(a):
    """Записывает RAW выбранного порта без reset; исходный отказ сохраняется после попытки release/close."""
    import serial

    out=bytearray()
    s=None
    opened=False
    completed=False
    original_error=None
    quiet_endpoints=getattr(a,'quiet_fps_endpoint',[])
    goal=FrameGoal(a.game,a.until_frame,track_performance=bool(quiet_endpoints)) if a.until_frame is not None else None
    sent_commands=[]

    def send(command):
        """Передаёт одну ASCII команду и фиксирует фактические bytes для quiet-window receipt."""
        payload=(command+'\n').encode('ascii')
        written=s.write(payload)
        s.flush()
        if quiet_endpoints:
            sent_commands.append(dict(command=command,bytes=written,expected_bytes=len(payload),rx_bytes_before=len(out)))

    def read(log):
        """Сохраняет полученные bytes до разбора и передаёт тот же поток текущему FrameGoal."""
        data=s.read(8192)
        if data:
            log.write(data)
            log.flush()
            out.extend(data)
            if goal is not None:goal.feed(data)

    a.log.parent.mkdir(parents=True,exist_ok=True)
    with a.log.open('wb') as f:
        try:
            s=serial.Serial(port=None,baudrate=115200,timeout=.1,write_timeout=1)
            s.dtr=False;s.rts=False;s.port=a.port;s.open()
            opened=True
            send('status')
            if a.task_stats:send('tasks')
            if a.game is not None:send(f'run {a.game}')
            for frame in a.start_frame:
                send(f'padat {frame} 8')
                send(f'padat {frame+3} 0')
            for frame,mask in a.pad_edges:send(f'padat {frame} {mask}')
            begin=time.monotonic();next_status=a.status_interval;events=[]
            for moment in a.start_at:events.extend([(moment,'pad 8'),(moment+.25,'pad 0')])
            events.sort()
            while time.monotonic()-begin<a.seconds:
                elapsed=time.monotonic()-begin
                while events and elapsed>=events[0][0]:send(events.pop(0)[1])
                if quiet_endpoints:
                    # Drain progress before deciding to enqueue an expensive
                    # snapshot. PERF informs exclusion only, never completion.
                    read(f)
                    if goal.reached:break
                    if elapsed>=next_status and not in_quiet_fps_window(goal.progress_frames,quiet_endpoints):
                        send('status');next_status=elapsed+a.status_interval
                else:
                    if elapsed>=next_status:send('status');next_status+=a.status_interval
                    read(f)
                if goal is not None and goal.reached:break
            if goal is not None and not goal.reached:
                raise TimeoutError(f'game did not reach completed frame {a.until_frame} within {a.seconds} seconds')
            if getattr(a,'cleanup_commands',None) is None:
                send('pad 0')
                if a.task_stats:send('tasks')
                if not a.leave_playing:send('menu')
                send('status')
            else:
                assert a.cleanup_commands==('pad 0','settings','status','menu','status')
                if a.task_stats:send('tasks')
                for command in a.cleanup_commands:send(command)
            end=time.monotonic()+1
            while time.monotonic()<end:read(f)
            completed=True
        except BaseException as error:
            original_error=error
            raise
        finally:
            cleanup_errors=[]
            emergency_attempted=False

            def emergency_release():
                """Пытается отпустить UART кнопки и вернуться в menu в прежнем порту; ошибки сохраняются для caller."""
                nonlocal emergency_attempted
                emergency_attempted=True
                if s is not None and getattr(s,'is_open',opened):
                    # A disconnect may make either write fail; still try both.
                    for command in ('pad 0','menu'):
                        try:send(command)
                        except BaseException as error:cleanup_errors.append(error)

            if not completed or original_error is not None:emergency_release()
            if s is not None:
                try:s.close()
                except BaseException as error:
                    cleanup_errors.append(error)
                    # A failed close can leave a live port after an otherwise
                    # normal --leave-playing run. Release before one last close.
                    if not emergency_attempted:emergency_release()
                    try:s.close()
                    except BaseException as retry_error:cleanup_errors.append(retry_error)
            try:f.flush()
            except BaseException as error:cleanup_errors.append(error)
            if quiet_endpoints:
                try:
                    record=dict(completed=completed and original_error is None and not cleanup_errors,
                                log_sha256=hashlib.sha256(out).hexdigest(),
                                quiet_fps_endpoints=quiet_endpoints,commands=sent_commands,
                                probe_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
                    a.log.with_suffix('.commands.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
                except BaseException as error:cleanup_errors.append(error)
            if cleanup_errors:
                if original_error is None:raise cleanup_errors[0]
                for error in cleanup_errors:
                    note=f'Probe cleanup also failed: {type(error).__name__}: {error}'
                    if hasattr(original_error,'add_note'):original_error.add_note(note)
                    else:print(note,file=sys.stderr)
    return out


def main(argv=None):
    """Запускает явно заданный probe и печатает отфильтрованные сообщения, сохраняя полный RAW."""
    out=capture(parse_args(argv))
    for line in out.decode('utf-8',errors='replace').splitlines():
        if any(x in line for x in ['NES_', 'XBOX_','Guru','assert','abort','timeout']):print(line[:500])
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
