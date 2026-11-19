# Plan — CanBus self-driving engine (approach 2)

Spec: `docs/superpowers/specs/2026-11-12-self-driving-canbus-design.md`
Repo: `C:\OpenCodeProjects\CanDLL` (branch `self-driving-engine`)

## Why

`src/src.pro` builds `CanDLL.dll` with `CONFIG += plugin dll` against a **static** Qt,
so QtCore is linked *into* the DLL and the host app links its own copy: two Qt
runtimes in one process. Each copy keeps module-local thread data
(`qthread.cpp`: `static QThreadData *data`), so inside the DLL the owning thread's
`eventDispatcher()` is always null. Consequences: `QTimer` never fires
(`QObject::startTimer: Timers can only be used with threads started with QThread`)
and `Qt::QueuedConnection` delivery across the DLL boundary can never happen.
The library therefore has to drive itself with plain C++ and deliver by direct call.

## Global constraints (binding)

1. **No Qt timers, no Qt threading** in `src/`: after this change `src/canbus.h`
   and `src/canbus.cpp` contain no `QTimer`, no `startTimer`, no `startTimerId`,
   no `QThread`, no `QCoreApplication::postEvent`, no
   `QMetaObject::invokeMethod`, no `QAbstractEventDispatcher`. (`QCoreApplication`
   stays allowed for `applicationDirPath()` in the settings helper.)
2. **Exactly one worker thread per `CanBus` instance**, a plain `std::thread`,
   started on the first successful `open*()`, joined in the destructor. It never
   exits on `close()`: it idles instead, bounded by `kEngineIdleMs = 100` ms.
3. **One lock**: `mutable std::recursive_mutex m_lock` guards every non-atomic
   data member and `m_api`/`m_channel`/`m_txQueue`. Every public method locks it.
   The four pacing fields (`m_rxIntervalMs`, `m_txIntervalMs`, `m_txBurstPerTick`)
   become `std::atomic<int>` and are read by the engine without the lock.
4. **Nothing emits while `m_lock` is held.** Host code is only ever called from
   `emitAfterUnlock()`, after every lock is released. Lock order is
   `m_lock -> m_wakeMtx`, never the reverse; `wakeEngine()` takes `m_wakeMtx` only.
5. **Signal set unchanged.** `frameReceived`, `errorOccurred`, `openChanged`,
   `frameSent`, `txQueueChanged` keep their names and signatures; `rxSlot()` and
   `txSlot()` stay public slots with no arguments, so manual triggering still works.
6. **Two deadlines, one thread.** RX pacing uses `rxIntervalMs` (default 10 ms),
   TX pacing uses `txIntervalMs` (default 25 ms). A non-empty TX queue must not
   make the TX pump run faster than `txIntervalMs`.
7. **Behaviour carried over unchanged**: adaptive RX drain (`kRxBudgetDefault 200`,
   `kRxBudgetMin 20`, `kRxBudgetMax 4000`, grow on a full budget, halve back
   towards default), `PCAN_ERROR_INITIALIZE` during a read => `m_open = false` +
   `errorOccurred` + `openChanged(false)`, TX burst `m_txBurstPerTick` (default 1),
   failed TX frame is dropped and the pump continues, TX queue cleared on `close()`.
8. **No compiler or Qt kit exists in this working copy.** Verification is the
   static audit in Task 1 plus the user's build on `D:\Libs\CanDLL\R4`. Do not
   claim the code compiles.

## File map

| File | Change |
|---|---|
| `src/canbus.h` | engine members + `setFrameHandler`/`isEngineRunning`, drop timer API |
| `src/canbus.cpp` | engine thread, lock discipline, `Emissions` delivery plumbing |
| `example/main.cpp` | two connects get `Qt::DirectConnection` |
| `README.md` | "Two Qt runtimes in one process", threading/delivery notes, API lines |
| `CHANGELOG.md` | Unreleased/Changed entry for the engine |

---

### Task 1: Replace the two QTimers with the engine thread (`src/canbus.h`, `src/canbus.cpp`)

Both files change together — they cannot compile apart.

#### 1a. `src/canbus.h`

* Includes: remove `<QTimer>`; add `<atomic>`, `<condition_variable>`,
  `<functional>`, `<mutex>`, `<thread>`, `<QVector>`, `<QStringList>`.
