#include "canbus.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

// ============================================================================
// PCANBasic.dll binding
// ============================================================================

struct CanBus::Api
{
    FnCAN_Initialize      Initialize      = nullptr;
    FnCAN_InitializeFD    InitializeFD    = nullptr;
    FnCAN_Uninitialize    Uninitialize    = nullptr;
    FnCAN_Reset           Reset           = nullptr;
    FnCAN_GetStatus       GetStatus       = nullptr;
    FnCAN_Read            Read            = nullptr;
    FnCAN_ReadFD          ReadFD          = nullptr;
    FnCAN_Write           Write           = nullptr;
    FnCAN_WriteFD         WriteFD         = nullptr;
    FnCAN_FilterMessages  FilterMessages  = nullptr;
    FnCAN_GetValue        GetValue        = nullptr;
    FnCAN_SetValue        SetValue        = nullptr;
    FnCAN_GetErrorText    GetErrorText    = nullptr;

    bool isComplete() const
    {
        return Initialize && InitializeFD && Uninitialize && Reset && Read &&
               ReadFD && Write && WriteFD && GetValue && SetValue;
    }
};

// Resolves all entry points once from the given library (loaded DLL stays
// loaded for the process lifetime, so resolved pointers stay valid).
// A member function (not a free one or a lambda) because Api is a private
// nested type of CanBus.
CanBus::Api CanBus::makeApi(QLibrary &lib)
{
    CanBus::Api a;
    if (lib.load()) {
        a.Initialize     = reinterpret_cast<FnCAN_Initialize>(lib.resolve("CAN_Initialize"));
        a.InitializeFD   = reinterpret_cast<FnCAN_InitializeFD>(lib.resolve("CAN_InitializeFD"));
        a.Uninitialize   = reinterpret_cast<FnCAN_Uninitialize>(lib.resolve("CAN_Uninitialize"));
        a.Reset          = reinterpret_cast<FnCAN_Reset>(lib.resolve("CAN_Reset"));
        a.GetStatus      = reinterpret_cast<FnCAN_GetStatus>(lib.resolve("CAN_GetStatus"));
        a.Read           = reinterpret_cast<FnCAN_Read>(lib.resolve("CAN_Read"));
        a.ReadFD         = reinterpret_cast<FnCAN_ReadFD>(lib.resolve("CAN_ReadFD"));
        a.Write          = reinterpret_cast<FnCAN_Write>(lib.resolve("CAN_Write"));
        a.WriteFD        = reinterpret_cast<FnCAN_WriteFD>(lib.resolve("CAN_WriteFD"));
        a.FilterMessages = reinterpret_cast<FnCAN_FilterMessages>(lib.resolve("CAN_FilterMessages"));
        a.GetValue       = reinterpret_cast<FnCAN_GetValue>(lib.resolve("CAN_GetValue"));
        a.SetValue       = reinterpret_cast<FnCAN_SetValue>(lib.resolve("CAN_SetValue"));
        a.GetErrorText   = reinterpret_cast<FnCAN_GetErrorText>(lib.resolve("CAN_GetErrorText"));
    }
    return a;
}

CanBus::Api *CanBus::resolveApi()
{
    // QLibrary loads PCANBasic.dll from the PEAK driver installation
    // (already on the default DLL search path as a system component).
    static QLibrary lib(QStringLiteral("PCANBasic"));
    static Api api = makeApi(lib);   // initialized on first call, thread-safe
    return &api;
}

// ============================================================================
// Construction / destruction
// ============================================================================

CanBus::CanBus(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<CanBusFrame>("CanBusFrame");
}

CanBus::~CanBus()
{
    // The worker must be gone before QObject's data starts going away: after
    // this returns nothing can touch this object any more.
    stopEngine();
    close();
}

// ============================================================================
// The engine thread
// ============================================================================

namespace {
// Longest the engine will sleep when the channel is closed: an open request
// wakes it immediately, this is only the backstop for a missed notification.
constexpr int kEngineIdleMs = 100;

// Bounds for the adaptive RX drain budget used by drainRxLocked().
constexpr int kRxBudgetDefault = 200;   // steady state: normal traffic
constexpr int kRxBudgetMin     = 20;    // never read fewer than this per tick
constexpr int kRxBudgetMax     = 4000;  // ceiling so a flood cannot hog the loop
} // namespace

void CanBus::engineLoop()
{
    using clock = std::chrono::steady_clock;

    // Two independent deadlines: RX drains the receive queue, TX paces the send
    // queue. Both start out due, so the first tick works immediately and a channel
    // opened later is picked up without delay. They are locals of this thread only,
    // so they need no lock.
    clock::time_point nextRx = clock::now();
    clock::time_point nextTx = nextRx;

    while (!m_stopRequested.load()) {
        Emissions out;
        {
            std::lock_guard<std::recursive_mutex> g(m_lock);
            if (m_open) {
                const auto now = clock::now();
                if (nextRx <= now) {
                    drainRxLocked(out);
                    nextRx = now
                        + std::chrono::milliseconds(std::max(1, m_rxIntervalMs.load()));
                }
                if (nextTx <= now) {
                    pumpTxLocked(out);
                    nextTx = now
                        + std::chrono::milliseconds(std::max(1, m_txIntervalMs.load()));
                }
            }
        }
        // Signals and the frame handler run with no lock held.
        emitAfterUnlock(out);

        // Sleep outside the lock until the earlier deadline: RX and TX keep their
        // own periods, so a slow TX interval cannot slow reception down. While the
        // channel is closed nothing is due, so the tick falls back to kEngineIdleMs;
        // opening, re-opening and closing wake the engine through m_wakePending
        // instead of waiting for that period to expire.
        clock::time_point due;
        {
            std::lock_guard<std::recursive_mutex> g(m_lock);
            const auto now = clock::now();
            due = m_open ? std::min(nextRx, nextTx)
                         : now + std::chrono::milliseconds(kEngineIdleMs);
        }
        if (due <= clock::now())
            continue;                  // already due: loop now, do not sleep on it

        std::unique_lock<std::mutex> wl(m_wakeMtx);
        // m_wakePending is cleared after the wait, not before: a wakeEngine() that
        // lands while the deadline is computed must shorten this wait, not be lost.
        m_wake.wait_until(wl, due, [this] {
            return m_wakePending || m_stopRequested.load();
        });
        if (m_stopRequested.load())
            break;
        m_wakePending = false;
    }
    m_engineRunning.store(false);
}

