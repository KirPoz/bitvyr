"""C6 build: новый SDK bootstrap и39 fresh TU по сохранённым ordered argv.

По умолчанию --audit/--prepare не запускают SDK. Исполнение требует --execute-root.
Скрипт не читает прежний build/experiments и никогда не выполняет flash/OTA/COM.
"""
from pathlib import Path
import argparse,hashlib,json,os,re,shutil,subprocess,sys,tempfile,time
PROJECT=Path(__file__).resolve().parents[1]
ROOT=PROJECT.parents[1]

def read(p):
    """Читает durable рецепт; historical paths из provenance не разрешаются как input."""
    return json.loads(p.read_text('utf-8'))

def digest(p):
    """Вычисляет SHA полного файла перед заимствованием или запуском инструмента."""
    with p.open('rb')as f:return hashlib.file_digest(f,'sha256').hexdigest()

def pin(p):
    """Привязывает выход текущей новой попытки, не переименовывая старый ELF."""
    return dict(path=str(p),bytes=p.stat().st_size,sha256=digest(p))

def save(p,v):
    """Создаёт новый receipt; повторная попытка требует нового --out."""
    with p.open('x',encoding='utf-8')as f:json.dump(v,f,ensure_ascii=False,indent=2);f.write('\n')

def checked(ref):
    """Проверяет source pin внутри репозитория; не допускает выхода из source tree."""
    p=(ROOT/ref['path']).resolve();assert p.is_relative_to(ROOT)
    assert p.stat().st_size==ref['bytes']and digest(p)==ref['sha256'],str(p)
    return p

def config_values(path):
    """Читает только logical CONFIG values; порядок/EOL/default-comments не меняют профиль."""
    values={}
    for line in Path(path).read_text('utf-8').splitlines():
        match=re.fullmatch(r'(CONFIG_\w+)=(.*)',line)or re.fullmatch(r'# (CONFIG_\w+) is not set',line)
        if match:values[match.group(1)]=match.group(2)if match.lastindex==2 else'n'
    return values

def verify_config(path,lock):
    """До компиляции отклоняет любой drift полного SDK-generated профиля."""
    value=hashlib.sha256(json.dumps(config_values(path),sort_keys=True,separators=(',',':')).encode()).hexdigest()
    assert value==lock['effective_sdkconfig_sha256'],'Generated sdkconfig changed; review SDK/Kconfig before compiling this worker.'
    return value

def check_external(path,ref):
    """Версия/имя не заменяют проверку всех байтов внешнего executable/archive."""
    path=Path(path).expanduser().resolve()
    assert path.is_file()and path.stat().st_size==ref['bytes']and digest(path)==ref['sha256'],f"Pinned dependency mismatch: {path}; expected SHA256 {ref['sha256']}"
    return path

def choose_tool(name,explicit,ref,candidates):
    """Явный путь не имеет fallback; поиск выбирает только совпавший pin."""
    if explicit is not None:return check_external(explicit,ref)
    tried=[]
    for item in candidates:
        if item is None:continue
        path=Path(item).expanduser().resolve()
        if path in tried:continue
        tried.append(path)
        if path.is_file()and path.stat().st_size==ref['bytes']and digest(path)==ref['sha256']:return path
    raise RuntimeError(f"Cannot find pinned {name}; activate ESP-IDF 6.1 or pass --{name}; expected SHA256 {ref['sha256']}; checked {tried}")

