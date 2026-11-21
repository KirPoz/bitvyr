"""Проверяет отсутствие синхронных state observers в измеряемом UART FPS-окне."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def _initial_unbound_output(payload):
    """Принимает только точный idle handshake, не статус Frame Output игры."""
    def unique_fields(pairs):
        """Отвергает повторные ключи, скрывающие неоднозначный protocol record."""
        state={}
        for key,value in pairs:
            if key in state:raise ValueError('duplicate initial Output STATUS field')
            state[key]=value
        return state
    state=json.loads(payload,object_pairs_hook=unique_fields)
    fields={'playing':(bool,False),'frames':(int,0),'generation':(int,0),
            'owner':(str,'unbound'),'image_available':(bool,False),'game_fps_claim':(bool,False)}
    if not isinstance(state,dict) or set(state)!=set(fields) or any(
            type(state[key])is not kind or state[key]!=value for key,(kind,value) in fields.items()):
        raise ValueError('initial Output STATUS is not exact neutral unbound schema')
    return state


def audit(raw,game,endpoints,commands,*,status_tag='NES_STATUS ',perf_tag='NES_PERF ',error_tag='NES_STATUS_ERROR',allow_unbound=False,allow_initial_unbound_output_status=False):
    """Проверяет ordered observer fence; новый opt-in допускает один idle до launch."""
    errors=[];windows=[];perfs={};observers=[];faults=[];launches=[];initial_output=0
    if not endpoints or len(set(endpoints))!=len(endpoints) or any(type(n)is not int or n<120 or n%120 for n in endpoints):
        errors.append('invalid or empty endpoints')
    if commands.get('completed') is not True:errors.append('capture did not complete')
    if commands.get('log_sha256')!=hashlib.sha256(raw).hexdigest():errors.append('command record/log SHA mismatch')
    if sorted(commands.get('quiet_fps_endpoints',[]))!=sorted(endpoints):errors.append('command record endpoint mismatch')
    requests=commands.get('commands',[])
    if not requests or requests[0].get('command')!='status' or requests[-1].get('command')!='status':
        errors.append('missing initial/final STATUS request evidence')
    if any(row.get('bytes')!=row.get('expected_bytes') for row in requests):errors.append('incomplete command write')
    expected_responses=sum(row.get('command')=='status' for row in requests)
    for number,line in enumerate(raw.decode('utf-8',errors='strict').splitlines(),1):
        launch=re.search(r'NES_GAME_START index=(\d+)',line)
        if launch:launches.append((number,int(launch[1])))
        perf=re.search(re.escape(perf_tag)+r'game=(\d+) frames=(\d+) ',line)
        if perf and int(perf[1])==game:perfs.setdefault(int(perf[2]),[]).append(number)
        initial_tag='NES_OUTPUT_FRAME_PIPELINE_STATUS '
        if allow_initial_unbound_output_status and initial_tag in line:
            try:
                state=_initial_unbound_output(line.split(initial_tag,1)[1])
                if launches or observers:raise ValueError('initial Output STATUS after launch or another observer')
                observers.append((number,state));initial_output+=1
            except ValueError as error:errors.append(f'malformed initial Output STATUS at line{number}: {error}')
        elif status_tag in line:
            try:
                state=json.loads(line.split(status_tag,1)[1])
                unbound=allow_unbound and state.get('owner')=='unbound' and state.get('playing') is False and state.get('frames')==0 and state.get('image_available') is False
                if type(state.get('playing')) is not bool or (type(state.get('game')) is not int and not unbound) or type(state.get('frames')) is not int:
                    raise ValueError('invalid observer fields')
                observers.append((number,state))
            except (ValueError,AttributeError) as error:errors.append(f'malformed STATUS at line{number}: {error}')
        if error_tag in line or 'NES_RUNTIME_FAIL' in line:faults.append(number)
    if len(launches)!=1 or launches[0][1]!=game:errors.append('expected exactly one launch of the selected game')
    else:
        launch_line=launches[0][0]
        perfs={frame:[n for n in lines if n>launch_line] for frame,lines in perfs.items()}
        if sum(n<launch_line for n,state in observers)!=1:errors.append('initial STATUS handshake missing or unsolicited prelaunch observers')
        if any(n>launch_line and state.get('game',game)!=game for n,state in observers):errors.append('STATUS game changed after launch')
        for n,line in enumerate(raw.decode('utf-8').splitlines(),1):
            perf=re.search(re.escape(perf_tag)+r'game=(\d+) frames=(\d+) ',line)
            if n>launch_line and perf and int(perf[1])!=game:errors.append('PERF game changed after launch')
    if not raw.endswith(b'\n'):errors.append('unterminated UART capture')
    if len(observers)+sum(error_tag in line for line in raw.decode('utf-8').splitlines())!=expected_responses:
        errors.append('STATUS query/response count mismatch; loss or unsolicited observers cannot qualify')
    if faults:errors.append('runtime/status error markers present')
    for end in endpoints:
        begin_lines=perfs.get(end-120,[]) if end!=120 else [x[0] for x in launches]
        end_lines=perfs.get(end,[])
        row=dict(endpoint=end,previous_endpoint=end-120,passed=False)
        if len(begin_lines)!=1 or len(end_lines)!=1 or begin_lines[0]>=end_lines[0]:
            row['error']='missing, duplicate or unordered PERF boundaries'
        else:
            start,finish=begin_lines[0],end_lines[0]
            found=[n for n,state in observers if start<n<finish]
            row.update(start_line=start,end_line=finish,observer_lines=found,passed=not found)
            if found:row['error']='synchronous STATUS inside measured wall-time interval'
        windows.append(row)
    report=dict(passed=not errors and all(w['passed'] for w in windows),game=game,
                errors=errors,windows=windows,status_requests=expected_responses,status_responses=len(observers),
                log_sha256=hashlib.sha256(raw).hexdigest(),
                scope='Main-actor STATUS/PERF order plus complete query/response evidence; PERF never proves capture completion')
    if allow_initial_unbound_output_status:
        report['initial_unbound_output_status']={'opt_in':True,'accepted':initial_output,
            'scope':'One exact prelaunch idle response only; no postlaunch Frame Output/FPS acceptance'}
    return report


def main():
    """Разбирает сохранённый RAW и command receipt; UART/firmware не запускает."""
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('log',type=Path);p.add_argument('--commands',type=Path,required=True)
    p.add_argument('--game',type=int,required=True);p.add_argument('--endpoint',type=int,action='append',required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--allow-initial-unbound-output-status',action='store_true',
                   help='Допустить один точный начальный idle Output-status до NES_GAME_START; не разрешает Output после launch')
    a=p.parse_args()
    if a.game<0 or len(set(a.endpoint))!=len(a.endpoint) or any(n<120 or n%120 for n in a.endpoint):
        p.error('require nonnegative game and distinct positive120-frame endpoints')
    report=audit(a.log.read_bytes(),a.game,a.endpoint,json.loads(a.commands.read_text(encoding='utf-8')),
                 allow_initial_unbound_output_status=a.allow_initial_unbound_output_status)
    report['command_record_sha256']=hashlib.sha256(a.commands.read_bytes()).hexdigest()
    report['auditor_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    a.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report))
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