// Private helper: call with m_lock held. Idempotent. It never joins: the caller can
// be the engine thread itself (a close()/re-open performed from a slot connected
// with Qt::DirectConnection), and joining this_thread would abort the process.
// A thread the OS refuses to create throws, which under Qt's MSVC mkspecs
// (-EHs-c-) terminates the process - exactly what Qt's own thread class does in
// that configuration.
void CanBus::startEngine()
{
    if (m_stopRequested.load())
        return;                       // the destructor has begun: do not respawn

    if (m_worker.joinable()) {
        // Already running. It idles at kEngineIdleMs while the channel is closed,
        // so a reopen after close() has to be announced - waiting that period out
        // would delay the first RX drain and the first TX pump.
        wakeEngine();
        return;
    }

    m_stopRequested.store(false);
    m_engineRunning.store(true);      // published before the thread exists
    m_worker = std::thread(&CanBus::engineLoop, this);
    wakeEngine();
}

// Destructor only, call with no lock held.
void CanBus::stopEngine()
{
    // Setting the flag under m_lock is what makes the shutdown airtight:
    // startEngine() runs under that same lock and returns without spawning once
    // the flag is set, so no worker can appear between the flag and the join.
    // The thread object is handed over under the lock and joined only after it
    // is released - the worker needs m_lock to finish the pass it is in.
    std::thread worker;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        m_stopRequested.store(true);
        worker = std::move(m_worker);
        wakeEngine();
    }

    if (worker.joinable()) {
        if (worker.get_id() == std::this_thread::get_id()) {
            // The object is being destroyed from the engine thread itself - a slot
            // or handler that deletes it, which the header forbids. join() would
            // throw and, from a destructor, that means std::terminate; detach
            // instead and let the worker observe m_stopRequested and exit.
            worker.detach();
        } else {
            worker.join();
        }
    }
    m_engineRunning.store(false);
}

void CanBus::wakeEngine()
{
    {
        std::lock_guard<std::mutex> g(m_wakeMtx);
        m_wakePending = true;
    }
    m_wake.notify_one();
}

// The delivery step: called by whoever collected the emissions, after m_lock
// has been released. Errors first, then the open state, then frames in arrival
// order, so a handler that calls close() sees a consistent object.
void CanBus::emitAfterUnlock(const Emissions &out)
{
    if (out.frames.isEmpty() && out.sent.isEmpty() && out.errors.isEmpty() &&
        !out.queueChanged && out.openState < 0)
        return;   // nothing collected: keep an idle engine loop silent

    // The frame handler sees the same frames as frameReceived(), before any of
    // them reaches a queued QML connection. No lock is held here, so the handler
    // may call back into CanBus - close() included - but must not destroy it.
    // One snapshot per batch, taken before anything is delivered: a handler that
    // an errorOccurred/openChanged slot installs must not start seeing frames
    // that were collected before it existed.
    std::function<void(const CanBusFrame &)> handler;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        handler = m_frameHandler;   // copy: keep it alive across the calls
    }

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

// ============================================================================
// Channel / bit-rate helpers
// ============================================================================

QString CanBus::buildStamp()
{
    return QStringLiteral(__DATE__ " " __TIME__);
}

quint16 CanBus::channelHandle(const QString &name, bool *ok)
{
    if (ok)
        *ok = false;

    QString n = name.trimmed();
    n.remove(QLatin1Char(' '));
    n.remove(QLatin1Char('-'));
    n.remove(QLatin1Char('_'));   // "PCAN_USBBUS1" -> "PCANUSBBUS1"
    n = n.toUpper();
    if (n.startsWith(QStringLiteral("PCAN")))
        n.remove(0, 4);
    if (n.startsWith(QStringLiteral("BUS")))
        n.remove(0, 3);
    if (n.isEmpty())
        return PCAN_NONEBUS;

    // Numeric raw handle: "0x51", "81", ...
    if (n.at(0).isDigit()) {
        bool numOk = false;
        const uint v = n.startsWith(QStringLiteral("0X"))
                           ? n.mid(2).toUInt(&numOk, 16)
                           : n.toUInt(&numOk, 10);
        if (numOk && v > 0 && v <= 0xFFFF) {
            if (ok)
                *ok = true;
            return quint16(v);
        }
        return PCAN_NONEBUS;
    }

    // "USB1", "PCI12", "ISA2", "PCC1", "LAN3"
    static const struct { const char *prefix; quint16 base8; quint16 high; quint8 maxCh; } devs[] = {
        { "USB", 0x50, 0x500, 16 },   // ch 1..8 -> 0x51..0x58, ch 9..16 -> 0x509..0x510
        { "PCI", 0x40, 0x400, 16 },   // ch 1..8 -> 0x41..0x48, ch 9..16 -> 0x409..0x410
        { "ISA", 0x20, 0x000,  8 },
        { "PCC", 0x60, 0x000,  2 },
        { "LAN", 0x800, 0x000, 16 },  // ch 1..16 -> 0x801..0x810
    };

    for (const auto &d : devs) {
        const QString prefix = QString::fromLatin1(d.prefix);
        if (!n.startsWith(prefix))
            continue;
        QString rest = n.mid(prefix.size());
        if (rest.startsWith(QStringLiteral("BUS")))   // "PCAN_USBBUS2" -> "2"
            rest.remove(0, 3);
        bool chOk = false;
        const int ch = rest.toInt(&chOk);
        if (!chOk || ch < 1 || ch > d.maxCh)
            return PCAN_NONEBUS;
        if (ok)
            *ok = true;
        if (QString::fromLatin1(d.prefix) == QLatin1String("LAN"))
            return quint16(0x800 + ch);
        if (ch <= 8)
            return quint16(d.base8 + ch);
        return quint16(d.high + ch);   // 0x509 + .. -> equals d.high + ch
    }
    return PCAN_NONEBUS;
}

