# Changelog

Notable changes to CanDLL, newest first. Dates are the commit dates on
`master`. Each entry names the commit, so the exact diff is one command away:

```
git log --oneline --stat      # every commit with per-file line counts
git show 84c7775              # full diff of one commit
git diff 257c3f8 84c7775      # range diff
```

## [Unreleased]

Two passes since 1.0: the build-machine fixes of 2026-11-12 (`ed8573c` ..
`65d04f5`), then the self-driving engine of 2026-11-14 (`30565f8` .. `07a016e`)
that removes `CanBus`' dependency on Qt timers. The engine supersedes the first
*Fixed* entry below: the timer-deferral code it describes no longer exists.

### Added

* `setFrameHandler(callback)` (commit `30565f8`) - a
  `std::function<void(const CanBusFrame &)>` the engine calls for every received
  frame on the engine thread, right after `frameReceived()`. Delivery without
  the meta-object system, which is the point: nothing about it can be
  mis-connected. `nullptr` disables it. The handler is copied while the internal
  lock is held and invoked after the lock is released, so it may call back into
  the object (`close()` included) but must never destroy it.
* `isEngineRunning()` (commit `30565f8`) - lock-free view of the worker, false
  before the first successful `open()` and once the destructor has stopped it.

### Changed

* **`CanBus` drives itself now: no `QTimer`, one engine thread** (commit
  `30565f8`, +648 / -293 in `src/canbus.{h,cpp}`). A single plain `std::thread`,
  started by the first successful `open()`, keeps two independent deadlines -
  `rxIntervalMs()` (10 ms) for the receive drain, `txIntervalMs()` (25 ms) for
  the send pump - and waits on one condition variable, with a 1 ms floor on each
  period and a 100 ms backstop while the channel is closed. This is the fix for
  the symptom the whole pass started from: with a static Qt the app and
  `CanDLL.dll` each embed their own `QtCore`, `QThreadData::current()` is
  module-local, so inside the DLL the owning thread's event dispatcher is always
  null and `QObject::startTimer()` returns `0` - every timer start was silently
  lost and nothing was ever received.
* Nothing is emitted while the lock is held. The RX drain, the TX pump, opens
  and errors fill a private `Emissions` struct and `emitAfterUnlock()` delivers
  it once the guard is gone, in a fixed order: `errorOccurred()` ->
  `openChanged()` -> `frameReceived()` plus the frame handler -> `frameSent()` ->
  `txQueueChanged()`. A slot can no longer re-enter the library while its state
  is mid-update (`30565f8`, order closed in `fc69e60`).
* One `std::recursive_mutex` guards the state and every public method takes it,
  so the class is thread-safe: `open()` works from any thread, before
  `QApplication::exec()`, or from a thread that never runs an event loop. The
  pacing setters and getters (`setRxIntervalMs()`, `setTxIntervalMs()`,
  `setTxBurstPerTick()`, `rxIntervalMs()`, `txIntervalMs()`,
  `txIntervalBurst()`) and `isEngineRunning()` are `std::atomic` and take no
  lock. Lock order is `m_lock` -> `m_wakeMtx`, and `wakeEngine()` only ever
  touches the second (commit `fc69e60`).
* Thread lifecycle: `stopEngine()` is called from the destructor and joined
  there, not in `close()`, and detaches instead of joining if it is ever asked
  to join its own thread; `startEngine()` cannot fail or spawn a second worker,
  does not log while the lock is held, and never respawns once the destructor
  has set `m_stopRequested` (commits `fc69e60`, `408a440`).
* `close()` wakes the engine out of its idle wait and is idempotent even when
  the channel died on its own: a `PCAN_ERROR_INITIALIZE` during a drain now
  clears `m_channel` and `m_fd` as well, so a second `close()` neither
  re-uninitialises the handle nor repeats `openChanged(false)`, and `isFd()` is
  false on a closed object. `startEngine()` wakes an idling worker, so a re-open
  does not pay the 100 ms backstop for its first drain and first pump (commits
  `fc69e60`, `408a440`).
* `enqueue()` does not wake the engine: a burst of `sendFrame()` calls becomes
  one pump on the next TX deadline instead of one thread wakeup per frame
  (commit `fc69e60`).
* **ABI break:** the worker, mutex, condition variable and atomics are members,
  so `sizeof(CanBus)` changed and `CanDLL.dll` plus every consumer must be
  rebuilt from the same headers. `rxSlot()` and `txSlot()` stay public for
  manual driving; `startTimersWhenLoopUp()` is gone with the timers.

### Fixed

