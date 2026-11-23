"""Проверяет новые явные tags и отказ от вымышленных read witnesses; не снимает P4 замеры."""
import unittest
from tools.frame_pipeline_schema import schema, validate_bound, validate_perf


class FrameSchemaTests(unittest.TestCase):
    """Оба backend остаются различимыми, а output имеет более узкий witness контракт."""
    def output(self):
        """Возвращает минимальный синтетический словарь для parser unit-проверок."""
        return dict(owner='timing_fetch',consumer='output_compositor',game_fps_claim=False,
                    schema_version=2,cpu_snapshot='actual_primary_cut',read_witness='unavailable',
                    effective_tail='off',read_count=0,read_hash='0000000000000000')

    def test_existing_backend_remains_explicit(self):
        """Старый backend не обязан содержать поля новой схемы."""
        validate_bound(dict(owner='observable',consumer='actual_core',game_fps_claim=False),'observable')
        validate_perf({},'observable')
        self.assertEqual(schema('observable').status,'NES_FRAME_PIPELINE_BENCH_STATUS ')

    def test_output_identity(self):
        """Только новая явная identity допускает новую output схему."""
        validate_bound(self.output(),'output')
        with self.assertRaises(ValueError):validate_bound(self.output(),'observable')
        with self.assertRaises(ValueError):schema('auto')

    def test_no_synthetic_read_witness(self):
        """Мутанты отсутствующего witness обязаны отказаться до использования данных."""
        for key,value in [('read_witness','verified'),('read_count',1),('read_count',False),
                          ('read_hash','0000000000000001'),('effective_tail','bound16k'),
                          ('cpu_snapshot','guessed'),('schema_version',2.0),('game_fps_claim',True)]:
            with self.subTest(key=key,value=value):
                state=self.output();state[key]=value
                with self.assertRaises(ValueError):validate_bound(state,'output')

    def test_missing_output_metadata_declines(self):
        """Отсутствующие поля новой схемы не дополняются значениями по умолчанию."""
        for key in self.output():
            with self.subTest(key=key):
                state=self.output();del state[key]
                with self.assertRaises(ValueError):validate_bound(state,'output')

    def test_output_perf(self):
        """Проверяет статические output tags независимо от вычисления кадровой частоты."""
        row=dict(schema='2',producer='timing_fetch',consumer='output_compositor',
                 read_witness='unavailable',effective_tail='off')
        validate_perf(row,'output')
        for key in row:
            with self.subTest(key=key):
                bad=row.copy();bad[key]='unexpected'
                with self.assertRaises(ValueError):validate_perf(bad,'output')

    def sparse(self,sequence=119):
        """Полный ключ current cut; None никогда не означает числовой ноль."""
        state=self.output();state.update(schema_version=3,region='NTSC',generation=4,frames=sequence+1,
            input_frame=sequence,cpu_cycles=9000,ram_policy='sparse120',ram_requested=sequence%120==119,
            ram_valid=sequence%120==119,ram_status='available'if sequence%120==119 else'not_requested',
            ram_hash='0000000000000000'if sequence%120==119 else None,
            ram_key=dict(producer=1,rom=2,rom_bytes=8192,rom_epoch=3,lease_generation=1,transport_epoch=5,
                region='NTSC',sequence=sequence,input_frame=sequence,slot=sequence&1,app_generation=4,
                base_cycle=8000,end_cycle=9000,ppu_frame=sequence+1))
        return state

    def test_sparse_is_explicit_non_parity(self):
        """Старый parity потребитель по умолчанию отвергает schema3 даже с хешем."""
        for sequence in (118,119,120,239):
            state=self.sparse(sequence)
            with self.assertRaises(ValueError):validate_bound(state,'output')
            validate_bound(state,'output',allow_sparse_ram=True)

    def test_sparse_null_is_not_zero(self):
        """Unrequested/unobserved/unavailable имеют разные причины, но строго null hash."""
        state=self.sparse(118);validate_bound(state,'output',allow_sparse_ram=True)
        state['ram_hash']='0000000000000000'
        with self.assertRaises(ValueError):validate_bound(state,'output',allow_sparse_ram=True)
        state=self.sparse();state.update(ram_status='unobserved',ram_hash=None)
        validate_bound(state,'output',allow_sparse_ram=True)
        state.update(ram_status='unavailable',ram_key=None,ram_requested=False,ram_valid=False)
        validate_bound(state,'output',allow_sparse_ram=True)

    def test_sparse_key_and_types_reject_stale(self):
        """Одна изменённая ключевая часть не должна описывать последний current кадр."""
        import copy
        for field,value in [('sequence',118),('input_frame',118),('slot',0),('app_generation',3),
                            ('end_cycle',8999),('ppu_frame',119),('rom_epoch',0),('region','PAL'),('rom_bytes',False)]:
            state=copy.deepcopy(self.sparse());state['ram_key'][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):validate_bound(state,'output',allow_sparse_ram=True)
        for field,value in [('ram_valid',1),('ram_requested',1),('ram_status','available_old'),('ram_hash',0)]:
            state=self.sparse();state[field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):validate_bound(state,'output',allow_sparse_ram=True)


if __name__=='__main__':unittest.main()