quint8 CanBus::fdDlc(quint32 lenBytes)
{
    if (lenBytes <= 8)
        return quint8(lenBytes);
    for (quint8 dlc = 9; dlc <= 15; ++dlc) {
        if (lenBytes <= fdLength(dlc))
            return dlc;
    }
    return 15;
}

quint32 CanBus::fdLength(quint8 dlc)
{
    // FD DLC codes: 0..8 direct, 9..15 -> 12,16,24,32,48,64
    static const quint32 lenOf[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8,
                                     12, 16, 24, 32, 48, 64, 64 };
    return lenOf[dlc <= 15 ? dlc : 15];
}

QString CanBus::fdBitrateString(int nominalKbps, int dataKbps, quint32 clockHz)
{
    // Computes BTR-equivalent timing for one phase of the FD frame.
    // Returns the finest grid (largest time-quant count) that satisfies
    // tseg1 <= maxTseg1, 1 <= tseg2 <= min(tseg1, maxTseg2), target sample
    // point ~ spPercent.
    auto phaseTiming = [clockHz](int kbps, quint32 maxTseg1, quint32 maxTseg2,
                                 int spPercent,
                                 quint32 &brp, quint32 &tseg1, quint32 &tseg2,
                                 quint32 &sjw) -> bool {
        if (kbps <= 0 || clockHz == 0)
            return false;
        const quint64 bps = quint64(kbps) * 1000ull;
        quint32 bestN = 0;
        brp = tseg1 = tseg2 = sjw = 0;
        for (quint32 b = 1; b <= 1024; ++b) {
            const quint64 denom = quint64(b) * bps;
            if (denom > clockHz)
                break;
            if (clockHz % quint32(denom) != 0)
                continue;
            const quint32 n = quint32(quint64(clockHz) / denom);  // tq per bit
            if (n < 4 || n > maxTseg1 + maxTseg2 + 1u)
                continue;
            quint32 t1 = (n * quint32(spPercent)) / 100;
            quint32 t2 = n - 1 - t1;
            if (t2 < 1 || t1 > maxTseg1 || t2 > maxTseg2 || t2 > t1) {
                t2 = 1;
                t1 = n - 2;
            }
            if (t1 < 1 || t2 < 1 || t1 > maxTseg1 || t2 > maxTseg2 || t2 > t1)
                continue;
            if (n > bestN) {
                bestN = n;
                brp   = b;
                tseg1 = t1;
                tseg2 = t2;
            }
        }
        if (bestN == 0)
            return false;
        sjw = qMin(tseg2, 4u);
        return true;
    };

    quint32 nbrp = 0, nt1 = 0, nt2 = 0, nsjw = 0, dbrp = 0, dt1 = 0, dt2 = 0, dsjw = 0;
    if (!phaseTiming(nominalKbps, 256, 128, 80, nbrp, nt1, nt2, nsjw) ||
        !phaseTiming(dataKbps,    32,  32, 75, dbrp, dt1, dt2, dsjw))
        return QString();

    return QStringLiteral("f_clock=%1,nom_brp=%2,nom_tseg1=%3,nom_tseg2=%4,nom_sjw=%5,"
                          "data_brp=%6,data_tseg1=%7,data_tseg2=%8,data_sjw=%9")
        .arg(clockHz).arg(nbrp).arg(nt1).arg(nt2).arg(nsjw)
              .arg(dbrp).arg(dt1).arg(dt2).arg(dsjw);
}

// ============================================================================
// Open / close
// ============================================================================

// Private helpers: call with m_lock held. They never emit; they record what
// has to be announced in `out`, and the public wrapper emits after unlocking.
bool CanBus::openChannel(const QString &channel, Emissions &out)
{
    if (m_open) {
        recordError(out, ErrorCode::AlreadyOpen,
                    QStringLiteral("channel already open (%1)").arg(m_channelName));
        return false;
    }
    m_api = resolveApi();
    if (!m_api->isComplete()) {
        recordError(out, ErrorCode::Api, QStringLiteral("PCANBasic.dll not found - install the PEAK-System "
                                "PCAN driver (https://www.peak-system.com)"));
        return false;
    }
    bool ok = false;
    const TPCANHandle h = channelHandle(channel, &ok);
    if (!ok || h == PCAN_NONEBUS) {
        recordError(out, ErrorCode::BadChannel,
                    QStringLiteral("unknown channel '%1' (e.g. PCAN_USBBUS1, usb2, 0x51)")
                     .arg(channel));
        return false;
    }
    // Friendly error if the hardware is not plugged in (when supported).
    quint32 cond = PCAN_CHANNEL_AVAILABLE;
    if (m_api->GetValue &&
        m_api->GetValue(h, PCAN_CHANNEL_CONDITION, &cond, sizeof(cond)) == PCAN_ERROR_OK &&
        cond == PCAN_CHANNEL_UNAVAILABLE) {
        recordError(out, ErrorCode::NotInitialized,
                    QStringLiteral("channel %1 is not available (device not plugged in "
                                   "or driver not running)").arg(channel));
        return false;
    }
    m_channel     = h;
    m_channelName = channel.trimmed();
    return true;
}