def resolve_environment(args,lock):
    """Разрешает roots при prepare; current ordered recipe сохраняет Windows tool pins."""
    idf=args.idf or(Path(os.environ['IDF_PATH'])if os.environ.get('IDF_PATH')else None)
    if idf is None:raise RuntimeError('Pass --idf or activate ESP-IDF 6.1 (IDF_PATH).')
    idf=idf.expanduser().resolve()
    tools=args.idf_tools or(Path(os.environ['IDF_TOOLS_PATH'])if os.environ.get('IDF_TOOLS_PATH')else None)
    if tools is not None:tools=tools.expanduser().resolve()
    ref=next(row for row in lock['files']if row['root']=='TOOLCHAIN'and row['path']=='bin/riscv32-esp-elf-g++.exe')
    candidates=[shutil.which('riscv32-esp-elf-g++'),shutil.which('riscv32-esp-elf-g++.exe')]
    if tools is not None:candidates.extend(sorted((tools/'tools/riscv32-esp-elf').glob('*/riscv32-esp-elf/bin/riscv32-esp-elf-g++.exe')))
    explicit=args.toolchain/'bin/riscv32-esp-elf-g++.exe'if args.toolchain else None
    compiler=choose_tool('toolchain',explicit,ref,candidates);toolchain=compiler.parent.parent
    if tools is None and len(toolchain.parents)>3 and toolchain.parents[2].name=='tools':tools=toolchain.parents[3]
    if tools is None:raise RuntimeError('Pass --idf-tools or activate IDF_TOOLS_PATH for the SDK bootstrap.')
    candidates=[]
    if os.environ.get('IDF_PYTHON_ENV_PATH'):candidates.extend(Path(os.environ['IDF_PYTHON_ENV_PATH'])/p for p in ['Scripts/python.exe','bin/python'])
    candidates.extend([Path(sys.executable),shutil.which('python')])
    selected={'PYTHON':choose_tool('python',args.python,lock['host_tools']['python'],candidates)}
    for name in ['cmake','ninja']:
        filename=lock['host_tools'][name]['path'];candidates=[shutil.which(name),shutil.which(filename)]
        pattern='tools/cmake/*/bin/'+filename if name=='cmake'else'tools/ninja/*/'+filename
        candidates.extend(sorted(tools.glob(pattern)))
        selected[name.upper()]=choose_tool(name,getattr(args,name),lock['host_tools'][name],candidates)
    selected.update(IDF=idf,TOOLCHAIN=toolchain,IDF_TOOLS=tools);return selected

def external_inputs(lock,variables):
    """Относительные dependency paths остаются внутри выбранного SDK/toolchain."""
    result=[]
    for ref in lock['files']:
        assert ref['root']in['IDF','TOOLCHAIN']and not Path(ref['path']).is_absolute()
        root=Path(variables[ref['root']]).resolve();path=(root/ref['path']).resolve();assert path.is_relative_to(root)
        result.append(pin(check_external(path,ref)))
    return result

def effective_compile_sources(mapping,recipe):
    """Разрешает только явно перечисленные old→current source revisions без смены argv.

    Имя compile41 сохранено для совместимости рецепта; текущие39 TU сохраняют
    исходные ordered flags и historical source pins. Изменённый
    CPP разрешён лишь при exact совпадении старого member/pin, нового projection/pin
    и прежнего source path. Любой неизвестный drift остаётся ошибкой checked().
    """
    members={row['member']:row for row in recipe['rows']}
    revisions=mapping.get('compile_source_revisions',[])
    assert len({row['member']for row in revisions})==len(revisions)
    overrides={}
    for revision in revisions:
        member=revision['member'];assert member in members,member
        row=members[member]
        assert revision['previous']==row['source'],member
        current=revision['current'];assert current['path']==row['source']['path'],member
        argument=row['argv'][row['argv'].index('-c')+1]
        assert argument.startswith('${PROJECTION}/'),member
        projection=argument[len('${PROJECTION}/'):]
        matches=[item for item in mapping['rows']if item['projection']==projection]
        assert len(matches)==1 and matches[0]['source']==current,member
        assert matches[0].get('previous_source')==revision['previous'],member
        overrides[member]=current
    result=[]
    for row in recipe['rows']:
        current=overrides.get(row['member'],row['source']);checked(current)
        result.append(dict(member=row['member'],source=current,
            historical_recipe_source=row['source']if row['member']in overrides else None,
            explicit_revision=row['member']in overrides))
    return result

