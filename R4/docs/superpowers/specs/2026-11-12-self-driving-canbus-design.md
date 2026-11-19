# CanBus self-driving engine — design

Date: 2026-11-12
Status: waiting for approval

## Intent and the why

`CanDLL.dll` must actually work when it is linked by a Qt application built with the
same **static** Qt 6.2.2 / MSVC2019 kit.

Today it cannot. The DLL is built `CONFIG += plugin dll` against a static Qt, so the
whole of QtCore is linked into `CanDLL.dll`; the host application links that same
QtCore again. One process, two independent Qt runtimes. Qt keeps the current thread's
event dispatcher in storage belonging to the QtCore copy that runs the code
(`qthread.cpp`, `QThreadData::current()` → module-local `static QThreadData *data`).
The host's `QCoreApplication` installs a dispatcher in *its* copy only, so inside the
DLL `QThreadData::eventDispatcher` is always null, `QObject::startTimer()` warns
("Timers can only be used with threads started with QThread") and returns 0.
Consequence: the RX poll timer and the TX pump timer inside `CanBus` can never start,
in any thread, whatever the host does. Confirmed by observation — the example creates
`CanBus` on the main thread after `QCoreApplication` with a running loop and still gets
the warning; in a single-runtime build that warning is impossible.

The fix is structural: the library stops depending on a Qt event dispatcher for its
own engine.

## Goals

- RX polling and TX pacing run with **no event dispatcher anywhere** — no `QTimer`,
  no `QThread::exec()`, no `QCoreApplication` in either runtime.
- Works in every link mode: DLL + Qt host, DLL + non-Qt host, static `CanDLL.lib`.
- The two Qt warnings disappear permanently; no code path calls `startTimer()`.
- Public API (names, arguments, signals) stays as it is, so the host source does not
  need rewriting apart from the connection type (below).
- No data races: the engine thread and host threads touch the PCAN handle and the
  queue through one lock.

## Non-goals

- No Qt-free `extern "C"` interface. Not needed while the host is a Qt app; it would
  replace signals with callbacks and change every call site.
- No multi-channel support.
- No change to the PCAN-Basic binding (`pcanbasic_api.h`, `QLibrary` + `Fn*` typedefs).

## Anti-goals

- Frames silently never arriving because a connection type was wrong and nobody said
  so at run time.
- Deadlock between the engine thread and a host slot that calls back into the library.

## Constraints

- Static Qt 6.2.2, MSVC2019 x64, host links Qt itself → two runtimes in one process.
- Cross-runtime rules, in force for the whole design:
  - **Direct** signal/slot connections work (same Qt version, same compiler, same CRT,
    identical `QObjectPrivate` layout, moc code reached through the vtable).
  - **Queued** connections cannot work: posting an event from the DLL's copy reaches no
    dispatcher in the host's copy. `Qt::AutoConnection` resolves inside the DLL's copy
    and becomes queued when the emit happens on the engine thread → the frame is lost.
  - Therefore every connection to a `CanBus` signal must be `Qt::DirectConnection`.
- `QString` / `QByteArray` cross the boundary by value and by const reference. Accepted:
  both copies use the same Qt build and the same CRT, so one allocator is involved.
- Adding/removing data members changes `sizeof(CanBus)` → the import library and every
  consumer must be rebuilt together. Already documented in README "Which compiler?".

## Approaches considered

1. **One Qt runtime: `CONFIG += staticlib`.** Zero change in `canbus.cpp`; timers and
   queued connections work as documented today. Trades away the `CanDLL.dll` artifact.
   The `LNK2019` seen when trying it is the export macro (`CANDLL_EXPORT` still expands
   to `Q_DECL_IMPORT`, so the linker asks for `__imp_` thunks); a third
   `#elif defined(CANDLL_STATIC)` branch with an empty `CANDLL_EXPORT` fixes it.
2. **Self-driving DLL — chosen.** One dedicated OS thread inside the DLL runs the
   engine; `QTimer` is removed. Works whatever the host links, and the `.dll` file
   stays. Trades: delivery is a direct call on the engine thread (so `connect()` must
   pass `Qt::DirectConnection`, or use the new handler hook), and internal locking.