bool CanBus::initClassic(const QString &channel, int bitrateKbps, Emissions &out)
{
    if (!openChannel(channel, out))
        return false;

    TPCANBaudrate btr = 0;
    switch (bitrateKbps) {
    case 1000: btr = PCAN_BAUD_1M;   break;
    case  800: btr = PCAN_BAUD_800K; break;
    case  500: btr = PCAN_BAUD_500K; break;
    case  250: btr = PCAN_BAUD_250K; break;
    case  125: btr = PCAN_BAUD_125K; break;
    case  100: btr = PCAN_BAUD_100K; break;
    case   95: btr = PCAN_BAUD_95K;  break;
    case   83: btr = PCAN_BAUD_83K;  break;
    case   50: btr = PCAN_BAUD_50K;  break;
    case   47: btr = PCAN_BAUD_47K;  break;
    case   33: btr = PCAN_BAUD_33K;  break;
    case   20: btr = PCAN_BAUD_20K;  break;
    case   10: btr = PCAN_BAUD_10K;  break;
    case    5: btr = PCAN_BAUD_5K;   break;
    default:
        recordError(out, ErrorCode::NotInitialized,
                    QStringLiteral("unsupported bit rate %1 kbit/s").arg(bitrateKbps));
        m_channel = PCAN_NONEBUS;
        return false;
    }

    const TPCANStatus st = m_api->Initialize(m_channel, btr, 0, 0, 0);
    if (st != PCAN_ERROR_OK) {
        raiseError(out, st);
        m_channel = PCAN_NONEBUS;
        return false;
    }

    m_fd   = false;
    m_open = true;
    m_rxBudget = kRxBudgetDefault;   // fresh channel: start from the steady budget
    m_nominalKbps = bitrateKbps;
    applyOptions();
    clearError();
    startEngine();                  // the engine polls until close()
    out.openState = 1;
    return true;
}

// Public wrappers: take m_lock, call the private helper, release, then emit.
bool CanBus::open(const QString &channel, int bitrateKbps)
{
    Emissions out;
    bool ok = false;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        ok = initClassic(channel, bitrateKbps, out);
    }
    emitAfterUnlock(out);
    return ok;
}

bool CanBus::initFd(const QString &channel, const QString &fdBitrateString, Emissions &out)
{
    if (!openChannel(channel, out))
        return false;

    const QByteArray br = fdBitrateString.trimmed().toLatin1();
    const TPCANStatus st = m_api->InitializeFD(m_channel, br.constData());
    if (st != PCAN_ERROR_OK) {
        raiseError(out, st);
        m_channel = PCAN_NONEBUS;
        return false;
    }

    m_fd   = true;
    m_open = true;
    m_rxBudget = kRxBudgetDefault;   // fresh channel: start from the steady budget
    applyOptions();
    clearError();
    startEngine();                  // the engine polls until close()
    out.openState = 1;
    return true;
}

bool CanBus::openFd(const QString &channel, int nominalKbps, int dataKbps)
{
    // Note the explicit CanBus::: the local name would otherwise shadow the
    // static member fdBitrateString().
    const QString br = CanBus::fdBitrateString(nominalKbps, dataKbps);
    Emissions out;
    bool ok = false;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        if (br.isEmpty()) {
            recordError(out, ErrorCode::NotInitialized,
                        QStringLiteral("no valid CAN FD bit timing for %1/%2 kbit/s")
                            .arg(nominalKbps).arg(dataKbps));
        } else {
            m_nominalKbps = nominalKbps;
            m_dataKbps    = dataKbps;
            ok = initFd(channel, br, out);
        }
    }
    emitAfterUnlock(out);
    return ok;
}

bool CanBus::openFdRaw(const QString &channel, const QString &fdBitrateString)
{
    Emissions out;
    bool ok = false;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        ok = initFd(channel, fdBitrateString, out);
    }
    emitAfterUnlock(out);
    return ok;
}

void CanBus::close()
{
    Emissions out;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        // Closing an object that never held a channel is silent: openChanged(false)
        // would announce a state the host never saw. A channel that died on its own
        // reported openChanged(false) when the read failed, so only a currently open
        // one reports it here - while handle and FD mode are reset in both cases,
        // which is what turns a second close() into a no-op.
        const bool wasOpen    = m_open;
        const bool hadChannel = wasOpen || m_channel != PCAN_NONEBUS;
        // A closed channel has no handle to uninitialise; PCAN_ERROR_HARD for
        // PCAN_NONEBUS is harmless but pointless on every close() and in ~CanBus.
        if (m_api && m_api->Uninitialize && m_channel != PCAN_NONEBUS)
            m_api->Uninitialize(m_channel);
        if (hadChannel) {
            m_open    = false;
            m_fd      = false;
            m_channel = PCAN_NONEBUS;
            if (wasOpen)
                out.openState = 0;
        }
        if (!m_txQueue.isEmpty()) {
            m_txQueue.clear();          // pending commands are dropped on close
            out.queueChanged = true;
        }
        // The engine cannot be inside CAN_Read here (reads happen under this same
        // lock), but it may be asleep for up to rxIntervalMs(); waking it makes it
        // observe m_open == false at once instead of after its next tick.
        wakeEngine();
    }
    emitAfterUnlock(out);
}

// Call with m_lock held (it writes the channel parameters). Emits nothing: the
// options are applied while opening and while setters hold the lock. The guard is
// recursive, so a direct call from another locked scope is equally safe.
void CanBus::applyOptions()
{
    std::lock_guard<std::recursive_mutex> g(m_lock);
    if (!m_api || !m_api->SetValue)
        return;
    if (m_listenOnly) {
        quint32 on = PCAN_PARAMETER_ON;
        m_api->SetValue(m_channel, PCAN_LISTEN_ONLY, &on, sizeof(on));
    }
    if (m_autoReset) {
        quint32 on = PCAN_PARAMETER_ON;
        m_api->SetValue(m_channel, PCAN_BUSOFF_AUTORESET, &on, sizeof(on));
    }
}

// ============================================================================
// Configuration setters
// ============================================================================

// The pacing values are atomic: the engine reads them on every tick, and a
// setter that is called from the engine thread's own signal must not take
// m_lock. Reading the new value on the next tick is prompt enough.
void CanBus::setRxIntervalMs(int ms)
{
    m_rxIntervalMs.store(qMax(1, ms));
}

void CanBus::setListenOnly(bool on)
{
    Emissions out;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        m_listenOnly = on;
        if (m_open && m_api && m_api->SetValue) {
            quint32 v = on ? PCAN_PARAMETER_ON : PCAN_PARAMETER_OFF;
            const TPCANStatus st =
                m_api->SetValue(m_channel, PCAN_LISTEN_ONLY, &v, sizeof(v));
            if (st != PCAN_ERROR_OK)
                raiseError(out, st);
        }
    }
    emitAfterUnlock(out);
}