def audit():
    """Проверяет все projection pins,39 vectors и только явные изменения CPP исходников."""
    mapping=read(PROJECT/'recipe/source-map.json');recipe=read(PROJECT/'recipe/compile41.json')
    lock=read(PROJECT/'recipe/external-lock.json');checked(lock['sdkconfig'])
    assert lock['sdkconfig']['path']=='coprocessor/c6_audio/config/sdkconfig.profile'
    assert all(row['root']in['IDF','TOOLCHAIN']and not Path(row['path']).is_absolute()for row in lock['files'])
    assert len(recipe['rows'])==39 and len({x['member']for x in recipe['rows']})==39
    for row in mapping['rows']:checked(row['source'])
    effective=effective_compile_sources(mapping,recipe)
    for row in recipe['rows']:
        assert '-c'in row['argv']and '-o'in row['argv']
        assert not any('build/experiments/'in x.replace('\\','/')for x in row['argv'])
    ordered=read(PROJECT/'recipe/archive-order.json')
    assert len(ordered)==39 and len(set(ordered))==39
    assert set(ordered)=={row['member']for row in recipe['rows']}
    assert len(mapping['rows'])==62
    for p in [PROJECT/'recipe/link.rsp.in',PROJECT/'recipe/link-image.json']:
        assert 'build/experiments/'not in p.read_text('utf-8').replace('\\','/')
    return dict(status='PASS_DURABLE_C6_SOURCE_MAPPING_ORDERED39_RECIPE_AUDIT_NOT_BUILD',projection_sources=len(mapping['rows']),private_sources=19,inherited_sources=20,translation_units=39,
        explicit_compile_source_revisions=[row['member']for row in effective if row['explicit_revision']],
        ordered_argv_unchanged=True,public_sdkconfig_seed=pin(checked(lock['sdkconfig'])),effective_config_check='after_explicit_configure_before_compile',external_roots_from_environment=True,SDK=False,COM=False,FullClean_build_verified=False)

def replace(value,variables):
    """Подставляет только именованные roots; неизвестный token запрещает запуск."""
    for k,v in variables.items():value=value.replace('${'+k+'}',str(v).replace('\\','/'))
    assert '${'not in value,value
    return value

def checked_output(path):
    """Разрешает новый выход только в build либо выделенном временном корне C6."""
    out=path.resolve()
    roots=[(ROOT/'build').resolve(),(Path(tempfile.gettempdir())/'bitvyr-c6-audio').resolve()]
    assert any(out!=root and out.is_relative_to(root)for root in roots),'Output must be inside betta/build or TEMP/bitvyr-c6-audio.'
    return out