* Rewrite the class doc comment's last paragraph. It currently says "Thread
  affinity: use the object from the thread it belongs to (the QTimer runs on that
  thread's event loop). Do not call methods from other threads." Replace it with
  the real contract: every method is callable from any thread (one internal
  recursive mutex); frames are delivered **on the engine thread**, so a
  cross-thread connection must be `Qt::DirectConnection` — a queued connection
  cannot cross the DLL boundary on a static Qt build — or use
  `setFrameHandler()`; the handler/slot must be short and must not block on
  anything that waits for the calling thread; never destroy the `CanBus` from the
  handler.
* Public API additions:

```cpp
    /** true while the internal engine thread is running (false before the
        first successful open() and after the destructor started running). */
    bool isEngineRunning() const { return m_engineRunning.load(); }

    /**
     * Non-Qt delivery hook, called on the engine thread for every received
     * frame, after frameReceived(). Use it instead of a connection when the
     * consumer is built without Qt or when queued connections are unavailable
     * (static Qt). Pass an empty std::function to remove it.
     */
    void setFrameHandler(std::function<void(const CanBusFrame &)> handler);
```

* `int pending() const` moves out of line (it has to take the lock).
* Make the constructor `explicit CanBus(QObject *parent = nullptr);` — as a
  one-argument constructor it would otherwise be a converting constructor, and
  `CanBus` must stay non-copyable (it owns a thread).
* Const getters that read guarded state now lock: `isOpen()`, `isFd()`,
  `channelName()`, `lastError()`, `lastErrorCode()`, `lastErrorStatus()`,
  `rxBudget()`, `pending()`. `rxIntervalMs()`, `txIntervalMs()`,
  `txIntervalBurst()` stay inline (atomics).
* Delete `startPolling()`, `stopPolling()`, `startTxPumpIfArmed()`,
  `Q_INVOKABLE void startTimersWhenLoopUp();` and the two `QTimer *` members.
* Private section — add:

```cpp
    /** One batch of signals, emitted after m_lock is released. */
    struct Emissions {
        QVector<CanBusFrame> frames;   // frameReceived, arrival order
        QVector<quint32>     sent;     // frameSent, write order
        QStringList          errors;   // errorOccurred, occurrence order
        bool  queueChanged = false;    // txQueueChanged
        int   openState    = -1;       // -1 nothing, 0 closed, 1 opened
    };

    enum class ReadResult { Frame, Empty, Stopped };

    // engine thread
    void engineLoop();                              // body of m_worker
    void startEngine();                             // call with m_lock held
    void stopEngine();                              // destructor only
    void wakeEngine();                              // m_wakeMtx only
    void drainRxLocked(Emissions &out);             // requires m_lock
    void pumpTxLocked(Emissions &out);              // requires m_lock
    ReadResult readOneLocked(Emissions &out, bool &channelDied); // requires m_lock
    void emitAfterUnlock(const Emissions &out);     // requires: no lock held
    void recordError(Emissions &out, ErrorCode code, const QString &text,
                     TPCANStatus status = 0);
    void raiseError(Emissions &out, TPCANStatus status);

    // engine thread + synchronisation
    std::thread             m_worker;
    std::recursive_mutex    m_lock;               // guards everything below
    std::condition_variable m_wake;
    std::mutex              m_wakeMtx;            // guards m_wakePending
    bool                    m_wakePending = false;
    std::atomic<bool>       m_stopRequested{false};
    std::atomic<bool>       m_engineRunning{false};
    std::function<void(const CanBusFrame &)> m_frameHandler; // guarded by m_lock
```

* Members that change type: `m_rxIntervalMs`, `m_txIntervalMs`,
  `m_txBurstPerTick` -> `std::atomic<int>` with the same defaults (10 / 25 / 1).
  `m_open`, `m_fd`, `m_rxBudget`, `m_channel`, `m_channelName`, `m_api`,
  `m_txQueue`, `m_nominalKbps`, `m_dataKbps`, `m_listenOnly`, `m_autoReset`,
  `m_lastError*` stay as they are (guarded by `m_lock`).
* `clearError()` stays, unchanged.

#### 1b. `src/canbus.cpp` — engine

Delete the anonymous-namespace `hasEventDispatcher()` helper and the
`<QAbstractEventDispatcher>` include. Add the engine section:

```cpp
// ============================================================================
// Engine thread: RX drain + TX pump, paced by two deadlines
// ============================================================================

namespace {
constexpr int kEngineIdleMs = 100;   // poll period while the channel is closed
}

// Qt timers cannot drive this class: with a static Qt build the DLL owns a
// second QtCore copy whose thread data never sees the application's event
// dispatcher, so startTimer() fails and queued connections are never delivered.
// The engine is therefore plain C++: one std::thread, two deadlines.
void CanBus::engineLoop()
{
    using clock = std::chrono::steady_clock;
    m_engineRunning.store(true);

    auto nextRx = clock::now();
    auto nextTx = clock::now();

    while (!m_stopRequested.load()) {
        Emissions out;
        {
            std::lock_guard<std::recursive_mutex> guard(m_lock);
            if (m_open) {
                const auto now = clock::now();
                if (now >= nextRx) {
                    drainRxLocked(out);
                    nextRx = now + std::chrono::milliseconds(m_rxIntervalMs.load());
                }
                if (now >= nextTx && !m_txQueue.isEmpty()) {
                    pumpTxLocked(out);
                    nextTx = clock::now() + std::chrono::milliseconds(m_txIntervalMs.load());
                }
            } else {
                // Closed: stay quiet, wake up for open()/close() or the idle period.
                nextRx = nextTx = clock::now() + std::chrono::milliseconds(kEngineIdleMs);
            }
        }
        emitAfterUnlock(out);       // host code, never with m_lock held

        std::unique_lock<std::mutex> wakeGuard(m_wakeMtx);
        m_wake.wait_for(wakeGuard, std::min(nextRx, nextTx) - clock::now(), [this] {
            return m_stopRequested.load() || m_wakePending;
        });
        m_wakePending = false;
    }
    m_engineRunning.store(false);
}

void CanBus::startEngine()
{
    // Called with m_lock held. The worker is created once per object and joined
    // only in the destructor, so joinable() is the "already started" test - a
    // join here could deadlock against a worker waiting for m_lock.
    if (m_stopRequested.load())
        return;                       // the object is being destroyed
    if (m_worker.joinable()) {
        wakeEngine();                 // it idles while the channel is closed
        return;
    }
    m_wakePending = true;             // start it in "check now" state
    m_worker = std::thread([this] { engineLoop(); });
}

void CanBus::stopEngine()
{
    {
        std::lock_guard<std::mutex> wakeGuard(m_wakeMtx);
        m_stopRequested.store(true);
        m_wakePending = true;
    }
    m_wake.notify_all();
    if (m_worker.joinable()) {
        // Destroying the object from the frame handler would join the engine
        // thread from itself; detach instead of aborting the process.
        if (m_worker.get_id() == std::this_thread::get_id())
            m_worker.detach();
        else
            m_worker.join();
    }
}

void CanBus::wakeEngine()
{
    {
        std::lock_guard<std::mutex> wakeGuard(m_wakeMtx);
        m_wakePending = true;
    }
    m_wake.notify_all();
}

void CanBus::emitAfterUnlock(const Emissions &out)
{
    std::function<void(const CanBusFrame &)> handler;
    { std::lock_guard<std::recursive_mutex> guard(m_lock); handler = m_frameHandler; }

    for (const QString &e : out.errors)
        emit errorOccurred(e);
    if (out.openState >= 0)
        emit openChanged(out.openState == 1);
    for (const CanBusFrame &f : out.frames) {
        emit frameReceived(f);
        if (handler)
            handler(f);
    }
    for (quint32 id : out.sent)
        emit frameSent(id);
    if (out.queueChanged)
        emit txQueueChanged();
}
```

Include `<algorithm>` (for `std::min`) and `<chrono>`.

**Documented emission order** (a change from today, list it in the header comment
above `signals:`): `errorOccurred`, then `openChanged`, then `frameReceived`
(arrival order, each immediately followed by the frame handler), then `frameSent`,
then `txQueueChanged`.

#### 1c. `src/canbus.cpp` — retrofit every method

Mechanical, but the invariant must hold everywhere: **no `emit` while holding
`m_lock`**. The pattern for every method that can emit:

```cpp
bool CanBus::sendFrame(quint32 id, const QByteArray &data, bool extended, bool rtr,
                       bool immediate)
{
    Emissions out;
    bool ok = false;
    {
        std::lock_guard<std::recursive_mutex> guard(m_lock);
        TxItem item; /* ...as today... */
        if (validateForSend(item, out)) {
            if (immediate) {
                const TPCANStatus st = writeItem(item);
                if (st != PCAN_ERROR_OK) raiseError(out, st);
                else { clearError(); out.sent.append(id); }
            } else {
                enqueue(item, out);
            }
            ok = true;
        }
    }
    emitAfterUnlock(out);
    return ok;
}
```

Per-method requirements (methods not listed keep their current body and gain a
lock where they touch guarded state; the pure `static` helpers — `channelHandle`,
`fdDlc`, `fdLength`, `fdBitrateString`, `buildStamp`, all the `QSettings` ones,
`resolveApi`, `makeApi`, `codeFromStatus` — take no lock). **Every public
signature stays byte-identical to today's**, including `int fClock` on
`openFd()`; only the private helpers gain an `Emissions &` parameter.

Rules, restated because they are the review surface:

| Method | Requirement |
|---|---|
| ctor | no timers, no `connect`s; nothing else changes |
| dtor | `stopEngine(); close();` (engine stops before the channel is uninitialized) |
| `openChannel(channel, out)`, `initClassic(channel, kbps, out)`, `initFd(channel, str, out)`, `openFdRaw(channel, out, ...)` | private, called with `m_lock` held; `startPolling()` -> `startEngine()`; `emit openChanged(true)` -> `out.openState = 1`; their `setError(...)` -> `recordError(out, ...)` |
| `open`, `openFd`, `openFdConfigured`, `openConfigured` | public: `Emissions out; bool ok; { lock; ...; } emitAfterUnlock(out); return ok;` |
| `close` | same shape; drop `stopPolling()`/`m_txTimer->stop()`; clearing a non-empty queue sets `out.queueChanged`; `emit openChanged(false)` -> `out.openState = 0`; `wakeEngine()` at the end, still under the lock; no early `return` that would skip `emitAfterUnlock` |
| `applyOptions` | lock (recursive) around the two PCAN `SetValue` calls and the flag writes; it never emits |
| `setListenOnly`, `setAutoReset` | Emissions pattern — their "channel not open" error must still reach `errorOccurred` |
| `setRxIntervalMs`, `setTxIntervalMs`, `setTxBurstPerTick` | atomic stores (`store(qMax(1, ms))`), no timer calls |
| `enqueue(item, out)` | `emit txQueueChanged()` -> `out.queueChanged = true`; **no `wakeEngine()`** — the TX deadline paces the pump, waking per frame would defeat `txIntervalMs` |
| `validateForSend(item, out)` | its five `setError(...)` calls become `recordError(out, ...)` |
| `writeItem` | unchanged (callers hold the lock) |
| `sendFrame`, `sendFdFrame` | the lock/emit pattern above; `emit frameSent(id)` -> `out.sent.append(id)` |
| `txSlot` | `Emissions out; { lock; pumpTxLocked(out); } emitAfterUnlock(out);` |
| `clearTxQueue` | Emissions pattern; no timer |
| `filterMessages(fromId, toId, extended)`, `resetQueues()`, `saveConfiguration()` | Emissions pattern (they report "channel not open" through `errorOccurred`, and `resetQueues`/`clearTxQueue` set `out.queueChanged` when they drop a non-empty TX queue). No private `*Locked` split is needed for them |
| `openConfigured`, `openFdConfigured` | **stay lock-free**: they read `QSettings` and call `open()` / `openFd()`, which lock themselves. Rule for the whole file: a public method that locks must never be called from inside another method's locked scope |
| `rxSlot` | `Emissions out; { lock; drainRxLocked(out); } emitAfterUnlock(out);` |
| `drainRxLocked(out)` | the adaptive drain of today's `rxSlot()` using `readOneLocked()`; keeps `kRxBudgetDefault/Min/Max`; if the channel died (`!m_open`) reset `m_rxBudget = kRxBudgetDefault` and skip the adaptive update |
| `readOneLocked(out)` | today's `readAndEmitOne()` body; `emit frameReceived(frame)` -> `out.frames.append(frame)`; returns `ReadResult::Frame` / `Empty` / `Stopped`; on `PCAN_ERROR_INITIALIZE` sets `m_open = false`, `out.openState = 0`, records the error |
| `pumpTxLocked(out)` | today's `txSlot()` body: burst loop, `raiseError(out, st)`, `out.sent.append(item.id)`, `if (m_txQueue.isEmpty()) out.queueChanged = true;` at the end |
| `setFrameHandler` | new: lock, `m_frameHandler = std::move(handler);`, release the lock, then `wakeEngine()` — a handler installed after `open()` must start delivering without waiting for the current RX deadline to expire |
| `setError` / `raiseError(TPCANStatus)` | **deleted** — replaced by `recordError(Emissions&, ...)` / `raiseError(Emissions&, TPCANStatus)`, which write `m_lastError`/`m_lastErrorCode`/`m_lastErrorStatus` and `out.errors.append(text)` and do **not** emit |
| `clearError` | unchanged |
| `errorText`, `codeFromStatus`, `channelHandle`, `fdDlc`, `fdLength`, `fdBitrateString`, `buildStamp`, `resolveApi`, `makeApi`, all `QSettings` helpers | unchanged, no lock |

`recordError()` definition goes where `setError()` was, in the Errors section,
next to the comment explaining the mapping.

#### 1d. Verification (no compiler here — static audits only)

Write a throwaway script under `C:\Users\Fabio\AppData\Local\Temp\opencode\`
(**not** in the repo) and paste its output into your report:

1. no `QTimer`, `startTimer`, `hasEventDispatcher`, `QAbstractEventDispatcher`,
   `QMetaObject::invokeMethod`, `postEvent` tokens anywhere in `src/`
2. every method declared in `canbus.h` has exactly one definition in
   `canbus.cpp`, and every private helper called from `canbus.cpp` is declared
   (compare signatures mechanically, `CanBus::` definitions vs header decls)
3. no `emit ` line sits inside a scope that took `m_lock` — report every `emit`
   line number with the enclosing function so it can be eyeballed
4. `m_lock` guard counts per function (no unbalanced `lock_guard`)
5. `git grep -n "startPolling\|stopPolling\|startTxPumpIfArmed\|startTimersWhenLoopUp\|readAndEmitOne\|setError(\|raiseError(st)" src/` is empty
6. the header parses as C++ on its own (no new dependency on `<windows.h>`)

#### 1e. Commit

One commit: `canbus: drive RX and TX from an internal engine thread`.
Message body: why (two QtCore copies => Qt timers and queued delivery cannot
work), what replaces it, and the delivery contract.

---

### Task 2: Keep the example working (`example/main.cpp`)

The example is being rewritten by the user; it only has to stay compilable and
show the right connection type.

* `QObject::connect(&can, &CanBus::frameReceived, &printFrame);` and the
  `errorOccurred` connect (right below it) get `Qt::DirectConnection` as their
  third argument.
* Nothing else in `example/`.
* Commit: `example: connect CanBus signals with Qt::DirectConnection`.

---

### Task 3: Documentation (`README.md`, `CHANGELOG.md`)

Same shape (prose for this change), so one dispatch for both files.

**`README.md`**
* New section `## Two Qt runtimes in one process` placed right after
  `## Runtime DLLs`: what a static Qt build does, why `QObject::startTimer`
  warns, what the engine does instead, and the connection rule
  (`Qt::DirectConnection` or `setFrameHandler`).
* `## API summary`: add `setFrameHandler()` and `isEngineRunning()`.
* `## Usage`: the connect example must pass `Qt::DirectConnection`, and the text
  that says frames arrive on the GUI thread must say they arrive on the engine
  thread.
* `## Notes`: the class is thread-safe; keep TX handler/slot bodies short; do not
  destroy `CanBus` from the frame handler.

**`CHANGELOG.md`** — under `## [Unreleased]`, a `### Changed` entry (the `### Fixed`
entry from commit `65d04f5` describes the deferred timer starts; leave it, the new
entry supersedes it in prose):
* `CanBus` no longer uses `QTimer`; RX poll + TX pump run on an internal
  `std::thread`.
* Delivery contract: frames on the engine thread, `Qt::DirectConnection` /
  `setFrameHandler()`, signal order within a batch.
* `startTimersWhenLoopUp()` / `startPolling()` / `stopPolling()` /
  `startTxPumpIfArmed()` removed; `sizeof(CanBus)` changed => rebuild the library
  and every consumer together.
* One commit for the README and one for the CHANGELOG (`changelog: ...`, matching
  the existing CHANGELOG-only commit style, referencing the code commit hash of
  Task 1 — read it with `git log --oneline -1 -- src/canbus.cpp`).