* `stopEngine()` left one window open with a lock-free check (commit
  `07a016e`). It set `m_stopRequested` and tested `m_worker.joinable()` without
  the mutex, so an `open()` on another thread could start a worker between the
  two and leave it running, unjoined, against a dying object. Setting the flag
  and handing over the thread object now happen under `m_lock` - the same lock
  `startEngine()` holds when it refuses to respawn after the flag - and the join
  happens once the lock is released, because the worker needs it to finish the
  pass it is in. Only reachable by using the object while it is being destroyed,
  so this is belt and braces rather than a reported crash.
* `open()` no longer loses its RX timer on a thread without a running event
  loop (commit `65d04f5`). Both timer starts printed
  `QObject::startTimer: Timers can only be used with threads started with
  QThread` (from `startPolling()` and `startTxPumpIfArmed()`) and Qt returns `0`
  from `startTimer()` in that case - the timer was simply gone, so the channel
  looked dead and nothing was ever received. `QTimer` needs the thread's
  `QAbstractEventDispatcher`, which the `QCoreApplication` *constructor*
  installs for the main thread and `QThread::start()` installs for workers.
  `startPolling()` and `startTxPumpIfArmed()` now test that dispatcher
  themselves, via `QAbstractEventDispatcher::instance(obj->thread())` - the same
  thread data `QObject::startTimer()` tests - and queue the new
  `Q_INVOKABLE startTimersWhenLoopUp()` slot when it is missing; the slot
  re-checks `m_open` and starts whichever timer is still idle, so a channel
  closed in the meantime stays quiet and a queued retry is harmless.
  `startPolling()` also gained the `m_open` guard it never had, and both timer
  pointers are now null-checked. What it actually achieved: the two
  `startTimer` complaints became one line per thread
  ("thread '%s' has no event dispatcher yet") and reception stayed dead, because
  the dispatcher `CanBus` looks for is registered in the host's `QtCore` copy
  while the DLL checks its own - a condition no timer start can satisfy.
  **Superseded by `30565f8`:** `CanBus` does not use `QTimer` any more, so
  `startPolling()`, `startTxPumpIfArmed()` and `startTimersWhenLoopUp()` were
  deleted with the timers and that warning cannot be printed at all.
* The example compiles again with MSVC (commit `38fa1cc`). `example/console.cpp`
  called `m_thread.detach()` on a `QThread`, and `QThread` has no `detach()` -
  that is `std::thread` - hence `C2039: 'detach': is not a member of 'QThread'`.
  The shutdown path is reworked around what Qt actually allows: destroying a
  running `QThread` calls `qFatal()`, and a blocking `stdin` read cannot be
  cancelled, so `ConsoleReader` no longer has a destructor at all. It is created
  with `new()` in `main.cpp` and intentionally left alive until the process
  ends, which also removes the use-after-free window where the blocked worker
  woke up during CRT teardown and emitted on a destroyed object. Its
  `QTextStream` moved from a heap member to a stack local inside `run()`,
  `m_stop` became `std::atomic<bool>`, and the new `requestStop()` is hooked to
  `QCoreApplication::aboutToQuit` so the reader goes quiet on every exit path.
* `filterMessages()` now compiles with MSVC (commit `ed8573c`). The bundled PCAN
  subset never declared the two receive-filter modes that the call passes to
  `CAN_FilterMessages`, so MSVC stopped with `C2065: 'PCAN_MODE_EXTENDED':
  undeclared identifier` and the same for `PCAN_MODE_STANDARD`; the damaged
  argument list then cascaded into `C2198: 'FnCAN_FilterMessages': too few
  arguments for call` and `C2737: 'st': const object must be initialized` on the
  very same statement. `PCAN_MODE_STANDARD` and `PCAN_MODE_EXTENDED` are now
  defined in `src/pcanbasic_api.h` as aliases of `PCAN_MESSAGE_STANDARD` /
  `PCAN_MESSAGE_EXTENDED`, which is exactly what the official `PCANBasic.h`
  does, and the mode is computed on its own line before the call.
* `src/pcanbasic_api.h` was audited against the official PEAK header
  (PCAN-Basic 4.x, `Include/PCANBasic.h`): every `PCAN_*` / `TPCAN*` token used
  by CanDLL is declared, and all 13 runtime-resolved exports agree with the SDK
  in argument count, argument types and `__stdcall` convention - plus every
  `m_api->...()` call site was re-checked against its `Fn*` typedef. Worth the
  trouble because `PCANBasic.dll` is bound at run time through `QLibrary`: a
  wrong typedef compiles cleanly and only misbehaves on the bus.

### Docs