def prepare(args):
    """Проецирует single-source headers и локальный fixed APU только в Root build."""
    audit();lock=read(PROJECT/'recipe/external-lock.json');resolved=resolve_environment(args,lock)
    external=external_inputs(lock,resolved)
    out=checked_output(args.out)
    out.mkdir(parents=True,exist_ok=False);projection=out/'projection';projection.mkdir();(out/'objects').mkdir()
    variables=dict(PROJECT=PROJECT,ROOT=ROOT,OUT=out,PROJECTION=projection,SDK_BUILD=out/'sdk',**resolved)
    for row in read(PROJECT/'recipe/source-map.json')['rows']:
        source=checked(row['source']);target=projection/row['projection'];target.parent.mkdir(parents=True,exist_ok=True)
        # Единственная identity shared headers и обоих aliases FixedFilter.h.
        if row['projection_method']=='forward-header'or source.name=='FixedFilter.h':
            target.write_text('/** @brief Generated single-source include; не редактировать. */\n#pragma once\n#include "'+source.as_posix()+'"\n',encoding='utf-8',newline='\n')
        else:shutil.copyfile(source,target)
    shutil.copyfile(checked(lock['sdkconfig']),out/'sdkconfig')
    # Partition filename остаётся прежним; generated project содержит его локально.
    generated_project=out/'project';shutil.copytree(PROJECT/'project',generated_project)
    shutil.copyfile(PROJECT/'config/partitions.esp32c6.csv',generated_project/'partitions.esp32c6.csv')

    (out/'link.rsp').write_text(replace((PROJECT/'recipe/link.rsp.in').read_text('utf-8'),variables),encoding='utf-8',newline='\n')
    recipe=read(PROJECT/'recipe/compile41.json');recipes=recipe['rows'];commands=[]
    effective=effective_compile_sources(read(PROJECT/'recipe/source-map.json'),recipe)
    for row in recipes:commands.append(dict(label='compile-'+row['member'],argv=[replace(x,variables)for x in row['argv']],cwd=str(out/'sdk')))
    compiler=Path(commands[0]['argv'][0]);ar=compiler.with_name('riscv32-esp-elf-ar.exe')
    ordered=read(PROJECT/'recipe/archive-order.json')
    commands.append(dict(label='archive39',argv=[str(ar),'rcs',str(out/'libc6_audio.a'),*[str(out/'objects'/n)for n in ordered]],cwd=str(out/'sdk')))
    for label,argv in read(PROJECT/'recipe/link-image.json').items():
        if label in ['link','image']:commands.append(dict(label=label,argv=[replace(x,variables)for x in argv],cwd=str(out/'sdk')))
    rsp=(out/'link.rsp').read_text('utf-8')
    # Fresh SDK libraries/one generated app descriptor object; никакой inherited .a из old build.
    targets=sorted(set(re.findall(r'"((?:esp-idf/[^"\n]+\.a)|(?:CMakeFiles/[^"\n]+\.obj))"',rsp)))
    assert targets and all('main/libmain.a'not in x for x in targets)
    defaults=';'.join(str(PROJECT/'config'/n)for n in ['sdkconfig.defaults','sdkconfig.defaults.esp32c6','sdkconfig.probe.defaults'])
    save(out/'plan.json',dict(status='PREPARED_NEW_C6_ROOT_SDK_BOOTSTRAP_AND_ORDERED39_NOT_EXECUTED',variables={k:str(v)for k,v in variables.items()},configure=[str(variables['PYTHON']),str(variables['IDF']/'tools/idf.py'),'-C',str(generated_project),'-B',str(out/'sdk'),'-D','SDKCONFIG='+str(out/'sdkconfig'),'-D','SDKCONFIG_DEFAULTS='+defaults,'-D','IDF_TARGET=esp32c6','reconfigure'],sdk_targets=targets,commands=commands,external=external,host_tools={name:pin(variables[name.upper()])for name in lock['host_tools']},sdkconfig_seed=pin(checked(lock['sdkconfig'])),effective_sdkconfig_sha256=lock['effective_sdkconfig_sha256'],source_audit=audit(),effective_compile_sources=effective,source_recipe=[pin(PROJECT/'recipe'/n)for n in ['compile41.json','source-map.json','link-image.json','link.rsp.in','archive-order.json','external-lock.json']],SDK_executed=False,COM=False,flash=False))
    return out/'plan.json'