3. **DLL owns Qt completely** (host must not link Qt; DLL exports app/loop lifecycle,
   optionally a C API). Only justified if a non-Qt consumer must appear; largest change.

Chosen: **2**, because shipping `CanDLL.dll` is a hard requirement and it must work
with a Qt host. If shipping the `.dll` ever stops mattering, 1 is the smaller design
and the engine thread is harmless there too.

## Design (decided)

### Engine thread

- Members: `std::thread m_worker`, `std::atomic<bool> m_stopRequested`,
  `std::condition_variable mWake`, plus a `std::recursive_mutex m_lock` that guards the
  PCAN calls, the queue and the state. The `QTimer` members and their slots are gone.
- Created lazily by the first successful `open()`/`openFd()`, joined in `close()` and
  in the destructor. The thread is plain `std::thread`: no `QThread`, so no Qt runtime
  of any copy is involved in creating it.
- Loop, paced with `std::condition_variable::wait_for` (so `close()`,
  `setRxIntervalMs()`, `setTxIntervalMs()` and `sendFrame()` wake it at once):
  - every `rxIntervalMs` (default 10 ms): one adaptive RX drain — same algorithm and
    same bounds as now (`kRxBudgetDefault 200`, min 20, max 4000, double on full
    budget, relax towards default when it runs dry early).
  - every `txIntervalMs` (default 25 ms): up to `txBurstPerTick` items from the FIFO.
- `rxSlot()` / `txSlot()` become private engine functions called only by that thread;
  they keep their current behaviour, including the "channel went away" path
  (`PCAN_ERROR_INITIALIZE` → `m_open = false`, `openChanged(false)`).

### Locking and re-entrancy

- One `std::recursive_mutex`, held around every PCAN-Basic call and every read or write
  of `m_open`, `m_txQueue`, `m_channel`, budgets and intervals. The host may call any
  public method from any thread.
- Signals are emitted **while the lock is held** only where the current code already
  does it inline (`errorOccurred`, `openChanged`); `recursive_mutex` makes a host slot
  that calls `close()`/`sendFrame()` re-enter safely instead of deadlocking.
- `frameReceived` is emitted **without** the lock held: the engine reads one frame into
  a local `CanBusFrame` under the lock, releases it, then emits. This keeps arbitrary
  host code out of the critical section while a PCAN read is pending.

### Delivery

- Signals (`frameReceived`, `frameSent`, `txQueueChanged`, `openChanged`,
  `errorOccurred`) are emitted on the engine thread and are therefore **direct-call
  only** into the host. The header states this at the class level, the README gets a
  "Two Qt runtimes" section with the exact `connect()` form.
- Escape hatch that cannot be misconfigured:
  `void setFrameHandler(std::function<void(const CanBusFrame &)>)` — invoked directly on
  the engine thread before `frameReceived`. The handler is copied out under a short
  lock and called outside the lock.
- A host that wants frames on its own thread does it inside the slot/handler: it emits
  one of its *own* signals, queued in its own Qt runtime, which does reach its GUI or
  worker thread.

### Removed / changed API surface

- Removed: the two `QTimer` members, `startPolling()`/`stopPolling()`,
  `startTxPumpIfArmed()`, `Q_INVOKABLE startTimersWhenLoopUp()`, file-static
  `hasEventDispatcher()`.
- Unchanged signatures: `open`, `openFd`, `openFdRaw`, `close`, `sendFrame`,
  `sendFdFrame`, `clearTxQueue`, `resetQueues`, `filterMessages`, `setListenOnly`,
  `setAutoReset`, `setRxIntervalMs`, `setTxIntervalMs`, `setTxBurstPerTick`,
  `rxBudget`, the `ErrorCode`/`lastErrorCode`/`lastErrorStatus` diagnostics, all
  static configuration helpers.
- Added: `setFrameHandler`, `isEngineRunning()`.
- moc and `sizeof(CanBus)` change → full rebuild of library and host together.

## Left to the builder

- Exact wait/re-arming details of the loop, and whether one timer fd is shared for the
  two periods, as long as both periods are honoured within one interval.
- Keeping `m_rxBudget` as plain `int` (engine-thread only) or `std::atomic<int>` behind
  `rxBudget()`.
- Whether `errorOccurred` emission stays inside the lock (default: yes, unchanged).