void CanBus::setAutoReset(bool on)
{
    Emissions out;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        m_autoReset = on;
        if (m_open && m_api && m_api->SetValue) {
            quint32 v = on ? PCAN_PARAMETER_ON : PCAN_PARAMETER_OFF;
            const TPCANStatus st =
                m_api->SetValue(m_channel, PCAN_BUSOFF_AUTORESET, &v, sizeof(v));
            if (st != PCAN_ERROR_OK)
                raiseError(out, st);
        }
    }
    emitAfterUnlock(out);
}

// ============================================================================
// Transmission (FIFO queue + TX pump on the engine thread)
// ============================================================================

// Private helper: call with m_lock held; records errors in `out`.
bool CanBus::validateForSend(const TxItem &item, Emissions &out)
{
    if (!m_open || !m_api) {
        recordError(out, ErrorCode::NotOpen, QStringLiteral("send: channel is closed"));
        return false;
    }
    const quint32 idMax = item.extended ? 0x1FFFFFFFu : 0x7FFu;
    if (item.id > idMax) {
        recordError(out, ErrorCode::BadParameter,
                 QStringLiteral("send: ID 0x%1 out of range for %2 frame")
                     .arg(item.id, 0, 16).arg(item.extended ? "extended" : "standard"));
        return false;
    }
    if (item.fd && !m_fd) {
        recordError(out, ErrorCode::BadParameter,
                 QStringLiteral("sendFdFrame: channel was opened as Classic CAN - "
                                "use openFd() first"));
        return false;
    }
    if (!item.fd && item.data.size() > 8) {
        recordError(out, ErrorCode::BadParameter,
                 QStringLiteral("sendFrame: classic CAN payload is max 8 bytes (got %1)")
                     .arg(item.data.size()));
        return false;
    }
    if (item.fd && item.data.size() > 64) {
        recordError(out, ErrorCode::BadParameter,
                 QStringLiteral("sendFdFrame: CAN FD payload is max 64 bytes (got %1)")
                     .arg(item.data.size()));
        return false;
    }
    return true;
}

// Private helper: call with m_lock held.
bool CanBus::enqueue(TxItem item, Emissions &out)
{
    const bool wasEmpty = m_txQueue.isEmpty();
    m_txQueue.enqueue(item);
    if (wasEmpty)
        out.queueChanged = true;
    // No wakeEngine() here: the engine wakes on its own every txIntervalMs at
    // the latest, and a burst of sends then becomes one pump of the whole
    // queue instead of one write per wakeup.
    return true;
}

TPCANStatus CanBus::writeItem(const TxItem &item)
{
    if (item.fd) {
        TPCANMsgFD msg;
        std::memset(&msg, 0, sizeof(msg));
        msg.ID      = item.id;
        msg.MSGTYPE = TPCANMessageType(PCAN_MESSAGE_FD |
                                       (item.extended ? PCAN_MESSAGE_EXTENDED : 0) |
                                       (item.brs ? PCAN_MESSAGE_BRS : 0));
        msg.DLC     = fdDlc(quint32(item.data.size()));
        if (!item.data.isEmpty())
            std::memcpy(msg.DATA, item.data.constData(), size_t(item.data.size()));
        return m_api->WriteFD(m_channel, &msg);
    }

    TPCANMsg msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.ID      = item.id;
    msg.MSGTYPE = item.extended ? TPCANMessageType(PCAN_MESSAGE_EXTENDED)
                                : TPCANMessageType(PCAN_MESSAGE_STANDARD);
    if (item.rtr)
        msg.MSGTYPE = TPCANMessageType(msg.MSGTYPE | PCAN_MESSAGE_RTR);
    msg.LEN = quint8(item.data.size());
    if (!item.data.isEmpty())
        std::memcpy(msg.DATA, item.data.constData(), size_t(item.data.size()));
    return m_api->Write(m_channel, &msg);
}

bool CanBus::sendFrame(quint32 id, const QByteArray &data, bool extended, bool rtr,
                       bool immediate)
{
    TxItem item;
    item.id       = id;
    item.data     = data;
    item.extended = extended;
    item.rtr      = rtr;
    item.fd       = false;

    Emissions out;
    bool ok = false;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        if (validateForSend(item, out)) {
            if (immediate) {
                // CAN_Write under the lock: the engine cannot interleave a
                // write or a read on the same handle while this is in flight.
                const TPCANStatus st = writeItem(item);
                if (st == PCAN_ERROR_OK) {
                    clearError();
                    out.sent.append(id);
                    ok = true;
                } else {
                    // Reported after the unlock; a failed immediate write is
                    // still an error, as before.
                    raiseError(out, st);
                }
            } else {
                enqueue(item, out);
                ok = true;
            }
        }
    }
    emitAfterUnlock(out);
    return ok;
}

bool CanBus::sendFdFrame(quint32 id, const QByteArray &data, bool extended,
                         bool bitRateSwitch, bool immediate)
{
    TxItem item;
    item.id       = id;
    item.data     = data;
    item.extended = extended;
    item.fd       = true;
    item.brs      = bitRateSwitch;

    Emissions out;
    bool ok = false;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        if (validateForSend(item, out)) {
            if (immediate) {
                const TPCANStatus st = writeItem(item);
                if (st == PCAN_ERROR_OK) {
                    clearError();
                    out.sent.append(id);
                    ok = true;
                } else {
                    raiseError(out, st);
                }
            } else {
                enqueue(item, out);
                ok = true;
            }
        }
    }
    emitAfterUnlock(out);
    return ok;
}

// Private helper: call with m_lock held. Writes at most txBurstPerTick()
// commands; the engine calls it every tick, the queue is drained over ticks.
void CanBus::pumpTxLocked(Emissions &out)
{
    if (!m_open || m_txQueue.isEmpty())
        return;

    const int burst = std::max(1, m_txBurstPerTick.load());
    for (int i = 0; i < burst && !m_txQueue.isEmpty(); ++i) {
        const TxItem item = m_txQueue.dequeue();
        const TPCANStatus st = writeItem(item);
        if (st == PCAN_ERROR_OK) {
            clearError();
            out.sent.append(item.id);
        } else {
            // Failed frame is dropped; the pump continues with the next one.
            raiseError(out, st);
        }
    }

    // Only the non-empty -> empty transition is announced, so the signal keeps
    // marking the edges of a busy queue for the status bar.
    if (m_txQueue.isEmpty())
        out.queueChanged = true;
}