* One contract for slots and handlers (commit `07a016e`). The class doc in
  `src/canbus.h` told consumers not to call back into `CanBus` from a direct
  slot except through the atomic accessors - contradicting the signal block in
  the same header, the `setFrameHandler()` doc and the README, and pushing
  integrators away from a supported pattern. The header and README now agree on
  what the code does: a slot or handler runs on the engine thread with no lock
  held, so any method may be called from it (`close()`, a re-`open()`,
  `sendFrame()`), while blocking on work another thread must finish and
  destroying the object remain forbidden. The member comment no longer claims
  `m_lock` guards the worker and the wake condition.
* README gained a **Two Qt runtimes in one process** section: why two `QtCore`
  copies exist between the app and `CanDLL.dll`, why that breaks `QTimer` *and*
  queued signal delivery, what the engine does instead, and how a consumer must
  bind the signals - `Qt::DirectConnection`, or no context object at all, or
  `setFrameHandler()`. It includes the exact three `connect()` forms with the
  default each one gets, because the default is the trap: only the forms
  **without** a receiver or context object are direct, and a slot attached to a
  context object is queued and silently drops engine-thread frames. The rules a
  direct slot must follow are written down next to them.
* README API table, usage snippet and *Notes* now describe the engine (new rows
  for `setFrameHandler()`, `isEngineRunning()` and `rxBudget()`, corrected rows
  for `close()`, `rxSlot()` and `txSlot()`, the removed
  `startTimersWhenLoopUp()`), and the example app section explains what its
  `connect()` calls must look like.
* `example/main.cpp`: the two `CanBus` connects are annotated with why they are
  direct connections (commit `e0e5f80`, superseding `5f1430d`). The version
  `5f1430d` committed appended `Qt::DirectConnection` to a three-argument
  `connect()` with a function pointer / lambda slot, and no such overload exists
  in `qobject.h` - it would not have compiled.
* Superseded by the two entries above: README gained a **Timers need a running
  event loop** note (what `CanBus`'s thread must look like, and what the
  deferred start means) and an API row for `startTimersWhenLoopUp()`; both
  described the timer design and were removed with it. The ABI note survives but
  now says the opposite about size: `sizeof(CanBus)` *did* change.
* README headline now says **Qt 5.14+ or Qt 6.x**, matching the *Prerequisites*
  section and the version guard in `src/src.pro` (it still claimed "Qt 6.0+").
* `CHANGELOG.md` commit table completed (`15fbba1`, `ed8573c`, `3e5990e`).
* `src/pcanbasic_api.h`: the channel-handle block now says that only the base
  handle of each family is listed and that `PCAN_ISABUS*` / `PCAN_PCCBUS*` keep
  their historical ISA / PCMCIA values, which the current official
  `PCANBasic.h` no longer defines. They are reference documentation only -
  `CanBus::channelHandle()` computes every channel numerically.

Neither the library nor the example has ever been compiled here - this machine
has no Qt kit and no compiler (`cl`, `qmake`, `g++` are all absent) - so every
build runs on the build machine (static Qt 6.2.2, MSVC x64):
`qmake CanDLL.pro && nmake`. Each pass fixes what the previous one reported -
`ed8573c` for the library (`PCAN_MODE_*`), `38fa1cc` for the example
(`QThread::detach()`). What it did not fix was reception itself: `65d04f5` was
built and the channel stayed dead, which is what the engine pass above is for.
Two things matter for the next build: the tree that is built must be a current
copy of this repository, otherwise the same error comes back - and because
`sizeof(CanBus)` changed in `30565f8`, the library and the host application have
to be rebuilt together, not just the DLL.

## [1.0] - 2026-10-31

Commit `84c7775` - "canbus: adaptive RX drain budget + machine-readable error
codes" (README.md, src/canbus.cpp, src/canbus.h; +211 / -99).

### Added

* `CanBus::ErrorCode` enum (`Q_ENUM`, so it works with `qMetaEnum` and
  `qDebug()`): `NoError`, `Api`, `BadChannel`, `BadParameter`, `NotOpen`,
  `NotInitialized`, `AlreadyOpen`, `QueueFull`, `Hardware`, `Other`.
  Read it with `lastErrorCode()` to branch on failures instead of parsing
  strings.
* `lastErrorStatus()` - the raw `TPCANStatus` bits of the last failure, for
  cases the enum deliberately collapses.
* `rxBudget()` - the current per-tick RX drain budget, useful to watch the
  adaptive loop converge under load.
* All validation failures (ID out of range, FD frame on a classic channel,
  payload over 8 / 64 bytes) now report `ErrorCode::BadParameter`.
* README section "Which compiler? (ABI notes)": the consumer must be built
  with the same ABI as the Qt kit (MinGW vs MSVC), and anything linking the
  DLL must be rebuilt when `canbus.h` changes.

