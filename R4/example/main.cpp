// CanTest - console smoke test for the CanDLL Qt CAN bus library.
//
//   CanTest                          open PCAN_USBBUS1 @ 500 kbit/s
//   CanTest PCAN_USBBUS2             another channel
//   CanTest --fd [nom_kbps data_kbps]
//   CanTest --settings               open using candll.ini (QSettings)
//   CanTest --remember               ... and store this setup back
//   CanTest --tx                     also send a 1 Hz heartbeat on 0x500
//
// Interactive console: at the "can>" prompt type  <id> <hex bytes>
// e.g.  0x1AB DEAD BEEF   ("help" for more, "q" to quit). Received
// frames are printed as they arrive.

#include <QCoreApplication>
#include <QDebug>
#include <QTimer>
#include <QDateTime>

#include "canbus.h"
#include "console.h"

static void printFrame(const CanBusFrame &f)
{
    qDebug().noquote() << QStringLiteral("[%1] %2 id=0x%3 len=%4 data=%5")
                              .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
                              .arg(f.fd ? QStringLiteral("FD") : QStringLiteral("RX"))
                              .arg(f.id, f.extended ? 8 : 3, 16, QLatin1Char('0'))
                              .arg(f.data.size())
                              .arg(QString::fromLatin1(f.data.toHex(' ').toUpper()));
}

static void printHelp()
{
    qInfo().noquote() << QStringLiteral(
        "  <id> <hex bytes...>   send, e.g.  0x1AB DEAD BEEF   (id>0x7FF => 29-bit)\n"
        "  q | quit | exit       leave the example\n"
        "  help | ?              this text\n"
        "Everything received on the bus is printed as it arrives.");
}

