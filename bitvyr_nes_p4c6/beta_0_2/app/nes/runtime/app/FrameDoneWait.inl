/** @file FrameDoneWait.inl
 * @brief Private SDK-free Main wait: настоящий done при продвижении прежнего C6 audio.
 * @details Включается внутри nes::app; не владеет Worker, Machine, PCM либо lease.
 */
namespace frame_done {
/** @brief Ready даёт только actual semaphore; PumpFault и Deadline не являются join. */
enum class WaitResult {Ready,PumpFault,Deadline};
/**
 * @brief Продвигает Main consumer до actual done с неизменным пределом1500мс.
 * @param beganUs Начало monotonic host-clock ожидания в микросекундах.
 * @param tryDone Только неблокирующий actual semaphore take; true расходует настоящий token.
 * @param pump Один прежний C6 consumer poll; false липкий и не допускает forced Ready.
 * @param now Монотонные host microseconds; guest-time и Machine не наблюдаются.
 * @param idle Настоящая уступка IDLE0 раз в32мс pending; не задержка на каждом RPC.
 * @details Первый и каждый после pump/idle probe предшествуют следующему pump.
 * Deadline не продлевается активностью либо idle. Отказ не расходует поздний token,
 * не очищает inFlight, не читает job fields и не освобождает владельцев.
 */
template<class TryDone,class Pump,class Clock,class Idle>
inline WaitResult waitDone(std::int64_t beganUs,TryDone&&tryDone,Pump&&pump,
                           Clock&&now,Idle&&idle)noexcept {
    const auto deadline=beganUs+1500000;
    auto lastIdle=beganUs;
    for(;;){
        if(tryDone())return WaitResult::Ready;
        if(now()>=deadline)return WaitResult::Deadline;
        if(!pump())return WaitResult::PumpFault;
        const auto afterPump=now();
        if(afterPump>=deadline)return WaitResult::Deadline;
        if(afterPump-lastIdle>=32000){idle();lastIdle=now();}
    }
}
} // namespace frame_done