### Changed

* RX drain is adaptive instead of a fixed cap. `rxSlot()` keeps its bookkeeping
  and delegates per-frame work to the new private `readAndEmitOne()`. The loop
  runs while `count < budget`; a read failure breaks immediately. If a tick
  consumes its whole budget the driver queue was still not empty, so the budget
  doubles for the next tick; otherwise it halves back towards the default.
  Bounds: `kRxBudgetDefault 200`, minimum 20, ceiling `kRxBudgetMax 4000`.
  A flood is absorbed in fewer timer round-trips while the work per tick stays
  bounded, so the event loop is never starved.
* Default TX pump interval `m_txIntervalMs`: 50 ms -> 25 ms, i.e. 40 frames/s
  at 8 bytes - comfortably below a 500 kbit/s bus. `setTxIntervalMs()` and
  `setTxBurstPerTick()` still override it, `immediate=true` still bypasses
  the FIFO.
* Every error now funnels through one private helper
  `setError(ErrorCode code, const QString &text, TPCANStatus status = 0)`,
  which stores the three fields and emits `errorOccurred()`. `raiseError(st)`
  maps a `PCAN_ERROR_*` status to an enum code via `codeFromStatus()` plus a
  human-readable `errorText(st)`; success paths call `clearError()`.

### Removed

* `static const int MaxFramesPerTick = 200` - replaced by the adaptive budget.

## [0.9] - 2026-10-25

Commit `257c3f8` - "example interactive console; always-DLL build".

* The library is now always built as a DLL (`candll.pri` / `src/src.pro`
  simplified, `src/candll_global.h` export macro cleanup) instead of
  switching between static and shared.
* `example/` gained an interactive console (`console.h` / `console.cpp`) on
  top of `CanBus`: type commands to open a channel, send frames, toggle FD.

## [0.8] - 2026-10-19

Commit `56d612e` - initial import (1849 lines).

* `CanBus` QObject wrapper over PEAK PCAN-Basic: `open()` / `openFd()` /
  `openFdBitTiming()`, `close()`, `sendFrame()` / `sendFdFrame()`, TX FIFO with
  paced pump, polled RX with `framesReceived()`, bus-off handling with optional
  auto-reset, channel/bit-timing and rate settings from `candll.ini`.
* `src/pcanbasic_api.h` PCAN declarations, Qt project files, console example,
  README.

## Commit log

| Commit  | Date       | Subject |
| ------- | ---------- | ------- |
| 07a016e | 2026-11-14 | canbus: correct the slot contract, close stopEngine's spawn race |
| e0e5f80 | 2026-11-14 | example: keep the 3-argument connect forms, and say why they are direct |
| 5f1430d | 2026-11-14 | example: connect CanBus signals with Qt::DirectConnection |
| 408a440 | 2026-11-14 | canbus: wake the idling engine on re-open, make close() idempotent |
| fc69e60 | 2026-11-14 | canbus: fix engine pacing, locking and thread-lifecycle gaps |
| 30565f8 | 2026-11-14 | canbus: drive RX and TX from an internal engine thread |
| d180eb9 | 2026-11-12 | docs: design spec and plan for the self-driving CanBus engine |
| 6d4cb04 | 2026-11-12 | changelog: record the deferred timer starts (65d04f5) |
| 65d04f5 | 2026-11-12 | canbus: defer timer starts until the thread has an event dispatcher |
| 510caca | 2026-11-12 | changelog: record the console QThread::detach() fix (38fa1cc) |
| 38fa1cc | 2026-11-12 | example: drop QThread::detach() - no such member (C2039), rework console shutdown |
| 1fb7823 | 2026-11-12 | docs: align Qt version claim, complete changelog commit table |
| 3e5990e | 2026-11-12 | changelog: record the MSVC filterMessages fix |
| ed8573c | 2026-11-12 | pcanbasic_api: declare PCAN_MODE_STANDARD/EXTENDED (MSVC C2065 in filterMessages) |
| 15fbba1 | 2026-11-01 | docs: add CHANGELOG.md (0.8 -> 1.0) and link it from README |
| 84c7775 | 2026-10-31 | canbus: adaptive RX drain budget + machine-readable error codes |
| 257c3f8 | 2026-10-25 | 20261025 example interactive console; always-DLL build |
| 56d612e | 2026-10-19 | 20261019 WhpKnows |

Every commit reachable from `master` is in that table (`git log --oneline` is
empty against it apart from the doc commits that only touch these two files).

No remote is configured (`git remote -v` is empty), so all history lives in
this repository only.