// The TX pump step, callable from QML. Takes the lock, pumps, then emits.
void CanBus::txSlot()
{
    Emissions out;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        pumpTxLocked(out);
    }
    emitAfterUnlock(out);
}

void CanBus::clearTxQueue()
{
    Emissions out;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        if (!m_txQueue.isEmpty()) {
            m_txQueue.clear();
            out.queueChanged = true;
        }
    }
    emitAfterUnlock(out);   // an empty batch is dropped by its early return
}

void CanBus::setTxIntervalMs(int ms)
{
    m_txIntervalMs.store(qMax(1, ms));
}

void CanBus::setTxBurstPerTick(int n)
{
    m_txBurstPerTick.store(qMax(1, n));
}

bool CanBus::filterMessages(quint32 fromId, quint32 toId, bool extended)
{
    Emissions out;
    bool ok = false;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        if (!m_open || !m_api || !m_api->FilterMessages) {
            recordError(out, ErrorCode::NotOpen, QStringLiteral("filterMessages: channel is closed"));
        } else {
            // PCAN_MODE_* are plain message-type flags (11-bit vs 29-bit identifiers).
            const TPCANMode mode = extended ? TPCANMode(PCAN_MODE_EXTENDED)
                                            : TPCANMode(PCAN_MODE_STANDARD);
            const TPCANStatus st = m_api->FilterMessages(m_channel, fromId, toId, mode);
            if (st != PCAN_ERROR_OK)
                raiseError(out, st);
            else
                ok = true;
        }
    }
    emitAfterUnlock(out);
    return ok;
}

bool CanBus::resetQueues()
{
    Emissions out;
    bool ok = false;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        if (!m_open || !m_api) {
            recordError(out, ErrorCode::NotOpen, QStringLiteral("resetQueues: channel is closed"));
        } else {
            const TPCANStatus st = m_api->Reset(m_channel);
            if (st != PCAN_ERROR_OK) {
                raiseError(out, st);
            } else {
                ok = true;
                // CAN_Reset also empties the driver's TX queue, so the commands
                // still waiting here could never be written: drop them and tell
                // the UI, so pending() matches the hardware again.
                if (!m_txQueue.isEmpty()) {
                    m_txQueue.clear();
                    out.queueChanged = true;
                }
            }
        }
    }
    emitAfterUnlock(out);
    return ok;
}

// ============================================================================
// Reception (the rx slot, driven by the engine thread)
// ============================================================================

// Drains the PCAN receive queue, bounded by the adaptive budget. Called with
// m_lock held: holding the lock across the reads is what keeps the channel
// handle from being uninitialised underneath them. Frames, errors and the
// openChanged(false) of a channel that died are only recorded here -
// drainRxLocked() never emits, because the caller emits the batch after
// releasing the lock.
void CanBus::drainRxLocked(Emissions &out)
{
    // Adaptive drain: read up to the current budget. A round that consumes its
    // whole budget means the PCAN queue was still non-empty, so the budget is
    // doubled for the next round (up to kRxBudgetMax) to keep up with a burst.
    // A round that runs dry early means the queue is shallower than the budget,
    // so it is halved back towards kRxBudgetDefault. Either way the work per
    // round stays bounded, so neither the engine nor the UI is starved.
    const int budget = qMax(m_rxBudget, kRxBudgetMin);
    int count = 0;
    for (; count < budget; ++count) {
        const ReadResult r = readOneLocked(out);
        if (r == ReadResult::Stopped) {
            m_rxBudget = kRxBudgetDefault;
            return;
        }
        if (r == ReadResult::Empty)
            break;
    }

    if (count == budget)
        m_rxBudget = qMin(budget * 2, kRxBudgetMax);     // queue was deeper: grow
    else if (budget > kRxBudgetDefault)
        m_rxBudget = qMax(budget / 2, kRxBudgetDefault); // queue is shallow: relax
    else
        m_rxBudget = kRxBudgetDefault;                   // steady state
}

// The public rx slot: still callable directly (and from the engine on demand),
// but now it takes the lock like every other entry point and emits afterwards.
void CanBus::rxSlot()
{
    Emissions out;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        if (!m_open || !m_api)
            return;
        drainRxLocked(out);
    }
    emitAfterUnlock(out);
}

