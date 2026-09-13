# R2 / R2.1 radio concurrency repair

Scope: uncommitted R2 and R2.1 against `main@08d85df`, SX126x-Arduino
**2.0.32**, Adafruit nRF52 **1.7.0** (package `1.10700.0`). No RF/wire changes.

## Why Astra rejected R2

The original callback mutated NetworkService queue/dedupe while the loop could
read a partially written entry. R2's single application owner fixed that race,
but its two-pass/GetStatus timeout recovery was still unsafe:

```
FreeRTOS timer service -> RadioOnTxTimeoutIrq -> RadioBgIrqProcess
  -> application TxTimeout callback -> return
  -> RadioStandby -> RadioSleep -> return
```

RX software timeout has the same post-callback cleanup. SPI operations under
standby/sleep call `SX126xWaitOnBusy -> delay(1) -> vTaskDelay`. Even a higher
priority task can yield before cleanup finishes. Two loop passes and the cached
RF_IDLE mode are not synchronization. Untagged TX results and callback-time
role epoch capture also lacked a boundary excluding delayed old driver work.

## Dependency execution contexts and evidence

Inspected upstream files (patch preserves `radio.cpp.orun-original`):

- `src/boards/mcu/board.cpp`: DIO semaphore, `_lora_task`, `start_lora_task`;
  LORA calls `Radio.BgIrqProcess()` in task context.
- `src/radio/sx126x/radio.cpp`: ISR `RadioOnDioIrq` sets a flag and gives the
  semaphore using FromISR; dispatch reads/clears chip IRQs, copies RX bytes and
  invokes callbacks synchronously. Both software timeout handlers run dispatch
  followed by standby/sleep.
- `src/boards/mcu/nrf52832/timer.cpp`, core `utility/SoftwareTimer.cpp`:
  callbacks run directly in the FreeRTOS timer service, not Adafruit's separate
  callback task.
- Core `main.cpp`, `rtos.h`, `freertos/config/FreeRTOSConfig.h`: loop priority
  **1**, LORA **1** (library macro fallback), timer **2**, preemption enabled.
- `src/boards/sx126x/sx126x-board.cpp`: yielding SPI/BUSY waits. nRF52
  BoardDisableIrq/BoardEnableIrq are empty, making plain IrqFired sharing unsafe.

## Reproducible patch and serialization

Tracked `firmware/scripts/patch_radio.py` runs in PlatformIO's **post
extra-script** phase, after dependency resolution but before compilation. It
checks library/core versions, the complete upstream radio.cpp SHA-256 and exact
replacement fragments. Unknown source/version or changed patched output fails
the build. Reapplication must produce identical output. No generated-only fix
is required. Dependency/core upgrades require explicit re-audit.

Patch scope is one dependency translation unit:

1. Wrap all BgIrqProcess dispatch, callbacks and remaining driver operations
   with a static FreeRTOS mutex. The deep-sleep dispatch entry also acquires it.
   A raw `orunRadioDispatchLocked` entry avoids recursive acquisition by owner.
2. Make IrqFired a lock-free atomic bool, consumed by exchange; retain existing
   ISR semaphore behavior. ISR never acquires a mutex.
3. Add locked timeout cleanup and quiescence bridge functions.
4. Remove P2P Send software-timer arming. The owner services the same configured
   **5000 ms** TX deadline on M3 monotonic time, under the gate. Old software
   timer callback entries are no-ops: no stale expiration can touch the driver,
   block the timer task or become a new TX result. Existing inactive timer
   handles remain because dependency TimerStop uses them. Hardware DIO TX/RX
   timeout handling remains. Rx/RxBoosted assert zero software timeout.

All application Radio operations, including initialization/configuration,
GetStatus, Send and Rx, hold the same gate. Initialization creates it before
LORA task creation. The loop tries without waiting and defers **before driver
reads or result consumption** if busy. LORA uses a nonrecursive FreeRTOS mutex
with priority inheritance. BUSY may yield, but no second driver user can enter.
GetStatus is only a mode decision inside the gate, never synchronization.

Lock order: driver mutex -> short handoff critical section/zero-wait queue.
Callbacks never reacquire the gate. No mutex acquisition occurs inside a
critical section or ISR. Timer callbacks do not acquire it. Existing TimerStop
commands can be serviced without the gate, so no timer/mutex cycle exists.
No second application mutex or recursive lock was introduced.

## Single owner and event handoff

| State | Classification and access |
| --- | --- |
| Relay queue/count/active forward and dedupe | Owner-only NetworkService receive/dequeue/result/reset |
| Network diagnostics, TX kind/busy/recovery/scheduling | Owner-only loop |
| Runtime role/request, sequences, TX buffers | Owner-only; Send copies bytes synchronously into FIFO |
| RX payload/length/RSSI/SNR/epoch | Immutable event copy |
| RX queue | Static FreeRTOS queue, zero-wait producer/consumer |
| TX latch/generations, RX error counters | Driver gate and short publication critical sections |
| Event diagnostics/RX acceptance | Gate and/or short critical sections; public API is loop-only |
| Driver generation/buffers/mode | Driver gate; only IRQ flag is ISR-shared atomic |
| Active manager pointer/callback table | Immutable after initialization, installed before RX |

RX capacity stays **4**, with **255-byte** payloads matching the dependency
limit. Queue control/backing storage and mutex have static lifetime; backing
alignment and item size are explicit. R2.1 adds no runtime heap allocation;
upstream task/timer/semaphore allocations remain. Callback validation rejects
null/oversize input before memcpy and retains no pointer. Full queue drops the
**newest RX** and increments rx_queue_drops. TX uses a separate latch so RX
saturation cannot strand it. RX control counters saturate and count excess;
diagnostic counters have unsigned wrapping semantics. Callback work is bounded,
without logs, clock reads, network mutation, Radio calls or waits.