// Returns false when the example should quit.
static bool handleConsoleLine(CanBus &can, bool fd, const QString &raw)
{
    const QString line = raw.trimmed();
    if (line.isEmpty())
        return true;
    if (line.compare(QLatin1String("q"), Qt::CaseInsensitive) == 0
        || line.compare(QLatin1String("quit"), Qt::CaseInsensitive) == 0
        || line.compare(QLatin1String("exit"), Qt::CaseInsensitive) == 0)
        return false;
    if (line.compare(QLatin1String("help"), Qt::CaseInsensitive) == 0
        || line == QLatin1String("?")) {
        printHelp();
        return true;
    }

    const QStringList tok = line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    bool okId = false;
    QString idTxt = tok.first().toUpper();
    if (idTxt.startsWith(QLatin1String("0X")))
        idTxt = idTxt.mid(2);
    const quint32 id = idTxt.toUInt(&okId, 16);
    if (!okId || id > 0x1FFFFFFu) {
        qWarning().noquote() << QStringLiteral("bad id '%1' (hex, <= 0x1FFFFFF)").arg(tok.first());
        return true;
    }

    QString hex;
    for (int i = 1; i < tok.size(); ++i)
        hex += tok.at(i).toUpper();
    if (hex.startsWith(QLatin1String("0X")))
        hex = hex.mid(2);
    bool hexOk = true;
    for (const QChar &c : hex) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9'))
              || (c >= QLatin1Char('A') && c <= QLatin1Char('F')))) {
            hexOk = false;
            break;
        }
    }
    if (!hexOk || (hex.size() % 2) != 0) {
        qWarning().noquote() << QStringLiteral("data must be hex bytes, e.g. DEAD BEEF");
        return true;
    }
    const QByteArray data = QByteArray::fromHex(hex.toLatin1());
    const int maxLen = fd ? 64 : 8;
    if (data.size() > maxLen) {
        qWarning().noquote() << QStringLiteral("payload too long (%1 > %2 bytes)")
                                    .arg(data.size()).arg(maxLen);
        return true;
    }

    const bool ext = id > 0x7FFu;
    const bool sent = fd ? can.sendFdFrame(id, data, ext, true, true)
                         : can.sendFrame(id, data, ext, false, true);
    if (sent) {
        CanBusFrame echo;
        echo.id = id; echo.data = data; echo.extended = ext; echo.fd = fd;
        printFrame(echo);            // TX echo; replies arrive via frameReceived
    }
    return true;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QStringList args = QCoreApplication::arguments();
    QString channel  = CanBus::channelNameConfig();   // candll.ini, else PCAN_USBBUS1
    bool    fd       = false;
    bool    useSettings = false;
    bool    remember = false;
    bool    periodicTx = false;
    bool    haveChannel = false;
    int     nomKbps  = CanBus::fdNominalBitrateKbps();
    int     dataKbps = CanBus::fdDataBitrateKbps();

    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--fd")) {
            fd = true;
            if (i + 2 < args.size()) {
                nomKbps  = args[++i].toInt();
                dataKbps = args[++i].toInt();
            }
        } else if (args[i] == QLatin1String("--settings")) {
            useSettings = true;
        } else if (args[i] == QLatin1String("--remember")) {
            remember = true;
        } else if (args[i] == QLatin1String("--tx")) {
            periodicTx = true;
        } else {
            channel     = args[i];
            haveChannel = true;
        }
    }

    CanBus can;
    qInfo() << "CanDLL build:" << CanBus::buildStamp();
    // CanBus emits on its own engine thread, and the DLL holds its own QtCore
    // copy, so delivery has to be direct: a queued connection cannot cross the
    // boundary (see README, "Two Qt runtimes in one process").
    // Both connects below are direct already, because no context/receiver
    // object is passed: qobject.h forwards the 3-argument forms to
    // connect(sender, signal, sender, slot, Qt::DirectConnection). Adding a
    // context object would switch the default to Qt::AutoConnection, which
    // resolves to queued here and drops the frames.
    QObject::connect(&can, &CanBus::frameReceived, &printFrame);
    QObject::connect(&can, &CanBus::errorOccurred, [](const QString &e) {
        qWarning() << "error:" << e;
    });

    // --settings: take the rates from candll.ini; otherwise command line.
    if (useSettings) {
        if (!haveChannel)
            channel.clear();                  // use Channel/name from settings
        const bool ok = fd ? can.openFdConfigured(channel)
                           : can.openConfigured(channel);
        if (!ok) {
            qCritical().noquote() << QStringLiteral("openConfigured failed: %1")
                                         .arg(can.lastError());
            return 1;
        }
    } else {
        const bool ok = fd ? can.openFd(channel, nomKbps, dataKbps)
                           : can.open(channel, 250);
        if (!ok) {
            qCritical().noquote() << QStringLiteral("open(%1) failed: %2")
                                         .arg(channel, can.lastError());
            return 1;
        }
    }

    if (remember) {
        can.saveConfiguration();   // channel + active classic/FD rates -> ini
        qInfo() << "configuration saved to" << CanBus::settingsFilePath();
    }

    qInfo().noquote() << QStringLiteral("opened %1 (%2)").arg(
        can.channelName(), fd ? QStringLiteral("CAN FD %1/%2 kbit/s")
                                    .arg(nomKbps).arg(dataKbps)
                              : QStringLiteral("Classic CAN 500 kbit/s"));

    // Optional 1 Hz heartbeat on 0x500 (--tx); console is the main TX path.
    int txCounter = 0;
    QTimer txTimer;
    if (periodicTx) {
        QObject::connect(&txTimer, &QTimer::timeout, [&]() {
            QByteArray p(8, '\0');
            p[0] = char(txCounter & 0xFF);
            const bool sent = fd ? can.sendFdFrame(0x500, p, false, true)
                                 : can.sendFrame(0x500, p);
            if (sent)
                ++txCounter;
        });
        txTimer.start(1000);
    }

    // ---- Interactive console ------------------------------------------------
    // Type "0x1AB DEAD BEEF" to send; bus replies print via frameReceived.
    // Allocated with new and never deleted: its worker blocks in stdin and
    // cannot be woken, and destroying the reader while that thread may still
    // emit would be a use-after-free (QThread has no detach()). The process
    // is going away anyway - see the note in console.h.
    ConsoleReader *console = new ConsoleReader;
    QObject::connect(&app, &QCoreApplication::aboutToQuit, console,
                     [console]() { console->requestStop(); });

    QTimer idleTimer;                 // quit after 60 s without any input
    idleTimer.setSingleShot(true);
    QObject::connect(&idleTimer, &QTimer::timeout, [&app]() { app.quit(); });
    // Context = console (lives on main thread): lines are processed on
    // the main thread, where CanBus lives. Queued connection either way.
    QObject::connect(console, &ConsoleReader::lineEntered, console,
                     [&can, fd, &app, &idleTimer](const QString &line) {
                         idleTimer.start(60000);
                         if (!handleConsoleLine(can, fd, line))
                             app.quit();
                     });
    QObject::connect(console, &ConsoleReader::endOfInput, console,
                     [&app]() { app.quit(); });
    console->start();
    idleTimer.start(60000);

    const int rc = app.exec();
    can.close();
    return rc;
}