// Reads one message from the receive queue into `out`. Returns Empty when the
// queue is drained, Stopped when the read failed (m_open is cleared and
// out.openState records a channel that went away), Frame when a frame was
// appended. Errors are only recorded here; emitting them is the caller's job,
// after the lock is released.
CanBus::ReadResult CanBus::readOneLocked(Emissions &out)
{
    CanBusFrame frame;

    if (m_fd) {
        TPCANMsgFD     msg;
        TPCANTimestampFD ts = 0;
        std::memset(&msg, 0, sizeof(msg));
        const TPCANStatus st = m_api->ReadFD(m_channel, &msg, &ts);
        if (st == PCAN_ERROR_QRCVEMPTY)
            return ReadResult::Empty;
        if (st != PCAN_ERROR_OK) {
            if (st == PCAN_ERROR_INITIALIZE) {   // channel went away
                m_open = false;
                out.openState = 0;
                raiseError(out, st);
            } else {
                raiseError(out, st);
            }
            return ReadResult::Stopped;
        }
        frame.id          = msg.ID & (msg.MSGTYPE & PCAN_MESSAGE_EXTENDED
                                          ? 0x1FFFFFFFu : 0x7FFu);
        frame.extended    = msg.MSGTYPE & PCAN_MESSAGE_EXTENDED;
        frame.rtr         = msg.MSGTYPE & PCAN_MESSAGE_RTR;
        frame.fd          = msg.MSGTYPE & PCAN_MESSAGE_FD;
        frame.brs         = msg.MSGTYPE & PCAN_MESSAGE_BRS;
        frame.esi         = msg.MSGTYPE & PCAN_MESSAGE_ESI;
        frame.errorFrame  = msg.MSGTYPE & PCAN_MESSAGE_ERRFRAME;
        frame.timestampUs = qint64(ts);
        frame.data        = QByteArray(reinterpret_cast<const char *>(msg.DATA),
                                       int(fdLength(msg.DLC)));
    } else {
        TPCANMsg       msg;
        TPCANTimestamp ts;
        std::memset(&msg, 0, sizeof(msg));
        std::memset(&ts, 0, sizeof(ts));
        const TPCANStatus st = m_api->Read(m_channel, &msg, &ts);
        if (st == PCAN_ERROR_QRCVEMPTY)
            return ReadResult::Empty;
        if (st != PCAN_ERROR_OK) {
            if (st == PCAN_ERROR_INITIALIZE) {   // channel went away
                m_open = false;
                out.openState = 0;
                raiseError(out, st);
            } else {
                raiseError(out, st);
            }
            return ReadResult::Stopped;
        }
        frame.id          = msg.ID & (msg.MSGTYPE & PCAN_MESSAGE_EXTENDED
                                          ? 0x1FFFFFFFu : 0x7FFu);
        frame.extended    = msg.MSGTYPE & PCAN_MESSAGE_EXTENDED;
        frame.rtr         = msg.MSGTYPE & PCAN_MESSAGE_RTR;
        frame.errorFrame  = msg.MSGTYPE & PCAN_MESSAGE_ERRFRAME;
        frame.timestampUs = qint64(ts.micros) + 1000 * qint64(ts.millis) +
                            0x100000000LL * 1000 * qint64(ts.millis_overflow);
        frame.data        = QByteArray(reinterpret_cast<const char *>(msg.DATA),
                                       int(msg.LEN));
    }

    out.frames.append(frame);
    return ReadResult::Frame;
}

// ============================================================================
// Errors
// ============================================================================

// Records an error: lastError()/lastErrorCode()/lastErrorStatus() report it from
// now on, and the message is queued for errorOccurred. It never emits - the
// caller normally holds m_lock, and a signal must not leave the object while a
// handler could still be waiting for that lock. emitAfterUnlock() delivers it.
// Call with m_lock held.
void CanBus::recordError(Emissions &out, ErrorCode code, const QString &text,
                         TPCANStatus status)
{
    m_lastError = text;
    m_lastErrorCode = code;
    m_lastErrorStatus = status;
    out.errors.append(text);
}

// Records a PCAN status as an error. Call with m_lock held.
void CanBus::raiseError(Emissions &out, TPCANStatus status)
{
    recordError(out, codeFromStatus(status), errorText(status), status);
}

// Maps PCAN status bits onto ErrorCode. Order matters: the driver reports
// several bits at once (e.g. QOVERRUN | OVERRUN on an overrun burst).
CanBus::ErrorCode CanBus::codeFromStatus(TPCANStatus status)
{
    if (status == PCAN_ERROR_OK)
        return ErrorCode::NoError;
    if (status & (PCAN_ERROR_QXMTFULL | PCAN_ERROR_XMTFULL))
        return ErrorCode::QueueFull;
    if (status & PCAN_ERROR_INITIALIZE)
        return ErrorCode::NotOpen;              // channel uninitialised / gone
    if (status & PCAN_ERROR_ILLHANDLE)
        return ErrorCode::NotInitialized;       // bad or never-created handle
    if (status & (PCAN_ERROR_ILLPARAMTYPE | PCAN_ERROR_ILLPARAMVAL | PCAN_ERROR_ILLDATA))
        return ErrorCode::BadParameter;         // driver rejected the call arguments
    if (status & (PCAN_ERROR_ILLOPERATION | PCAN_ERROR_ILLMODE))
        return ErrorCode::Other;                // operation not allowed right now
    return ErrorCode::Hardware;                 // bus errors, driver faults, unknown
}

QString CanBus::errorText(TPCANStatus status) const
{
    if (m_api && m_api->GetErrorText) {
        char buf[256] = {0};
        // 0x09 = English
        if (m_api->GetErrorText(status, 0x09, buf) == PCAN_ERROR_OK && buf[0])
            return QStringLiteral("PCAN error 0x%1: %2")
                .arg(status, 0, 16).arg(QString::fromLatin1(buf));
    }
    // Fallback table (no GetErrorText available).
    struct { TPCANStatus s; const char *t; } known[] = {
        { PCAN_ERROR_OK,           "no error" },
        { PCAN_ERROR_XMTFULL,      "transmit buffer of CAN controller full" },
        { PCAN_ERROR_OVERRUN,      "CAN controller read too late" },
        { PCAN_ERROR_BUSLIGHT,     "bus error: error-passive warning limit" },
        { PCAN_ERROR_BUSHEAVY,     "bus error: error-passive limit" },
        { PCAN_ERROR_BUSPASSIVE,   "bus error: controller error passive" },
        { PCAN_ERROR_BUSOFF,       "bus error: bus-off" },
        { PCAN_ERROR_QRCVEMPTY,    "receive queue empty" },
        { PCAN_ERROR_QOVERRUN,     "receive queue overrun" },
        { PCAN_ERROR_QXMTFULL,     "transmit queue full" },
        { PCAN_ERROR_REGTEST,      "no hardware found" },
        { PCAN_ERROR_NODRIVER,     "driver not loaded" },
        { PCAN_ERROR_HWINUSE,      "hardware already in use" },
        { PCAN_ERROR_NETINUSE,     "network already in use" },
        { PCAN_ERROR_ILLHW,        "invalid hardware handle" },
        { PCAN_ERROR_RESOURCE,     "resource cannot be created" },
        { PCAN_ERROR_ILLPARAMTYPE, "invalid parameter" },
        { PCAN_ERROR_ILLPARAMVAL,  "invalid parameter value" },
        { PCAN_ERROR_UNKNOWN,      "unknown error" },
        { PCAN_ERROR_ILLDATA,      "invalid data / operation" },
        { PCAN_ERROR_ILLMODE,      "driver object state wrong for operation" },
        { PCAN_ERROR_CAUTION,      "operation done with irregularities" },
        { PCAN_ERROR_INITIALIZE,   "channel not initialized" },
        { PCAN_ERROR_ILLOPERATION, "invalid operation" },
    };
    for (const auto &e : known) {
        if (e.s == status)
            return QStringLiteral("PCAN error 0x%1: %2")
                .arg(status, 0, 16).arg(QLatin1String(e.t));
    }
    return QStringLiteral("PCAN error 0x%1").arg(status, 0, 16);
}

