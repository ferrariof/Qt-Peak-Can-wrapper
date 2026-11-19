#ifndef CANBUS_H
#define CANBUS_H

#include <QObject>
#include <QLibrary>
#include <QQueue>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "candll_global.h"
#include "pcanbasic_api.h"

/**
 * One received CAN / CAN FD / error-frame.
 */
struct CanBusFrame
{
    quint32    id          = 0;    ///< 11- or 29-bit identifier
    QByteArray data;               ///< payload (max 8, or 64 bytes for CAN FD)
    bool       extended    = false;///< extended (29-bit) frame
    bool       rtr         = false;///< remote transmission request
    bool       fd          = false;///< CAN FD frame
    bool       brs         = false;///< FD bit-rate switch
    bool       esi         = false;///< FD error state indicator
    bool       errorFrame  = false;///< CAN error frame
    qint64     timestampUs = 0;    ///< hardware reception timestamp, microseconds

    bool isValidId() const
    {
        return extended ? (id <= 0x1FFFFFFFu) : (id <= 0x7FFu);
    }
};
Q_DECLARE_METATYPE(CanBusFrame)

/**
 * CanBus - CAN / CAN FD interface for PEAK-System PCAN hardware.
 *
 * The class drives itself: one internal engine thread (a plain std::thread,
 * never a Qt one) drains the PCAN receive queue every rxIntervalMs() and pumps
 * the TX FIFO every txIntervalMs(). Every received message is emitted as
 * frameReceived(). rxSlot() and txSlot() stay public, so a host can also
 * trigger one drain / one pump by hand.
 *
 * Usage:
 * @code
 * CanBus can;
 * connect(&can, &CanBus::frameReceived, [](const CanBusFrame &f) { ... });
 * can.open("PCAN_USBBUS1", 500);          // Classic CAN, 500 kbit/s
 * can.sendFrame(0x123, QByteArray::fromHex("DEADBEEF"));
 * // - or -
 * CanBus canFd;
 * canFd.openFd("PCAN_USBBUS1", 500, 2000); // CAN FD 500k nom / 2M data
 * canFd.sendFdFrame(0x123, payload, false, true);
 * @endcode
 *
 * Threading and delivery: every method is callable from any thread - the ones
 * that touch guarded state take the same internal recursive mutex, while the
 * pacing and status accessors are atomic and take none. Frames are delivered
 * **on the engine thread**, so a cross-thread connection must be
 * Qt::DirectConnection: a queued connection can never be delivered across the
 * DLL boundary of a static Qt build, where the library and the host application
 * each own a copy of QtCore. Such a direct slot therefore runs on the engine
 * thread, between two engine ticks and with no lock held: it may call back into
 * any method, close() included, because the engine takes the mutex again only
 * once the batch has been delivered and simply waits for the slot to return.
 * The atomic accessors rxIntervalMs(), txIntervalMs() and isEngineRunning() need
 * no lock at all. Two things remain forbidden: blocking inside the slot on work
 * another thread has to finish - the engine is blocked in the slot, and so is
 * whatever waits for the mutex behind it - and destroying the CanBus object from
 * a slot or from the frame handler. setFrameHandler() is the hook to prefer when
 * a connect() is inconvenient; the same rules apply to it.
 */
class CANDLL_EXPORT CanBus : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool  open        READ isOpen           WRITE setOpen   NOTIFY openChanged)
    Q_PROPERTY(bool  listenOnly  READ listenOnly       WRITE setListenOnly)
    Q_PROPERTY(bool  autoReset   READ autoReset        WRITE setAutoReset)
    Q_PROPERTY(int   rxIntervalMs READ rxIntervalMs    WRITE setRxIntervalMs)
    Q_PROPERTY(int   txIntervalMs READ txIntervalMs    WRITE setTxIntervalMs)
    Q_PROPERTY(int   txPending   READ pending          NOTIFY txQueueChanged)
    Q_PROPERTY(QString channelName READ channelName    NOTIFY openChanged)

