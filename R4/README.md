# CanDLL — Qt CAN bus DLL (PEAK-System PCAN)

A small C++ shared library (qmake, **Qt 5.14+ or Qt 6.x**) that wraps the
PEAK-System **PCAN-Basic API** and exposes a Qt-idiomatic `CanBus` class:

* `open()` / `close()` — open/close a PCAN channel (Classic CAN)
* `openFd()` / `openFdRaw()` — open a channel in CAN FD mode
* `sendFrame()` — queue a Classic CAN packet (≤ 8 bytes)
* `sendFdFrame()` — queue a CAN FD frame (≤ 64 bytes, BRS switchable)
* TX FIFO + pump — commands are queued and written on a paced deadline
  (default 25 ms per frame, bursts via `setTxBurstPerTick()`), so the bus never
  gets flooded
* RX/TX engine — one internal thread (`std::thread`) drains the RX queue and
  paces the TX FIFO and emits every signal; no `QTimer` is involved, see
  [Two Qt runtimes in one process](#two-qt-runtimes-in-one-process)

The PCAN functions are bound at **run time** from `PCANBasic.dll` (shipped with
the PEAK driver), so the PCAN-Basic SDK/`.lib` is *not* needed to build. The
official `PCANBasic.h` values were verified and a minimal API subset is bundled
in `src/pcanbasic_api.h`.

## Layout

```
CanDLL.pro          root project (src + example)
candll.pri          include this from your own .pro to consume the DLL
src/                CanDLL.dll  (CanBus, export macro, PCAN API subset)
example/            CanTest.exe console smoke test
lib/  bin/          build output (created by qmake builds)
CHANGELOG.md        per-commit change history (0.8 -> 1.0, plus unreleased)
```

## Prerequisites

1. **Qt 5.14+ or Qt 6.x** (any kit, MSVC or MinGW) with `qmake` in `PATH`.
   CanDLL is always built as a shared library (`CanDLL.dll`). If your kit is Qt
   configured with `-static`, read
   [Two Qt runtimes in one process](#two-qt-runtimes-in-one-process).
2. **PEAK PCAN driver** on every target machine
   ([peak-system.com](https://www.peak-system.com/PCAN-Driver.352.0.html?&L=1));
   it installs `PCANBasic.dll` as a system component.
3. A PCAN device (PCAN-USB / PCAN-Pro / PCAN-LAN...).

## Build

```bat
cd CanDLL
qmake CanDLL.pro
nmake            REM  MSVC   (or: mingw32-make  for MinGW)
```

Outputs: `lib/CanDLL.dll` (+ import lib) and `bin/CanTest.exe`
(the example copies the DLL next to itself automatically on Windows).

## Runtime DLLs

| DLL | Provided by | Needed when |
|---|---|---|
| `CanDLL.dll` | this project | always (your app links against it) |
| `Qt6Core.dll` | Qt kit (deploy with `windeployqt`) | always |
| `PCANBasic.dll` | PEAK driver install (`C:\Windows\System32`) | at first `open()` — loaded on demand via `QLibrary`, so CanDLL itself loads fine without a device/driver |

There is **no explicit `LIBS += -lPCANBasic`**: the import library
(`PCANBasic.lib`) is deliberately *not* linked. All `CAN_*` entry points are
resolved at run time with `QLibrary`/`resolve()`, so the project builds with
plain Qt on both MSVC and MinGW, and fails gracefully (with a clear
`errorOccurred()` message) if the driver is missing instead of refusing to
start. See `CanBus::resolveApi()` in `src/canbus.cpp`.

## Two Qt runtimes in one process

`CanDLL.dll` links QtCore into itself. If your application also links QtCore —
the usual case, static or shared — the process holds two QtCore copies, and
they do not know about each other.

That rules out both mechanisms a Qt class normally uses to deliver work:

* **`QTimer` does not work inside the DLL.** `QObject::startTimer()` needs an
  event dispatcher for the object's thread, registered in the *owning* QtCore
  copy. Inside the DLL there is none for your thread, so the call fails and
  logs `QObject::startTimer: Timers can only be used with threads started with
  QThread`.
* **Queued signal delivery does not work across the boundary.** A queued
  connection posts an event through the receiver's QtCore copy; the two copies
  do not share a queue, so a `Qt::QueuedConnection` (or an `AutoConnection`
  that resolves to queued) silently never arrives. Direct connections are fine:
  they are plain function calls.

`CanBus` therefore drives itself. One internal thread — the *engine*, a plain
`std::thread` — starts at the first successful `open()`, and on each tick
drains the PCAN receive queue and, on its own deadline, writes a burst from the
TX FIFO. It then waits on a condition variable until the earlier of the two
deadlines, and only while no other thread holds the internal mutex; `open()`
and `close()` wake it. Everything the engine does (`CAN_Read`, `CAN_Write`, the
TX FIFO, the settings cache) happens on that thread, under one recursive mutex
against every public method — so the class is thread-safe and the host's GUI
thread is never used for bus work.

Delivery follows from that: **signals are emitted on the engine thread** (the
ones a method produces directly, like `frameSent()` from an `immediate` send,
are emitted on the calling thread), always after the internal lock is released.
So every consumer connection must be **direct**. Which `connect()` form you use
decides whether you have to say so — checked against `qobject.h` of Qt 6.2.2
and the 5.15 branch:

```cpp
// A: no receiver/context object. Qt forwards this to
//    connect(sender, signal, sender, slot, Qt::DirectConnection), so it is
//    direct already.
QObject::connect(&can, &CanBus::frameReceived, &printFrame);
QObject::connect(&can, &CanBus::frameReceived, [](const CanBusFrame &f) { … });

// B: a receiver/context object is present. The trailing `type` argument then
//    defaults to Qt::AutoConnection, which resolves to queued across threads
//    and silently drops the frame here. Name the type yourself.
QObject::connect(&can, &CanBus::frameReceived, panel, &Panel::onFrame,
                 Qt::DirectConnection);

// C: plain callback, no meta-object system involved.
can.setFrameHandler([&sink](const CanBusFrame &f) { sink.push(f); });
```

Form **B** is the easy mistake: it compiles, runs, and never delivers. Note the
connection type is the *last* argument — a functor with a context object is
`connect(sender, signal, context, functor, type)`, and there is no
`connect(sender, signal, functor, type)` overload.

A direct slot runs between two engine ticks, with no lock held — exactly like
the frame handler, which is the plain-callback form of the same thing (form
**C**). Keep it short and thread-safe. It may call back into any method,
`close()` included: the engine takes the mutex again only once the whole batch
has been delivered, so re-opening or sending from a slot is equally safe. Two
things remain forbidden:

* blocking inside the slot on work another thread has to finish — the engine is
  stuck inside the slot, and so is everything waiting for the mutex behind it;
* destroying the `CanBus` object from a slot or from the handler (the engine
  thread cannot join itself).

`rxIntervalMs()`, `txIntervalMs()` and `isEngineRunning()` are atomic and need
no lock at all. `rxSlot()` and `txSlot()` remain public if you want to drive the
bus yourself (for instance from your own thread or in a test); the engine calls
the same code. `isEngineRunning()` reports whether the engine thread is alive.

## Which compiler? (ABI notes)

`CanBus` is a C++ class exported from the DLL (`class CANDLL_EXPORT CanBus`),
so a consumer must match the *toolchain family of the Qt kit it builds against* —
MSVC Qt (`msvc2022_64`) is built with cl.exe, MinGW Qt (`mingw_64`) with g++.
This is Qt's own requirement, not a limitation of PCAN:

* **Either compiler works.** `PCANBasic.dll` is a plain C DLL resolved at run
  time, so no MSVC-only import library is involved anywhere.
* **Do not mix.** An MSVC-built app against a MinGW-built `CanDLL.dll` (or vice
  versa) can crash on the C++ ABI boundary (`QString`, virtuals, name mangling).
  Pick one kit and build Qt, CanDLL and your app with it.
* **Rebuild consumers when the header changes.** Adding members (e.g. the
  engine's `m_worker` / `m_lock`, or `m_lastErrorCode`) changes `sizeof(CanBus)`,
  so an out-of-date app that constructs `CanBus` itself must be recompiled —
  there is no stable C ABI or d-pointer here. Adding a signal/slot does not
  change `sizeof(CanBus)` but does change its meta-object, so `moc` has to run
  again. Everything is intentionally in one header, so a normal rebuild is
  all it takes.

## API summary (`src/canbus.h`)

| Member | Description |
|---|---|
| `bool open(channel, bitrateKbps=500)` | Classic CAN. `channel` = `PCAN_USBBUS1`..16, `PCAN_PCIBUS1`..16, `PCAN_LANBUS1`..16, `usb1`, `pci2`, or raw handle `0x51`. Bit rates 5–1000 kbit/s |
| `bool openFd(channel, nominalKbps=500, dataKbps=2000)` | CAN FD; bit-timing strings are computed automatically (80 MHz clock) |
| `bool openFdRaw(channel, fdBitrateString)` | CAN FD with a raw PEAK string, e.g. `f_clock=80000000,nom_brp=10,nom_tseg1=13,...` |
| `void close()` | Uninitializes the channel, wakes the engine and drops pending commands (the engine thread itself lives until the object is destroyed) |
| `bool sendFrame(id, data, extended=false, rtr=false, immediate=false)` | Queue (or `immediate`: right now) a Classic CAN packet |
| `bool sendFdFrame(id, data, extended=false, bitRateSwitch=true, immediate=false)` | Queue (or `immediate`) a CAN FD frame (channel must be `openFd`) |
| `void txSlot()` | **TX pump slot**: pops up to `txIntervalBurst()` commands and writes them; emits `frameSent(id)`. The engine does the same every `txIntervalMs` — the slot exists for manual/testing use |
| `int pending()` / `void clearTxQueue()` | commands still in the FIFO / discard them |
| `setTxIntervalMs(ms)` / `setTxBurstPerTick(n)` | pump period (25 ms) / commands per tick (1 = max pacing) |
| `void rxSlot()` | **RX slot**: drains the queue with an **adaptive budget** (starts at 200 frames/tick, doubles while the queue is still full, halves back to 200 when it runs dry, ceiling 4000), emits `frameReceived` per frame. The engine does the same every `rxIntervalMs` (default 10 ms) — the slot exists for manual/testing use |
| `int rxBudget() const` | current adaptive drain budget (read-only, for diagnostics) |
| `bool isEngineRunning() const` | is the internal engine thread alive? (false before the first successful `open()`, false after the destructor) |
| `void setFrameHandler(std::function<void(const CanBusFrame &)>)` | plain callback for every received frame, called on the engine thread after `frameReceived()`; the delivery hook to use when a `Qt::DirectConnection` connect is not convenient. `nullptr` disables it |
| `bool filterMessages(from, to, extended)` / `resetQueues()` | HW receive filter / queue reset |
| `setListenOnly(bool)` / `setAutoReset(bool)` | Listen-only mode / bus-off auto reset |
| `openConfigured()` / `openFdConfigured()` | open with the rates from `candll.ini` |
| `saveConfiguration()` | persist this instance's channel + active rates |
| `lastError()`, signals `errorOccurred`, `openChanged`, `frameSent`, `txQueueChanged`, `frameReceived(CanBusFrame)` | diagnostics; every signal is emitted on the engine thread unless your own call produced it (see [Two Qt runtimes in one process](#two-qt-runtimes-in-one-process)) |
| `lastErrorCode()` / `lastErrorStatus()` | machine-readable error: `CanBus::ErrorCode` enum (`NoError`, `Api`, `BadChannel`, `BadParameter`, `NotOpen`, `NotInitialized`, `AlreadyOpen`, `QueueFull`, `Hardware`, `Other`) + the raw `TPCANStatus` bits (`0` for errors not reported by the driver) |

`CanBusFrame` carries `id, data, extended, rtr, fd, brs, esi, errorFrame,
timestampUs` (hardware timestamp in µs) and is registered as a metatype.

## Usage

```cpp
#include "canbus.h"

CanBus can;
// frames arrive on the engine thread: without a receiver/context object this
// overload is a direct connection already
QObject::connect(&can, &CanBus::frameReceived, [](const CanBusFrame &f) {
    qDebug() << "rx" << f.id << f.data.toHex();
});

// with a receiver or context object the type defaults to AutoConnection, so
// name it
QObject::connect(&can, &CanBus::frameReceived, panel, &RxPanel::onFrame, Qt::DirectConnection);

// ... or skip the meta-object system and install a plain callback
can.setFrameHandler([](const CanBusFrame &f) { /* thread-safe sink */ });

// Classic CAN @ 500 kbit/s
if (!can.open("PCAN_USBBUS1", 500))
    qWarning() << can.lastError();
can.sendFrame(0x123, QByteArray::fromHex("1122334455667788"));

// ... or CAN FD 500 kbit/s nominal, 2 Mbit/s data phase
// can.openFd("PCAN_USBBUS1", 500, 2000);
// can.sendFdFrame(0x123, QByteArray(64, '\x5A'));

// later: can.close();
```

Call `open()` from any thread: the engine thread is started there and then runs
on its own, so it also works before `QApplication::exec()` and on a `QThread`
worker thread. Frames arrive on the engine thread, not on the GUI thread — see
[Two Qt runtimes in one process](#two-qt-runtimes-in-one-process).

Use from another qmake project:

```pro
include($$PWD/../CanDLL/candll.pri)
```

## Rate settings (`candll.ini`, QSettings)

Bit rates and the default channel are persisted in an INI file so a bench
setup survives restarts and can be edited by hand:

```ini
; candll.ini  (next to the executable by default)
[Channel]
name=PCAN_USBBUS1
[Can]
bitrateKbps=500
[CanFD]
nominalKbps=500
dataKbps=2000
```

```cpp
CanBus::saveFdBitrateKbps(500, 2000);   // persist
CanBus::openFdConfigured();             // ... and open from the file
can.saveConfiguration();                // persist this instance's setup
CanBus::setSettingsFilePath("C:/ProgramData/MyApp/candll.ini"); // optional
```

| API | Keys / defaults |
|---|---|
| `saveCanBitrateKbps` / `canBitrateKbps()` | `Can/bitrateKbps` (500) |
| `saveFdBitrateKbps` / `fdNominalBitrateKbps()` / `fdDataBitrateKbps()` | `CanFD/nominalKbps` (500), `CanFD/dataKbps` (2000) |
| `saveChannelNameConfig` / `channelNameConfig()` | `Channel/name` (`PCAN_USBBUS1`) |
| `openConfigured(ch)` / `openFdConfigured(ch)` | open with stored rates; empty `ch` -> stored channel |
| `saveConfiguration()` | persist this instance's channel + active rate(s) |

## Example app

```bat
bin\CanTest                        :: PCAN_USBBUS1 @ 500 kbit/s, print rx
bin\CanTest --tx                   :: also send a 1 Hz heartbeat on 0x500
bin\CanTest --fd 500 2000          :: CAN FD 500k/2M
bin\CanTest PCAN_USBBUS2
bin\CanTest --settings             :: use channel + rates from candll.ini
bin\CanTest --fd --remember        :: open 500/2000 FD and store it to candll.ini
```

It binds `frameReceived` and `errorOccurred` with the three-argument `connect()`
forms (**A** above): no receiver or context object, which makes them direct
connections already, exactly what crossing the DLL boundary needs
(`example/main.cpp`). Use that as the reference for binding `CanBus` from your
own app — or, if you want your slot attached to a receiver object, pass
`Qt::DirectConnection` yourself as in **B**.

### Interactive console

Once open, the example shows a `can>` prompt. Type an id and hex payload:

```
can> 0x1AB DEAD BEEF        :: classic CAN, standard id (id > 0x7FF -> 29-bit)
can> 18DA00F1 02 10 01      :: 29-bit id, bytes separated or not
can> help                   :: usage
can> q                      :: quit (Ctrl+Z also works)
```

Manual sends bypass the TX FIFO (`immediate`), so replies from the ECU show up
right away, printed by the usual `frameReceived` handler. Reception runs on the
`CanBus` engine thread, and `printFrame()` is called there; stdin is read on a
separate thread so typing never blocks reception. Idle for 60 s and the example
exits.

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for the per-commit history. Highlights of
1.0 (`84c7775`): machine-readable errors (`ErrorCode` from `lastErrorCode()`,
raw driver bits from `lastErrorStatus()`) replacing string-only diagnostics, an
adaptive RX drain budget (`rxBudget()`) replacing the fixed 200-frame cap, and
a 25 ms default TX pump interval.

## Notes

* **TX pacing:** `sendFrame()`/`sendFdFrame()` return as soon as the command
  is in the FIFO; the pump writes one command per `txIntervalMs` (25 ms →
  40 frames/s at 8 bytes, well below 500 kbit/s load — no congestion). Tune
  `setTxIntervalMs()`/`setTxBurstPerTick()` for more throughput, or pass
  `immediate=true` to bypass the queue. A frame that fails to write is dropped
  and reported via `errorOccurred()`; the pump continues. Pending commands are
  dropped by `close()`.
* One `CanBus` instance = one channel; create several instances for several
  channels. Every public method takes the internal lock, so calls are safe from
  any thread; the pacing setters and `isEngineRunning()` are lock-free atomics.
* **No timers, no event loop needed:** the RX poll and the TX pump run on the
  library's own engine thread, a plain `std::thread` started by the first
  successful `open()`. Nothing in `CanBus` uses `QTimer`, because inside this DLL
  `QObject::startTimer()` cannot succeed for a thread whose dispatcher is
  registered in the host's QtCore copy (see
  [Two Qt runtimes in one process](#two-qt-runtimes-in-one-process)). Practical
  consequences: `open()` works before `QApplication::exec()` and on a thread that
  never runs an event loop; the engine keeps running while the host's loop is
  busy; and frames arrive on the engine thread, so bind them with form **A** or
  **B** above, or with `setFrameHandler()`. `close()` stops the ticking and
  wakes the engine out of its wait; the thread itself is joined in the
  destructor, so destroying the object never leaves a thread behind.
* `sendFrame()` also works on an FD-initialized channel (the frame is sent as
  a classic frame); `sendFdFrame()` requires `openFd*()`.
* RX uses non-blocking `CAN_Read` / `CAN_ReadFD`. Each engine tick drains with
  an **adaptive budget** instead of a fixed cap: it starts at 200 frames, doubles
  whenever a tick consumes its whole budget (the driver queue was still not
  empty) up to a ceiling of 4000, and halves back to 200 as soon as a tick runs
  dry. A flood is therefore absorbed with fewer driver round-trips, while the
  work done per tick stays bounded so the engine keeps its `rxIntervalMs`
  deadline.
* Errors are available three ways: the human-readable `lastError()` string and
  `errorOccurred()` signal, the `CanBus::ErrorCode` enum from `lastErrorCode()`
  for branching, and the raw driver bits via `lastErrorStatus()`.
* Bus-off errors surface through `errorOccurred()`; enable `setAutoReset(true)`
  to let the driver recover automatically.