## TX identity and recovery proof

Before new TX, while holding the gate, the owner stops RX, masks all DIO IRQs,
clears chip IRQs and software IRQ/timeout flags. It assigns a nonzero uint32
generation and arms it before Send. That generation is immutable through the
entire dispatch/callback/cleanup. A semaphore is only a wakeup; it cannot retain
old decoded registers across the gate. No asynchronous software timer is armed.

**No new TX until the previous operation is terminal and driver-quiescent.**
This boundary makes callback-time generation sampling safe. No old dispatch
stack can outlive generation replacement. Wrap is safe because no outstanding
operation survives until token reuse. The first matching terminal wins;
zero/disarmed/mismatched and duplicate DONE/TIMEOUT results increment
stale_tx_results. Owner applies only its active generation and restores RX
before another TX. TX_DONE remains local PHY completion, never DELIVERED.

Update drains pending DIO results before checking the owner deadline. The
locked timeout bridge invokes the callback, performs standby/sleep, and returns
with the gate still held. Only then does owner consume the result and restore
RX. All post-callback cleanup, including BUSY waits, is serialized. Both sleep
and standby report RF_IDLE; library Rx wakes a sleeping SX1262 through its
normal CheckDeviceReady path. Owner state and the gated mode check exclude
active TX/CAD from recovery.

## Role quiescence barrier

setRole requests a transition and rejects old RX events immediately, without
changing the installed radio role. Active TX finishes or reaches its owner
deadline; new TX is disallowed meanwhile. Once the owner obtains the gate,
pending dispatch runs under the old role and its RX is rejected. A dispatch
paused before callback must finish/release the gate first. With TX terminal:

1. Stop RX, mask IRQs, clear chip IRQs and atomic pending flag.
2. Drain/drop the four-slot application queue.
3. Install the different role, resetting network queues/dedupe.
4. Increment epoch, disarm old TX, arm new RX, then enable event acceptance,
   all before releasing the gate.

A late old GPIO/semaphore reads cleared registers, not retained old payload.
Decoded old bytes cannot cross the mutex barrier, so callback-time epoch
capture cannot relabel old RX. Old relay completion while transition is pending
releases RadioManager TX state without updating the soon-reset network state.
Latest request wins; re-requesting the still-installed role cancels an unapplied
transition and retains that role's active forward completion/queue/dedupe. USB
reports the requested role while radio installation may be pending. Provisioning
remains volatile.

## Clock and preserved semantics

R2's loop-only monotonic::nowMs scheduling remains. Relay deadlines begin at
owner processing, not RF arrival. Backpressure adds processing latency but
cannot cause early forwarding, alter the hash or grow bounded queues. Jitter
remains **1200–4200 ms**, with modular half-range comparisons. M3 extends raw
1024 Hz ticks before conversion, avoiding Adafruit's approximately 48.5-day
millis discontinuity.

TRACKER never relays; one hop, exact original 34-byte POSITION inside 49-byte
RELAY_FORWARD, separate relay ID, original source/sequence, nested rejection,
BASE direct+relay observations and datum dedupe are unchanged. No per-packet
ACK/RF/wire changes. PositionFlow retains a stored live packet if gate contention
defers Send rather than consuming its single live attempt.

## Validation and limitations

Host tests compile real RadioManager, NetworkService and driver gate. Patch
guard tests extract the **actual transformed** dispatch wrapper, timeout and
quiescence functions into that binary; only hardware operations and raw IRQ
decoding are modeled. Mutex stubs track ownership/critical depth, not no-ops.
The timeout test pauses after callback/before cleanup, exposes RF_IDLE and runs
ten owner passes plus Send: no driver entry is allowed until release. Old
two-pass recovery would call GetStatus/Rx during this pause and fail.

Tests also cover TX A/B stale tokens, both duplicate terminal orders, pending
role/TX, delayed old callback, pending DIO, last old RX at Standby, late GPIO,
role-request cancellation, RX saturation/lifetime/255/256 boundaries, M5
codecs/dedupe/malformed semantics, relay/deadline uint32 rollover and live packet
retention on gate defer. M3 separately checks actual core-tick conversion;
R2 supplies controlled monotonic readings, not a full FreeRTOS scheduler.

Validation on 2026-09-13: host checks and patch guards pass with ASan, UBSan,
-Wall -Wextra -Werror. PlatformIO installed dependencies and applied the patch
automatically after the old .pio was moved to /tmp and building from no .pio.
Packages used the available download cache; remote download was not needed.
See task report for final image sizes and diff check.

Remaining assumptions: one RadioManager initialized once, loop-only public API,
no future gate bypass, audited nRF52 P2P Send and continuous Rx(0). Timed RX,
continuous-wave, LoRaWAN, alternate targets and new radio APIs require re-audit.
Core version is guarded, not every core file's hash. BUSY failure handling is
unchanged. Software deadline is serviced at the next loop/gate opportunity;
a stalled loop delays recovery. This is not a hardware watchdog.

Physical validation still required: forced timeout/BUSY yield, DIO bursts,
rapid role commands during TX/RX, repeated post-sleep RX, queue pressure, stack
high-water marks, over-air direct/relay behavior, RSSI/SNR, range/current and
long-run/core rollover operation. Host sanitizers/build do not prove hardware.