public:
    /** Machine-readable classification of lastError() (branching in C++/QML). */
    enum class ErrorCode {
        NoError = 0,      ///< no failure recorded since the channel was opened
        Api,              ///< PCANBasic.dll could not be loaded
        BadChannel,       ///< unknown / unparsable channel name
        BadParameter,     ///< id / payload invalid for the requested frame type
        NotOpen,          ///< operation requested while the channel was closed
        NotInitialized,   ///< CAN_Initialize()/CAN_InitializeFD() failed
        AlreadyOpen,      ///< open() on an already open channel
        QueueFull,        ///< TX queue rejected a frame (queue full)
        Hardware,         ///< a PCAN call failed (see lastError() for the bits)
        Other             ///< anything not covered above
    };
    Q_ENUM(ErrorCode)

    explicit CanBus(QObject *parent = nullptr);
    ~CanBus() override;

    // --- configuration (set before open()) -----------------------------------

    /** Receive polling period in milliseconds (default 10 ms). */
    void          setRxIntervalMs(int ms);
    int           rxIntervalMs() const { return m_rxIntervalMs.load(); }

    /** Current adaptive RX drain budget (frames read per engine RX tick). */
    int           rxBudget() const;

    /** Listen-only mode: never acknowledge or disturb the bus (default off). */
    void          setListenOnly(bool on);
    bool          listenOnly() const;

    /** Automatic re-initialization after bus-off (default off). */
    void          setAutoReset(bool on);
    bool          autoReset() const;

    // --- delivery (engine thread) --------------------------------------------

    /** true while the internal engine thread is running (false before the
        first successful open() and after the destructor has stopped it). */
    bool isEngineRunning() const { return m_engineRunning.load(); }

    /**
     * Non-Qt delivery hook, called on the engine thread for every received
     * frame, after frameReceived(). Use it instead of a connection when the
     * consumer is built without Qt or when queued connections are unavailable
     * (static Qt).
     *
     * It runs with no CanBus lock held, so it may call back into the object,
     * close() included; it may be called concurrently with any other method, so
     * it needs to be thread-safe itself. setFrameHandler(nullptr) disables the
     * delivery again.
     */
    void setFrameHandler(std::function<void(const CanBusFrame &)> handler);

    // --- connection -----------------------------------------------------------

    /**
     * Opens a Classic CAN channel.
     * @param channel     "PCAN_USBBUS1".."16", "PCAN_PCIBUS1".."16",
     *                    "PCAN_LANBUS1".."16", "usb1", "pci2", ...
     *                    or a raw numeric handle ("0x51" or decimal).
     * @param bitrateKbps Nominal bit rate in kbit/s (5, 10, 20, 33, 47, 50,
     *                    83, 95, 100, 125, 250, 500, 800, 1000).
     */
    Q_INVOKABLE bool open(const QString &channel = QStringLiteral("PCAN_USBBUS1"),
                          int bitrateKbps = 500);

    /**
     * Opens a CAN FD channel. The bit timing registers for the nominal and
     * the data phase are computed automatically from the requested rates.
     */
    Q_INVOKABLE bool openFd(const QString &channel = QStringLiteral("PCAN_USBBUS1"),
                            int nominalKbps = 500, int dataKbps = 2000);

    /**
     * Opens a CAN FD channel with a raw PEAK bit-rate string, e.g.
     * "f_clock=80000000,nom_brp=10,nom_tseg1=13,nom_tseg2=2,nom_sjw=1,
     *  data_brp=4,data_tseg1=7,data_tseg2=2,data_sjw=1".
     */
    bool openFdRaw(const QString &channel, const QString &fdBitrateString);

    /** Closes the channel; the engine thread keeps idling until the object dies. */
    Q_INVOKABLE void close();

    /** Sets open()/close() according to @a on. */
    void setOpen(bool on) { if (on) open(); else close(); }

    // These getters read state guarded by the internal mutex, so they take it.
    bool    isOpen() const;
    bool    isFd() const;
    QString channelName() const;
    QString lastError() const;

    /** Classification of the last error (NoError until the first one). */
    ErrorCode lastErrorCode() const;

    /** Raw PCANBasic status bits of the last error, 0 when the error was not
        reported by the driver (e.g. argument validation). */
    TPCANStatus lastErrorStatus() const;

    // --- transmission (FIFO queue, no bus congestion) -------------------------
    //
    // sendFrame()/sendFdFrame() push the command into an internal FIFO queue
    // and return immediately. The engine thread pops up to txIntervalBurst()
    // commands every txIntervalMs (default 25 ms) and writes them to the bus.
    // Pass immediate=true to bypass the queue and write synchronously.

    /**
     * Queues (or with @a immediate, sends) a Classic CAN packet (<= 8 bytes).
     * @return true if queued/sent, false + lastError() on invalid arguments
     *         or closed channel.
     */
    Q_INVOKABLE bool sendFrame(quint32 id, const QByteArray &data,
                               bool extended = false, bool rtr = false,
                               bool immediate = false);

    /**
     * Queues (or with @a immediate, sends) a CAN FD frame (<= 64 bytes,
     * channel must be FD-open).
     * @param bitRateSwitch  BRS flag: transmit payload at the data phase rate.
     */
    Q_INVOKABLE bool sendFdFrame(quint32 id, const QByteArray &data,
                                 bool extended = false, bool bitRateSwitch = true,
                                 bool immediate = false);

    /** Commands still waiting in the TX FIFO. */
    int  pending() const;

    /** Discards all queued TX commands. */
    Q_INVOKABLE void clearTxQueue();

    /** TX pump period in milliseconds (default 25 ms, min 1 ms). */
    void setTxIntervalMs(int ms);
    int  txIntervalMs() const { return m_txIntervalMs.load(); }

    /** Commands popped per engine TX tick (default 1 = max pacing). */
    void setTxBurstPerTick(int n);
    int  txIntervalBurst() const { return m_txBurstPerTick.load(); }

    /** Hardware receive filter for a closed ID range (all IDs by default). */
    bool filterMessages(quint32 fromId, quint32 toId, bool extended);

    /** Resets RX/TX queues of the channel. */
    Q_INVOKABLE bool resetQueues();

    // --- helpers --------------------------------------------------------------

    /** Maps "PCAN_USBBUS1", "usb1", "pci3", "0x51"... to a PCAN handle. */
    static quint16 channelHandle(const QString &name, bool *ok = nullptr);

    /**
     * Build stamp of THIS library binary ("Mmm d yyyy HH:mm:ss", MSVC/GCC
     * __DATE__/__TIME__). Print it when in doubt whether the loaded
     * CanDLL.dll is the one you just built (stale DLL copies in bin/,
     * build dirs or System32 shadow the fresh one).
     */
    static QString buildStamp();

    /**
     * Builds a PEAK CAN FD bit-rate string for the given nominal/data rates
     * (kbit/s). Returns an empty string if no valid timing is found.
     */
    static QString fdBitrateString(int nominalKbps, int dataKbps,
                                   quint32 clockHz = 80000000);

    // --- persisted rate settings (QSettings, candll.ini) --------------------

    /**
     * Settings file (INI). Default: candll.ini next to the application
     * executable; can be overridden, e.g. to a writable per-user location.
     */
    static QString settingsFilePath();
    static void    setSettingsFilePath(const QString &path);

    /** Persist / load the Classic CAN bit rate (key Can/bitrateKbps). */
    static void saveCanBitrateKbps(int kbps);
    static int  canBitrateKbps();                    ///< default 500

    /** Persist / load the CAN FD rates (keys CanFD/nominalKbps, CanFD/dataKbps). */
    static void saveFdBitrateKbps(int nominalKbps, int dataKbps);
    static int  fdNominalBitrateKbps();              ///< default 500
    static int  fdDataBitrateKbps();                 ///< default 2000

    /** Persist / load the default channel (key Channel/name). */
    static void   saveChannelNameConfig(const QString &channel);
    static QString channelNameConfig();              ///< default "PCAN_USBBUS1"

    /**
     * open() using the persisted configuration. @a channel empty -> use the
     * configured Channel/name and Can/bitrateKbps.
     */
    Q_INVOKABLE bool openConfigured(const QString &channel = QString());

    /** openFd() using the persisted CanFD/nominalKbps and CanFD/dataKbps. */
    Q_INVOKABLE bool openFdConfigured(const QString &channel = QString());

    /**
     * Persists the channel and the bit rate(s) currently active on this
     * instance (Classic rate after open(), FD rates after openFd()).
     */
    Q_INVOKABLE bool saveConfiguration();

    // All signals below are emitted on the engine thread (or on the thread that
    // called the public method that collected them), always AFTER the internal
    // mutex was released - never while it is held, so a slot may call back into
    // any CanBus method without deadlocking. One batch is delivered in this order:
    // errorOccurred, then openChanged, then frameReceived (arrival order, each
    // immediately followed by the frame handler), then frameSent, then
    // txQueueChanged. Emissions are delivered by whichever thread produced them,
    // so a tick on the engine thread and a sendFrame() on the caller's thread can
    // deliver concurrently: a Qt::DirectConnection slot must therefore be
    // thread-safe itself. A cross-thread connection must use Qt::DirectConnection.
