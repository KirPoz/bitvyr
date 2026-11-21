"""Маршруты/ROM identity/равные ROI и native refusal: без COM/SDK/user-ROM dependencies."""
import copy
import json
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch
import warnings
import zipfile

import prepare_c6_routes as r


def image(mapper=4, submapper=None):
    """Собственный синтетический iNES/NES2, не копия игры пользователя."""
    raw=bytearray(16+32768+8192)
    raw[:4]=b'NES\x1a';raw[4]=2;raw[5]=1;raw[6]=(mapper&15)<<4;raw[7]=mapper&0xf0
    if submapper is not None:raw[7]|=8;raw[8]=(submapper<<4)|(mapper>>8)
    return bytes(raw)


def fixtures():
    """Один fixed-input маршрут с запасом до observer после ROI."""
    identity=r.rom_identity(image());crc=identity['crc32']
    game=dict(crc32=crc,title='own-synthetic',mapper=4,region='NTSC',rom_pal=False,
              baseline_report=960,roi_reports=[1080,1200],roi_input0=[960,1199],end_frame=1440,
              edges=[dict(frame=60,mask=8),dict(frame=63,mask=0),dict(frame=1000,mask=128),dict(frame=1400,mask=0)])
    return dict(games=[game]),dict(by_crc={crc:[dict(identity,origin=dict(path='unused',member=None))]},errors=[])


def human_segment(game):
    """Sparse logged source остаётся candidate даже при целых source bounds."""
    return dict(crc32=game['crc32'],title=game['title'],mapper=4,region='NTSC',replayable=True,
        hold_reasons=[],first_frame=0,frame0_source=dict(line=10),source_generation=9,start_reason='GAME_START',
        events=[dict(frame=0,mask=0),dict(frame=60,mask=8),dict(frame=63,mask=0),dict(frame=240,mask=128),dict(frame=1000,mask=0)],
        roi_candidates=[dict(kind='moving_input_candidate',baseline_report=120,roi_input0=120,roi_input_last=1199)],
        last_completed_frame=1320,source_guid='a'*64,input_prefix_sha256='b'*64,
        session_id='capture:01',variant_id='own-v01',last_completed_source=dict(line=50),
        source_input_counts=dict(xbox_raw=4),observed_gaps=[],limitations=['candidate, not exact'])


