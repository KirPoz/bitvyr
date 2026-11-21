"""Явные UART схемы двух Frame backend; не переводит и не дополняет исходный RAW."""
from dataclasses import dataclass


@dataclass(frozen=True)
class FrameSchema:
    """Хранит идентичность producer/consumer отдельно от измеренной частоты кадров."""
    status: str
    perf: str
    owner: str
    consumer: str


SCHEMAS = {
    'observable': FrameSchema('NES_FRAME_PIPELINE_BENCH_STATUS ', 'NES_FRAME_PIPELINE_BENCH ',
                              'observable', 'actual_core'),
    'output': FrameSchema('NES_OUTPUT_FRAME_PIPELINE_STATUS ', 'NES_OUTPUT_FRAME_PIPELINE_TIMES ',
                          'timing_fetch', 'output_compositor'),
}


def schema(name):
    """Отказывается от неизвестного backend, вместо автоматического угадывания по логу."""
    try:
        return SCHEMAS[name]
    except (KeyError, TypeError) as error:
        raise ValueError('Unknown Frame backend: ' + str(name)) from error


def validate_bound(state, name, *, allow_sparse_ram=False):
    """Проверяет фактический bound STATUS; output не получает отсутствующий read witness."""
    selected = schema(name)
    if state.get('owner') != selected.owner or state.get('consumer') != selected.consumer:
        raise ValueError('Frame producer/consumer identity mismatch')
    if state.get('game_fps_claim') is not False:
        raise ValueError('Unexpected game FPS claim')
    if name == 'output':
        version=state.get('schema_version')
        if (type(version) is not int or version not in (2,3) or state.get('cpu_snapshot') != 'actual_primary_cut' or
            state.get('read_witness') != 'unavailable' or state.get('effective_tail') != 'off' or
            type(state.get('read_count')) is not int or state.get('read_count') != 0 or state.get('read_hash') != '0000000000000000'):
            raise ValueError('Output schema or unavailable read witness mismatch')
        if version==3:
            if allow_sparse_ram is not True:
                raise ValueError('Sparse RAM schema requires explicit non-parity opt-in')
            validate_sparse_ram(state)


def validate_sparse_ram(state):
    """Проверяет nullable RAM/key, не подставляя ноль и не принимая stale witness."""
    if (state.get('ram_policy')!='sparse120' or type(state.get('ram_requested'))is not bool or
        type(state.get('ram_valid'))is not bool or 'ram_hash'not in state or 'ram_key'not in state):
        raise ValueError('Sparse RAM metadata is missing or mistyped')
    status=state.get('ram_status');key=state['ram_key'];value=state['ram_hash']
    if status=='unavailable':
        if state['ram_requested']or state['ram_valid']or key is not None or value is not None:
            raise ValueError('Unavailable RAM must have null key/hash')
        return
    keys=set('producer rom rom_bytes rom_epoch lease_generation transport_epoch region sequence input_frame slot app_generation base_cycle end_cycle ppu_frame'.split())
    if not isinstance(key,dict)or set(key)!=keys:
        raise ValueError('RAM key is incomplete or ambiguous')
    if any(type(v)is not int or v<0 for k,v in key.items()if k!='region'):
        raise ValueError('RAM key numbers must be unsigned integers')
    if any(not key[k]for k in ('producer','rom','rom_bytes','rom_epoch','lease_generation','transport_epoch','app_generation')):
        raise ValueError('RAM key identity must be nonzero')
    if (key['region']not in ('NTSC','PAL')or key['region']!=state.get('region')or
        key['sequence']!=key['input_frame']or key['input_frame']!=state.get('input_frame')or
        key['slot']!=(key['sequence']&1)or key['ppu_frame']!=state.get('frames')or
        key['app_generation']!=state.get('generation')or key['end_cycle']!=state.get('cpu_cycles')or
        key['base_cycle']>=key['end_cycle']):
        raise ValueError('RAM key does not name this accepted cut')
    wanted=key['sequence']%120==119
    if state['ram_requested']is not wanted or state['ram_valid']is not wanted:
        raise ValueError('Sparse RAM request/validity is not its deterministic cadence')
    if not wanted:
        if status!='not_requested'or value is not None:raise ValueError('Unrequested RAM hash must remain null')
    elif status=='unobserved':
        if value is not None:raise ValueError('Unobserved RAM hash must remain null')
    elif status!='available'or type(value)is not str or len(value)!=16 or any(c not in '0123456789abcdef'for c in value):
        raise ValueError('Available RAM requires a full hexadecimal witness')


def validate_perf(row, name):
    """Проверяет статические output tags, сохраняя прежний формат observable без новых требований."""
    schema(name)
    if name == 'output' and any(row.get(k) != v for k, v in {
        'schema': '2', 'producer': 'timing_fetch', 'consumer': 'output_compositor',
        'read_witness': 'unavailable', 'effective_tail': 'off',
    }.items()):
        raise ValueError('Output PERF schema mismatch')