signals:
    /** Emitted for every received frame, on the engine thread. */
    void frameReceived(const CanBusFrame &frame);

    /** Emitted on API/bus errors (also sets lastError()). */
    void errorOccurred(const QString &errorText);

    /** Emitted when the channel was opened (true) or closed (false). */
    void openChanged(bool open);

    /** Emitted by the TX pump for every frame actually written to the bus. */
    void frameSent(quint32 id);

    /** Emitted when the TX FIFO gains its first / loses its last command. */
    void txQueueChanged();

public slots:
    /**
     * The receive slot: drains the PCAN receive queue once, with the same
     * adaptive budget the engine uses. The engine calls drainRxLocked()
     * directly; this slot does the same work and exists for manual and
     * testing use. Emits frameReceived() per frame.
     */
    void rxSlot();

    /**
     * The transmit slot: pops up to txIntervalBurst() commands from the TX
     * FIFO and writes them to the bus. The engine calls pumpTxLocked()
     * directly; this slot does the same work and exists for manual and
     * testing use.
     */
    void txSlot();

private:
    struct Api;             // resolved PCANBasic.dll entry points
    struct TxItem {
        quint32    id     = 0;
        QByteArray data;
        bool       extended = false;
        bool       rtr      = false;
        bool       fd       = false;
        bool       brs      = true;
    };

    static Api *resolveApi();
    static Api  makeApi(QLibrary &lib);           // resolve all entry points

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
    void engineLoop();                                     // body of m_worker
    void startEngine();                                    // call with m_lock held
    void stopEngine();                                     // destructor only
    void wakeEngine();                                     // m_wakeMtx only
    void drainRxLocked(Emissions &out);                    // requires m_lock
    void pumpTxLocked(Emissions &out);                     // requires m_lock
    ReadResult readOneLocked(Emissions &out);              // requires m_lock
    void emitAfterUnlock(const Emissions &out);            // requires: no lock held
    void recordError(Emissions &out, ErrorCode code, const QString &text,
                     TPCANStatus status = 0);
    void raiseError(Emissions &out, TPCANStatus status);

    bool openChannel(const QString &channel, Emissions &out); // common open() bookkeeping
    bool initClassic(const QString &channel, int bitrateKbps, Emissions &out);
    bool initFd(const QString &channel, const QString &fdBitrateString, Emissions &out);
    void applyOptions();
    bool enqueue(TxItem item, Emissions &out);
    TPCANStatus writeItem(const TxItem &item);     // synchronous CAN_Write/FD
    bool validateForSend(const TxItem &item, Emissions &out);
    // Call with m_lock held.
    void clearError() { m_lastError.clear(); m_lastErrorCode = ErrorCode::NoError;
                        m_lastErrorStatus = 0; }
    static ErrorCode codeFromStatus(TPCANStatus status);
    QString errorText(TPCANStatus status) const;

    static quint8  fdDlc(quint32 lenBytes);        // length -> FD DLC 0..15
    static quint32 fdLength(quint8 dlc);           // FD DLC 0..15 -> length

    // engine thread + synchronisation. m_lock guards the bus state listed below
    // it; m_wakeMtx guards m_wakePending; the two atomics are published by
    // startEngine()/stopEngine() without it; m_worker belongs to those two
    // (startEngine() under m_lock, stopEngine() in the destructor).
    std::thread             m_worker;
    mutable std::recursive_mutex m_lock;          // guards the bus state below
    std::condition_variable m_wake;
    std::mutex              m_wakeMtx;            // guards m_wakePending
    bool                    m_wakePending = false;
    std::atomic<bool>       m_stopRequested{false};
    std::atomic<bool>       m_engineRunning{false};
    std::function<void(const CanBusFrame &)> m_frameHandler; // guarded by m_lock

    Api           *m_api            = nullptr;
    bool           m_open           = false;
    bool           m_fd             = false;
    TPCANHandle    m_channel        = PCAN_NONEBUS;
    QString        m_channelName;
    QString        m_lastError;
    ErrorCode      m_lastErrorCode = ErrorCode::NoError;
    TPCANStatus    m_lastErrorStatus = 0;
    std::atomic<int> m_rxIntervalMs{10};
    int            m_rxBudget       = 200;       // adaptive frames per RX tick
    bool           m_listenOnly     = false;
    bool           m_autoReset      = false;

    QQueue<TxItem> m_txQueue;                      // FIFO of TX commands
    std::atomic<int> m_txIntervalMs{25};           // TX pump period
    std::atomic<int> m_txBurstPerTick{1};          // commands per TX tick

    int            m_nominalKbps    = 500;         // last used / for QSettings
    int            m_dataKbps       = 2000;        // FD data phase, ditto
};

#endif // CANBUS_H