def execute(path,args):
    """Root выполняет fresh dependencies и exact active39; timeout сохраняет HOLD без flash."""
    path=path.resolve();plan=read(path);out=path.parent
    checked_output(out);assert not(out/'result.json').exists();audit()
    for ref in plan['source_recipe']:
        p=Path(ref['path']);assert digest(p)==ref['sha256']
    lock=read(PROJECT/'recipe/external-lock.json')
    env=dict(os.environ);env['IDF_PATH']=plan['variables']['IDF']
    env['IDF_TOOLS_PATH']=plan['variables']['IDF_TOOLS']
    env['ESP_IDF_VERSION']='6.1'
    # idf.py должен использовать уже сверенный virtualenv, а не угаданный default path.
    env['IDF_PYTHON_ENV_PATH']=str(Path(plan['variables']['PYTHON']).parent.parent)
    env['PYTHONUTF8']='1'
    prefixes=[str(Path(plan['variables']['TOOLCHAIN'])/'bin')]
    external_inputs(lock,{name:Path(value)for name,value in plan['variables'].items()})
    for name in ['cmake','ninja','python']:
        tool=check_external(plan['variables'][name.upper()],lock['host_tools'][name])
        explicit=getattr(args,name)
        assert explicit is None or explicit.resolve()==tool,'Re-run --prepare when changing a tool path.'
        if name!='python':prefixes.append(str(tool.parent))
    env['PATH']=os.pathsep.join(prefixes)+os.pathsep+env.get('PATH','')
    receipts=[];result=dict(status='HOLD_FRESH_C6_SOURCE_PROJECT_BUILD',plan=pin(path),receipts=receipts,COM=False,flash=False,target_qualified=False)
    def run(label,argv,cwd):
        """Сохраняет literal argv/stdout/stderr каждого actual Root subprocess."""
        save(out/(label+'-argv.json'),argv);start=time.monotonic()
        p=subprocess.run(argv,cwd=cwd,env=env,capture_output=True,timeout=1800)
        stdout=out/(label+'-stdout.log');stderr=out/(label+'-stderr.log');stdout.write_bytes(p.stdout);stderr.write_bytes(p.stderr)
        row=dict(label=label,rc=p.returncode,seconds=time.monotonic()-start,stdout=pin(stdout),stderr=pin(stderr));receipts.append(row)
        if p.returncode:raise RuntimeError(label+' rc='+str(p.returncode))
    try:
        run('configure',plan['configure'],ROOT)
        verify_config(out/'sdkconfig',lock)
        ninja=plan['variables']['NINJA']
        run('sdk-dependencies',[ninja,'-C',plan['variables']['SDK_BUILD'],*plan['sdk_targets']],ROOT)
        for row in plan['commands']:run(row['label'],row['argv'],row['cwd'])
        result.update(status='PASS_FRESH_C6_SOURCE_PROJECT_BUILD_NOT_TARGET_OR_FLASH',outputs={n:pin(out/n)for n in ['network_adapter.elf','network_adapter.bin','libc6_audio.a','network_adapter.map']},byteexact_ae1c_claim=False,hardware_qualification=False)
    except BaseException as e:result['error']=repr(e)
    save(out/'result.json',result);print(json.dumps(result,ensure_ascii=False))
    return 0 if result['status']=='PASS_FRESH_C6_SOURCE_PROJECT_BUILD_NOT_TARGET_OR_FLASH'else 1

def main():
    """Отделяет SDK-free проверку/plan от явно запрошенного Root исполнения."""
    lock=read(PROJECT/'recipe/external-lock.json');p=argparse.ArgumentParser(description=__doc__)
    mode=p.add_mutually_exclusive_group(required=True);mode.add_argument('--audit',action='store_true');mode.add_argument('--prepare',action='store_true');mode.add_argument('--execute-root',type=Path)
    p.add_argument('--out',type=Path,default=ROOT/'build/c6_audio/fresh01');p.add_argument('--idf',type=Path,help='SDK root; default IDF_PATH');p.add_argument('--idf-tools',type=Path,help='Tools installation root; default IDF_TOOLS_PATH');p.add_argument('--toolchain',type=Path,help='riscv32-esp-elf root; default matching activated installation');p.add_argument('--python',type=Path,help='Pinned ESP-IDF virtualenv interpreter');p.add_argument('--ninja',type=Path);p.add_argument('--cmake',type=Path)
    args=p.parse_args()
    if args.audit:print(json.dumps(audit()))
    elif args.prepare:print(json.dumps(dict(plan=str(prepare(args)),SDK=False,COM=False)))
    else:return execute(args.execute_root,args)

if __name__=='__main__':raise SystemExit(main())
