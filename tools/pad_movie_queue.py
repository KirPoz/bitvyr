"""Точная rolling-очередь guest input. Нет COM, таймера FPS, retry или сдвига кадров.

Слот резервируется ДО вызова sender и удерживается при неизвестном исходе write.
Успешный write не освобождает слот; credit даёт только точный NES_PAD_APPLY.
ACK/APPLY текущей firmware не содержат generation: их область bracketed явными
NES_INPUT/PERF/status generation и подтверждённым запуском/cleanup collector.
"""
from collections import deque

def _require(value,reason):
    """Fail-closed guard, работающий также при python -O."""
    if not value:
        raise ValueError(reason)

def _uint(value):
    """Не принимает bool/float вместо unsigned guest counter."""
    return type(value) is int and value >= 0

class RollingPadQueue:
    """Single-owner FIFO до16 sent/unACKed/ACKed reservations; полный movie не режется."""
    def __init__(self,movie):
        self.movie=movie
        plan=movie.get("queue_plan",{})
        _require(plan.get("mode")=="rolling","Rolling mode must be explicit")
        _require(plan.get("queue_capacity",16)==16,"Rolling capacity must be16")
        edges=movie.get("edges")
        _require(isinstance(edges,list),"Missing full rolling edges")
        self.edges=[]
        for edge in edges:
            _require(isinstance(edge,dict) and _uint(edge.get("frame")) and _uint(edge.get("mask")) and edge["mask"]<=255,"Invalid rolling edge")
            item=(edge["frame"],edge["mask"])
            _require(not self.edges or self.edges[-1][0]<item[0],"Rolling frames must be strictly increasing")
            _require(item!=(0,item[1]) or item[1]==0,"Nonzero frame0 cannot be guaranteed by UART rolling replay")
            self.edges.append(item)
        initial=plan.get("initial",[])
        deferred=plan.get("deferred",[])
        _require(isinstance(initial,list) and isinstance(deferred,list),"Rolling initial/deferred must be lists")
        def decode(items):
            result=[]
            for item in items:
                _require(isinstance(item,str) and item.count(":")==1,"Invalid rolling plan item")
                parts=item.split(":")
                _require(all(p.isascii() and p.isdecimal() for p in parts),"Invalid rolling plan number")
                result.append(tuple(map(int,parts)))
            return result
        _require(decode(initial+deferred)==self.edges,"Rolling initial+deferred must equal full edges in order")
        _require(0<=len(initial)<=16 and (bool(initial) or not self.edges),"Rolling initial must seed1..16 edges")
        self.initial_count=len(initial)
        self.reservations=[]
        self.pending=deque()
        self.acked=0
        self.applied=0
        self.generation=None
        self.latest_consumed_input=-1
        self.peak_reserved=0
        self.failed_reason=None
        self.started=False

    @property
    def all_sent(self):
        """Все edge write-attempts завершены успешно; ACK/APPLY проверяются отдельно."""
        return len(self.reservations)==len(self.edges) and all(r["write_complete"] for r in self.reservations)

    def _fail(self,reason):
        """Не возвращает uncertain reservation и запрещает дальнейшую подачу."""
        if self.failed_reason is None:
            self.failed_reason=reason
        raise ValueError(reason)

    def _healthy(self):
        """После любой ошибки автоматический retry/reuse этого queue запрещён."""
        if self.failed_reason is not None:
            raise ValueError("Rolling queue held after failure: "+self.failed_reason)

    def _check(self,value,reason):
        if not value:
            self._fail(reason)

    def _send_one(self,sender):
        """Резервирует один slot до write; короткий write/flush exception удерживает его навсегда."""
        self._healthy()
        self._check(len(self.pending)<16,"Rolling reservation overflow")
        index=len(self.reservations)
        self._check(index<len(self.edges),"Rolling extra send")
        frame,mask=self.edges[index]
        self._check(frame>self.latest_consumed_input,"Rolling next edge already late; no shifting permitted")
        record=dict(ordinal=index,frame=frame,mask=mask,write_complete=False,acked=False,applied=False)
        self.reservations.append(record)
        self.pending.append(index)
        self.peak_reserved=max(self.peak_reserved,len(self.pending))
        try:
            sender([f"padat {frame} {mask}"],"rolling_pad_queue")
        except Exception as exc:
            self._fail("Rolling write outcome uncertain; reservation held: "+str(exc))
        record["write_complete"]=True

    def start(self,sender):
        """Ставит только declared initial seed после run; не пытается читать или ждать ACK."""
        self._healthy()
        self._check(not self.started,"Rolling start repeated")
        self.started=True
        for _ in range(self.initial_count):
            self._send_one(sender)

    def observe_batch(self,packets):
        """Учитывает ВЕСЬ уже прочитанный batch ДО refill, чтобы старый ACK не скрывал поздний tail."""
        self._healthy()
        for packet in packets:
            tag=packet.get("raw_type","")
            f=packet.get("fields",packet)
            relevant=tag in {"NES_INPUT","NES_PAD_SCHEDULE","NES_PAD_APPLY","NES_OUTPUT_FRAME_PIPELINE_TIMES","NES_COUPLED_BENCH","NES_OUTPUT_FRAME_PIPELINE_STATUS","NES_COUPLED_BENCH_STATUS"}
            if not relevant:
                continue
            generation=f.get("generation")
            if generation is not None:
                self._check(_uint(generation) and generation>0,"Rolling invalid generation")
                if self.generation is None:
                    self.generation=generation
                self._check(generation==self.generation,"Rolling generation changed")
            progress=None
            if tag=="NES_INPUT" and f.get("action")=="none":
                progress=f.get("frame")
            elif tag=="NES_PAD_APPLY":
                progress=f.get("frame")
            elif tag.endswith("_TIMES") or tag=="NES_COUPLED_BENCH" or tag.endswith("_STATUS") and f.get("playing") is True:
                frames=f.get("frames")
                if _uint(frames) and frames>0:
                    progress=frames-1
            if progress is not None:
                self._check(_uint(progress),"Rolling invalid progress")
                self.latest_consumed_input=max(self.latest_consumed_input,progress)

    def on_packet(self,packet):
        """Подтверждает точный FIFO ACK/APPLY; ACK не создаёт credit."""
        self._healthy()
        tag=packet.get("raw_type","")
        f=packet.get("fields",packet)
        if tag=="NES_PAD_SCHEDULE":
            self._check(self.acked<len(self.reservations),"Unexpected/duplicate rolling ACK")
            record=self.reservations[self.acked]
            self._check(record["write_complete"],"ACK before complete rolling write")
            self._check((f.get("frame"),f.get("mask"))==(record["frame"],record["mask"]),"Rolling ACK order/mask mismatch")
            expected_queued=self.acked+1-self.applied
            self._check(type(f.get("queued")) is int and f["queued"]==expected_queued and 1<=f["queued"]<=16,"Rolling ACK queue depth mismatch")
            record["acked"]=True
            self.acked+=1
        elif tag=="NES_PAD_APPLY":
            self._check(self.applied<self.acked,"Rolling APPLY before exact ACK/duplicate")
            record=self.reservations[self.applied]
            self._check(f.get("frame")==f.get("scheduled")==record["frame"] and f.get("mask")==record["mask"],"Rolling late/out-of-order/duplicate APPLY")
            self._check(bool(self.pending) and self.pending[0]==self.applied,"Rolling reservation order mismatch")
            self.pending.popleft()
            record["applied"]=True
            self.applied+=1

    def refill(self,sender):
        """После каждого разобранного ACK/APPLY использует только реальные APPLY credits."""
        self._healthy()
        while len(self.pending)<16 and len(self.reservations)<len(self.edges):
            self._send_one(sender)

    def finish_batch(self):
        """Пропавший APPLY нельзя скрыть последующим input/PERF progress."""
        self._healthy()
        if self.applied<len(self.reservations):
            self._check(self.reservations[self.applied]["frame"]>self.latest_consumed_input,
                        "Rolling missing APPLY at already observed input frame")
        if len(self.reservations)<len(self.edges):
            self._check(self.edges[len(self.reservations)][0]>self.latest_consumed_input,
                        "Rolling unsent edge is already late")

    def finish(self):
        """Разрешает PASS только при всех writes/ACKs/APPLY и явной generation области."""
        self._healthy()
        self._check(self.generation is not None,"Rolling generation never observed")
        self._check(self.all_sent and self.acked==self.applied==len(self.edges) and not self.pending,
                    "Rolling incomplete writes/ACK/APPLY")

    def snapshot(self):
        """Возвращает проверяемый ledger, включая held uncertain reservation при HOLD."""
        return dict(mode="rolling",capacity=16,total_edges=len(self.edges),initial_edges=self.initial_count,
                    attempted=len(self.reservations),completed_writes=sum(r["write_complete"] for r in self.reservations),
                    ACKs=self.acked,APPLYs=self.applied,reserved=len(self.pending),peak_reserved=self.peak_reserved,
                    generation=self.generation,generation_scope="bracketed_by_INPUT_PERF_status; ACK/APPLY generation absent in current wire",
                    latest_consumed_input=self.latest_consumed_input,failed_reason=self.failed_reason,
                    reservations=self.reservations)

def rolling_queue_for(movie):
    """Legacy без mode остаётся прежним; неизвестный explicit mode не деградирует в старый scheduler."""
    mode=movie.get("queue_plan",{}).get("mode")
    if mode is None:
        return None
    _require(mode=="rolling","Unknown queue_plan mode")
    return RollingPadQueue(movie)