// ============================================================================
// Persisted rate settings (QSettings, INI file "candll.ini")
// ============================================================================

static QString &settingsPathOverride()
{
    static QString path;    // set via CanBus::setSettingsFilePath()
    return path;
}

QString CanBus::settingsFilePath()
{
    const QString &p = settingsPathOverride();
    if (!p.isEmpty())
        return p;
    QString dir = QCoreApplication::applicationDirPath();
    if (dir.isEmpty())
        dir = QDir::currentPath();
    return QDir(dir).filePath(QStringLiteral("candll.ini"));
}

void CanBus::setSettingsFilePath(const QString &path)
{
    settingsPathOverride() = path;
}

// QSettings is constructed locally in each function: works identically on
// Qt 5.14 and Qt 6 without relying on copy/move semantics of QSettings.
void CanBus::saveCanBitrateKbps(int kbps)
{
    QSettings s(settingsFilePath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("Can/bitrateKbps"), kbps);
}

int CanBus::canBitrateKbps()
{
    QSettings s(settingsFilePath(), QSettings::IniFormat);
    return s.value(QStringLiteral("Can/bitrateKbps"), 500).toInt();
}

void CanBus::saveFdBitrateKbps(int nominalKbps, int dataKbps)
{
    QSettings s(settingsFilePath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("CanFD/nominalKbps"), nominalKbps);
    s.setValue(QStringLiteral("CanFD/dataKbps"), dataKbps);
}

int CanBus::fdNominalBitrateKbps()
{
    QSettings s(settingsFilePath(), QSettings::IniFormat);
    return s.value(QStringLiteral("CanFD/nominalKbps"), 500).toInt();
}

int CanBus::fdDataBitrateKbps()
{
    QSettings s(settingsFilePath(), QSettings::IniFormat);
    return s.value(QStringLiteral("CanFD/dataKbps"), 2000).toInt();
}

void CanBus::saveChannelNameConfig(const QString &channel)
{
    QSettings s(settingsFilePath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("Channel/name"), channel);
}

QString CanBus::channelNameConfig()
{
    QSettings s(settingsFilePath(), QSettings::IniFormat);
    return s.value(QStringLiteral("Channel/name"),
                   QStringLiteral("PCAN_USBBUS1")).toString();
}

bool CanBus::openConfigured(const QString &channel)
{
    const QString ch = channel.isEmpty() ? channelNameConfig() : channel;
    return open(ch, canBitrateKbps());
}

bool CanBus::openFdConfigured(const QString &channel)
{
    const QString ch = channel.isEmpty() ? channelNameConfig() : channel;
    return openFd(ch, fdNominalBitrateKbps(), fdDataBitrateKbps());
}

bool CanBus::saveConfiguration()
{
    Emissions out;
    bool saved = false;
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        if (m_channelName.isEmpty()) {
            recordError(out, ErrorCode::NotOpen,
                        QStringLiteral("saveConfiguration: no channel opened yet"));
        } else {
            saveChannelNameConfig(m_channelName);
            if (m_fd)
                saveFdBitrateKbps(m_nominalKbps, m_dataKbps);
            else
                saveCanBitrateKbps(m_nominalKbps);
            clearError();
            saved = true;
        }
    }
    // Nothing was saved: the recorded error is still handed over, but only now,
    // once m_lock is back off the table.
    emitAfterUnlock(out);
    return saved;
}

// ============================================================================
// Frame handler + state getters
// ============================================================================

void CanBus::setFrameHandler(std::function<void(const CanBusFrame &)> handler)
{
    {
        std::lock_guard<std::recursive_mutex> g(m_lock);
        m_frameHandler = std::move(handler);
    }
    // A pending wake makes the engine deliver what it has already read without
    // waiting for the next period, so the new handler sees the stream at once.
    wakeEngine();
}

// Every getter below takes m_lock, because the engine thread writes the same
// members. They never emit, so holding the lock for the whole body is safe.

int CanBus::rxBudget() const
{
    std::lock_guard<std::recursive_mutex> g(m_lock);
    return m_rxBudget;
}

bool CanBus::listenOnly() const
{
    std::lock_guard<std::recursive_mutex> g(m_lock);
    return m_listenOnly;
}

bool CanBus::autoReset() const
{
    std::lock_guard<std::recursive_mutex> g(m_lock);
    return m_autoReset;
}

bool CanBus::isOpen() const
{
    std::lock_guard<std::recursive_mutex> g(m_lock);
    return m_open;
}

bool CanBus::isFd() const
{
    std::lock_guard<std::recursive_mutex> g(m_lock);
    return m_fd;
}

QString CanBus::channelName() const
{
    std::lock_guard<std::recursive_mutex> g(m_lock);
    return m_channelName;
}

QString CanBus::lastError() const
{
    std::lock_guard<std::recursive_mutex> g(m_lock);
    return m_lastError;
}

CanBus::ErrorCode CanBus::lastErrorCode() const
{
    std::lock_guard<std::recursive_mutex> g(m_lock);
    return m_lastErrorCode;
}

TPCANStatus CanBus::lastErrorStatus() const
{
    std::lock_guard<std::recursive_mutex> g(m_lock);
    return m_lastErrorStatus;
}

int CanBus::pending() const
{
    std::lock_guard<std::recursive_mutex> g(m_lock);
    return int(m_txQueue.size());
}