class Routes(unittest.TestCase):
    """Проверяет числовой/источниковый контракт; native timing не используется как FPS."""
    # Проверяет идентификацию NES 2.0 и обработку неверного заголовка картриджа.
    def test_rom_identity_nes2_and_invalid_header(self):
        self.assertEqual(r.rom_identity(image(85,2))['mapper'],85)
        self.assertEqual(r.rom_identity(image(85,2))['submapper'],2)
        self.assertEqual(r.rom_identity(image())['bytes'],40976)
        with self.assertRaises(ValueError):r.rom_identity(b'notNES'*4)

    # Одинаковый образ в ZIP и отдельном файле даёт одинаковый идентификатор.
    def test_inventory_zip_and_raw_same_bytes_no_rom_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'own.nes').write_bytes(image())
            with zipfile.ZipFile(p/'own.zip','w') as z:z.writestr('nested/own.nes',image())
            inv=r.inventory_roms(p);crc=r.rom_identity(image())['crc32']
            self.assertEqual(len(inv['by_crc'][crc]),2);self.assertEqual(inv['errors'],[])
            self.assertFalse(inv['ROM_bytes_in_bank']);self.assertEqual(len(list(p.iterdir())),2)

    # Повреждение заголовка ROM или контейнера ZIP получает причину в инвентаризации.
    def test_inventory_corrupt_header_or_container_is_explicit(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'bad.nes').write_bytes(b'brokenNESimage123')
            (p/'bad.zip').write_bytes(b'notzip')
            inv=r.inventory_roms(p);self.assertEqual(len(inv['errors']),2);self.assertFalse(inv['by_crc'])

    # Проверяет отказ при неоднозначном выборе записи внутри архива.
    def test_inventory_duplicate_zip_member_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)
            with warnings.catch_warnings(record=True):
                with zipfile.ZipFile(p/'repeat.zip','w') as z:
                    z.writestr('same.nes',image());z.writestr('same.nes',image())
            inv=r.inventory_roms(p);self.assertEqual(len(inv['errors']),1);self.assertFalse(inv['by_crc'])

    # Сборка трёх окон сохраняет полный ввод и содержимое исходного описания маршрута.
    def test_pinned_equal3_preserves_all_input_and_source_object(self):
        p,roms=fixtures();original=copy.deepcopy(p)
        bank,coverage=r.build_bank(p,{},roms)
        self.assertEqual(p,original)
        g=bank['games'][0];self.assertEqual(g['edges'],p['games'][0]['edges'])
        self.assertEqual(g['roi_reports'],[1080,1200,1320]);self.assertEqual(g['roi_input0'],[960,1319])
        self.assertEqual(coverage[0]['measured_guest_frames'],360)
        self.assertFalse(g['qualification_scope']['scene_verified']);self.assertFalse(bank['route_preparation']['native_or_hardware_FPS_claim'])

    # При коротком хвосте маршрут сдвигает целое окно внутрь известного участка ввода.
    def test_tail_limit_backshifts_whole_window_not_beyond_declared_prefix(self):
        p,roms=fixtures();p['games'][0].update(end_frame=1321,baseline_report=1200,roi_input0=[1200,1319])
        p['games'][0]['edges']=p['games'][0]['edges'][:2]
        bank,_=r.build_bank(p,{},roms)
        g=bank['games'][0];self.assertEqual(g['baseline_report'],960);self.assertEqual(g['roi_reports'][-1],1320)
        self.assertTrue(g['scene_scope']['new_or_repositioned_roi'])

    # Проверяет явный отказ, когда известного ввода недостаточно для заданного числа окон.
    def test_short_prefix_never_pads_or_silently_uses_fewer_windows(self):
        p,roms=fixtures();p['games'][0]['end_frame']=500;p['games'][0]['edges']=p['games'][0]['edges'][:2]
        bank,rows=r.build_bank(p,{},roms)
        self.assertEqual(bank['games'],[]);self.assertEqual(rows[0]['reason'],'SHORT_PREFIX_FOR_EQUAL_WINDOWS')
        self.assertFalse(bank['route_preparation']['complete_requested_route_coverage'])

    # Проверяет тип числа окон и выбор ROM по известному CRC.
    def test_invalid_window_types_and_unknown_crc(self):
        p,roms=fixtures()
        for n in (0,-1,True,1.5):
            with self.subTest(n=n),self.assertRaises(ValueError):r.build_bank(p,{},roms,windows=n)
        with self.assertRaises(ValueError):r.build_bank(p,{},roms,crcs=['00000000'])

    # Все 26 записей банка дают по 360 измеряемых кадров на локальных тестовых данных.
    def test_all_26_routes_equal360_real_bank_without_external_rom_dependency(self):
        source=json.loads((r.HERE/'qualification-movies.json').read_text(encoding='utf8'))
        # Fake identity — только структурная проверка банка, не ROM acceptance.
        roms=dict(by_crc={g['crc32']:[dict(crc32=g['crc32'],mapper=g['mapper'],sha256='a'*64,bytes=32,origin={})] for g in source['games']},errors=[])
        bank,coverage=r.build_bank(source,{},roms)
        self.assertEqual(len(bank['games']),26);self.assertTrue(all(c['measured_guest_frames']==360 for c in coverage))
        self.assertEqual({len(g['roi_reports']) for g in bank['games']},{3})
        self.assertEqual([g['edges'] for g in bank['games']],[g['edges'] for g in source['games']])
        for g in bank['games']:self.assertIsNotNone(r.rolling_queue_for(g))

    # Рецепт выбирает заданный источник команд и сохраняет границы 360 кадров.
    def test_curated_recipe_preserves_360_frame_routes_and_selected_source(self):
        # Проверяем 26 реальных сценариев без загрузки ROM или открытия порта.
        pinned = json.loads((r.HERE / 'qualification-movies.json').read_bytes())
        human = json.loads((r.HERE / 'human-playthrough-movies.json').read_bytes())
        recipe = json.loads((r.HERE / 'c6-scene-recipe.json').read_bytes())
        battles = json.loads((r.HERE / 'c6-battle-scenes.json').read_bytes())
        battle_by_crc = {g['crc32']: g for g in battles['games']}
        roms = dict(by_crc={g['crc32']: [dict(crc32=g['crc32'], mapper=g['mapper'],
                     sha256='a' * 64, bytes=32, origin={})] for g in pinned['games']})
        for crc, scene in battle_by_crc.items():
            roms['by_crc'][crc] = [dict(scene['rom_identity'], origin={})]
        bank, coverage = r.build_bank(pinned, human, roms, source='catalog', scene_recipe=recipe,
                                      battle_bank=battles)
        by_crc = {g['crc32']: g for g in bank['games']}
        self.assertEqual(len(coverage), 26)
        self.assertEqual(len(by_crc), 26)
        for original in pinned['games']:
            crc = original['crc32']
            with self.subTest(crc=crc):
                game = by_crc[crc]
                self.assertEqual(game['roi_input0'][1] - game['roi_input0'][0] + 1, 360)
                self.assertFalse(game['qualification_scope']['scene_verified'])
                if crc in battle_by_crc:
                    scene = battle_by_crc[crc]
                    self.assertEqual(game['roi_input0'], scene['roi_input0'])
                    self.assertEqual(game['edges'], scene['edges'])
                    self.assertEqual(game['scene_scope']['source_native_battle'], scene['scene_scope'])
                elif crc in recipe['human_overrides']:
                    scene = recipe['human_overrides'][crc]
                    self.assertEqual(game['roi_input0'][0], scene['start'])
                    self.assertEqual(game['input_provenance']['variant_id'], scene['variant'])
                else:
                    self.assertEqual(game['edges'], original['edges'])

    # Отсутствие выбранного варианта ввода фиксируется как причина пропуска сцены.
    def test_catalog_missing_selected_variant_has_no_silent_fallback(self):
        pinned, roms = fixtures()
        segment = human_segment(pinned['games'][0])
        human = dict(schema='bitvyr_human_input_bank_v1', segments=[segment])
        recipe = dict(schema='bitvyr_c6_scene_recipe_v1',
                      human_overrides={segment['crc32']: dict(variant='absent-v02', start=720, description='Бой')})
        bank, coverage = r.build_bank(pinned, human, roms, source='catalog', scene_recipe=recipe)
        self.assertEqual(bank['games'], [])
        self.assertEqual(coverage[0]['reason'], 'SCENE_RECIPE_VARIANT_MISSING')

    def test_battle_sha_or_oversized_roi_holds_catalog_without_intro_fallback(self):
        """Короткая заставка не заменяет бой при несовпадении ROM или границ."""
        pinned, roms = fixtures()
        game = copy.deepcopy(pinned['games'][0])
        crc = game['crc32']
        game.update(roi_input0=[600,959], baseline_report=600, roi_reports=[720,840,960],
                    rom_identity={'sha256': '1'*64}, scene_scope={'combat': True},
                    input_provenance={'selected': 'fight'})
        battle_bank = dict(schema=1, games=[game])
        recipe = dict(schema='bitvyr_c6_scene_recipe_v1', human_overrides={})
        human = dict(schema='bitvyr_human_input_bank_v1', segments=[])
        bank, coverage = r.build_bank(pinned, human, roms, source='catalog', scene_recipe=recipe,
                                      battle_bank=battle_bank)
        self.assertEqual(bank['games'], [])
        self.assertEqual(coverage[0]['reason'], 'BATTLE_ROM_SHA_MISMATCH')
        game['rom_identity']['sha256'] = roms['by_crc'][crc][0]['sha256']
        bank, coverage = r.build_bank(pinned, human, roms, windows=4, source='catalog', scene_recipe=recipe,
                                      battle_bank=battle_bank)
        self.assertEqual(bank['games'], [])
        self.assertEqual(coverage[0]['reason'], 'REQUESTED_WINDOWS_EXCEED_PINNED_BATTLE')

    # Первые и отложенные команды вместе сохраняют полный поток из более чем 16 событий.
    def test_source_events_more_than16_never_truncated(self):
        p,roms=fixtures();p['games'][0]['edges']=[dict(frame=60+i*10,mask=i%2) for i in range(40)]
        bank,_=r.build_bank(p,{},roms);g=bank['games'][0]
        self.assertEqual(len(g['edges']),40);self.assertEqual(len(g['queue_plan']['initial']),14)
        self.assertEqual(len(g['queue_plan']['deferred']),26)

    # Отсутствие ROM, несовпадающий маппер и коллизия CRC останавливают выбор маршрута.
    def test_missing_mapper_mismatch_and_crc_collision_are_hold(self):
        for mode in ('missing','mapper','collision'):
            p,roms=fixtures();crc=p['games'][0]['crc32']
            if mode=='missing':roms['by_crc'].clear()
            elif mode=='mapper':roms['by_crc'][crc][0]['mapper']=85
            else:roms['by_crc'][crc].append(dict(roms['by_crc'][crc][0],sha256='c'*64))
            bank,rows=r.build_bank(p,{},roms)
            with self.subTest(mode=mode):self.assertFalse(bank['games']);self.assertEqual(rows[0]['status'],'HOLD')

    # Проверяет допустимые маски, начальный кадр и уникальность кадра для каждой команды.
    def test_edge_duplicate_bad_mask_and_nonzero_frame0_rejected(self):
        for edges in ([dict(frame=0,mask=1)],[dict(frame=1,mask=256)],
                      [dict(frame=1,mask=0),dict(frame=1,mask=1)]):
            with self.assertRaises(ValueError):r.checked_edges(edges)

    # Проверяет происхождение и статус маршрута, восстановленного из записи игрока.
    def test_human_prefix_candidate_not_exact_or_ble_scene_claim(self):
        p,roms=fixtures();seg=human_segment(p['games'][0]);h=dict(schema='bitvyr_human_input_bank_v1',segments=[seg])
        bank,_=r.build_bank(p,h,roms,source='human');g=bank['games'][0]
        self.assertEqual(g['baseline_report'],240);self.assertEqual(g['roi_reports'],[360,480,600])
        self.assertEqual(g['edges'],seg['events'][:4]);self.assertEqual(g['end_frame'],720)
        self.assertFalse(g['input_provenance']['exact_human_replay_proof']);self.assertFalse(g['input_provenance']['physical_BLE_HCI_load_reproduced'])
        self.assertEqual(g['input_provenance']['source_generation'],9)
        self.assertEqual(g['input_provenance']['after_chosen_prefix']['held_mask'],128)
        self.assertFalse(g['input_provenance']['after_chosen_prefix']['measured_ROI'])

    # Неопределённое начало, пауза или другой регион дают явную причину пропуска.
    def test_human_missing_frame0_hold_statecarry_region_never_falls_back_to_pinned(self):
        for mode in ('frame0','hold','state','region'):
            p,roms=fixtures();s=human_segment(p['games'][0])
            if mode=='frame0':s['frame0_source']=None
            elif mode=='hold':s['hold_reasons']=['ambiguous_pause']
            elif mode=='state':s['start_reason']='unsupported_state_carry'
            else:s['region']='PAL'
            bank,rows=r.build_bank(p,dict(schema='bitvyr_human_input_bank_v1',segments=[s]),roms,source='human')
            with self.subTest(mode=mode):self.assertFalse(bank['games']);self.assertEqual(rows[0]['status'],'HOLD')

    # Если у игры несколько записей прохождения, используется явно выбранный вариант.
    def test_human_repeated_variants_need_explicit_selection(self):
        p,roms=fixtures();a=human_segment(p['games'][0]);b=dict(a,session_id='capture:02',variant_id='own-v02')
        h=dict(schema='bitvyr_human_input_bank_v1',segments=[a,b])
        bank,rows=r.build_bank(p,h,roms,source='human');self.assertFalse(bank['games'])
        bank,_=r.build_bank(p,h,roms,source='human',variant_ids=['own-v02'])
        self.assertEqual(bank['games'][0]['input_provenance']['variant_id'],'own-v02')

    # Слишком короткий участок ручной записи получает собственную причину отказа.
    def test_human_short_candidate_interval_does_not_borrow_other_scene(self):
        p,roms=fixtures();seg=human_segment(p['games'][0])
        seg['roi_candidates'][0].update(baseline_report=1000,roi_input0=1000,roi_input_last=1199)
        bank,rows=r.build_bank(p,dict(schema='bitvyr_human_input_bank_v1',segments=[seg]),roms,source='human')
        self.assertFalse(bank['games']);self.assertEqual(rows[0]['reason'],'SHORT_HUMAN_SCENE_INTERVAL')

    # Проверяет имя варианта и допустимые границы измеряемого интервала при ручном выборе.
    def test_unknown_variant_and_misaligned_or_outside_ROI_override_reject(self):
        p,roms=fixtures();seg=human_segment(p['games'][0]);h=dict(schema='bitvyr_human_input_bank_v1',segments=[seg]);crc=p['games'][0]['crc32']
        with self.assertRaises(ValueError):r.build_bank(p,h,roms,source='human',variant_ids=['unknown'])
        with self.assertRaises(ValueError):r.build_bank(p,{},roms,roi_starts={crc:361})
        bank,rows=r.build_bank(p,{},roms,roi_starts={crc:1320})
        self.assertFalse(bank['games']);self.assertIn('exceeds known prefix',rows[0]['reason'])

    # При выборе границ сцены сохраняет порядок кнопок от начала записи до измерения.
    def test_explicit_human_ROI_uses_full_recorded_prefix_not_reordered_actions(self):
        p,roms=fixtures();seg=human_segment(p['games'][0]);h=dict(schema='bitvyr_human_input_bank_v1',segments=[seg]);crc=p['games'][0]['crc32']
        bank,_=r.build_bank(p,h,roms,source='human',roi_starts={crc:840})
        g=bank['games'][0];self.assertEqual(g['roi_input0'],[840,1199]);self.assertTrue(g['scene_scope']['explicit_ROI_override'])
        self.assertEqual(g['edges'],[dict(frame=e['frame'],mask=e['mask']) for e in seg['events'] if e['frame']<g['end_frame']])

    # Проверяет полную передачу длинного ввода и статус контрольной команды за концом прогона.
    def test_native2048_full_inputs_and_unexecuted_sentinel_preserve_hold(self):
        p,_=fixtures();g=p['games'][0];g['roi_reports']=[1080,1200,1320]
        g['edges']=[dict(frame=i,mask=0 if i==0 else 128) for i in range(600)]
        original=copy.deepcopy(g['edges'])
        args,proof=r.native_arguments('runner','rom',g,1320,'frame.ppm',2048)
        self.assertEqual(args.count('--pad-frame'),601);self.assertEqual(g['edges'],original)
        sentinel=proof['native_only_unexecuted_sentinel'];self.assertGreater(sentinel['frame'],1320)
        self.assertEqual(sentinel['mask'],0);self.assertTrue(proof['captured_input_prefix_unchanged'])
        self.assertLessEqual(proof['Windows_command_chars_including_NUL'],32767)

    # Превышение лимита команд Windows останавливает запуск до обрезки аргументов.
    def test_native_windows_command_limit_refuses_without_truncation(self):
        p,_=fixtures();g=p['games'][0];g['roi_reports']=[1080,1200,1320]
        g['edges']=[dict(frame=10000+i,mask=255) for i in range(2048)];g['edges'][-1]['mask']=0
        with self.assertRaisesRegex(ValueError,'WIN32_COMMAND_LINE_LIMIT'):
            r.native_arguments('runner','rom',g,1320,'frame.ppm',2048)
        self.assertEqual(len(g['edges']),2048)

    # Валидатор обнаруживает расхождение границ сцены и подготовленной очереди.
    def test_validator_rejects_tampered_ROI_and_queue(self):
        p,roms=fixtures();bank,_=r.build_bank(p,{},roms)
        for field in ('ROI','queue'):
            bad=copy.deepcopy(bank)
            if field=='ROI':bad['games'][0]['roi_reports'][1]+=1
            else:bad['games'][0]['queue_plan']['deferred']=['1:0']
            with self.subTest(field=field),self.assertRaises(ValueError):r.validate_bank(bad)

    # Лимит нативного прогона проверяется до запуска процесса и изменения входных команд.
    def test_native_refuses_more_than16_without_subprocess_or_edge_rewrite(self):
        p,roms=fixtures();p['games'][0]['edges']=[dict(frame=60+i*10,mask=i%2) for i in range(40)];p['games'][0]['edges'][-1]['mask']=0
        bank,_=r.build_bank(p,{},roms)
        with tempfile.TemporaryDirectory() as tmp:
            runner=Path(tmp)/'runner';runner.write_text('FAKE')
            with patch.object(r.subprocess,'run') as run:
                result=r.native_previews(bank,roms,runner,Path(tmp)/'preview',[p['games'][0]['crc32']])
                run.assert_not_called();self.assertEqual(result[0]['status'],'UNAVAILABLE')
        self.assertEqual(len(bank['games'][0]['edges']),40)

    # Сверяет завершённый кадр и кадр ввода в трёх нативных срезах.
    def test_native_three_cuts_have_correct_completed_and_input_frame_keys(self):
        p,roms=fixtures();bank,_=r.build_bank(p,{},roms);game=bank['games'][0]
        with tempfile.TemporaryDirectory() as tmp:
            tmp=Path(tmp);rom=tmp/'own.nes';rom.write_bytes(image());runner=tmp/'runner';runner.write_text('FAKE')
            roms['by_crc'][game['crc32']][0]['origin']=dict(path=str(rom),member=None)
            # Подменяет нативный процесс и сохраняет переданные номера кадров для проверки.
            def fake(args,**kwargs):
                n=int(args[args.index('--frames')+1]);ppm=Path(args[args.index('--ppm')+1]);ppm.write_bytes(b'P6\n1 1\n255\n\0\0\0')
                self.assertEqual(args.count('--pad-frame'),len(game['edges']))
                return types.SimpleNamespace(returncode=0,stdout=json.dumps(dict(frames=n,mapper=4,pal=False,frame_hash='aa',pcm_hash='bb',pcm_count=800,host_fps=99999)),stderr='')
            with patch.object(r.subprocess,'run',side_effect=fake):result=r.native_previews(bank,roms,runner,tmp/'previews',[game['crc32']])
            self.assertEqual([x['completed_frame'] for x in result[0]['cuts']],[961,1140,1320])
            self.assertEqual([x['input_frame'] for x in result[0]['cuts']],[960,1139,1319])
            self.assertNotIn('host_fps',json.dumps(result))
            self.assertFalse(result[0]['FPS_claim'])

if __name__=='__main__':unittest.main(verbosity=2)
