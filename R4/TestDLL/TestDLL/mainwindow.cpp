#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QDateTime>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);


    QString channel  = CanBus::channelNameConfig();   // candll.ini, else PCAN_USBBUS1
    int     nomKbps  = CanBus::fdNominalBitrateKbps();
    int     dataKbps = CanBus::fdDataBitrateKbps();
    bool    fd=false;

    //**START CAN ADAPTER**///
    qInfo() << "CanDLL build:" << CanBus::buildStamp();
    QObject::connect(&can, &CanBus::frameReceived, this, &MainWindow::CanMessageRx);
    QObject::connect(&can, &CanBus::errorOccurred, [](const QString &e) {
        qWarning() << "error:" << e;
    });
    const bool ok = fd ? can.openFd(channel, nomKbps, dataKbps)
                       : can.open(channel, 250);
    if (ok) ui->textBrowser->insertPlainText("Can Adapter Connected");
    else
    ui->textBrowser->insertPlainText("Can Adapter Failed");

}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::CanMessageRx(const CanBusFrame &frame)
{
    qDebug().noquote() << QStringLiteral("[%1] %2 id=0x%3 len=%4 data=%5")
    .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
        .arg(frame.fd ? QStringLiteral("FD") : QStringLiteral("App RX"))
        .arg(frame.id, frame.extended ? 8 : 3, 16, QLatin1Char('0'))
        .arg(frame.data.size())
        .arg(QString::fromLatin1(frame.data.toHex(' ').toUpper()));

    ui->textBrowser_RX->insertPlainText(QString::number(frame.data.at(0)));
}
void MainWindow::on_pushButton_clicked()
{
    static uint8_t txCounter=0;
    QByteArray p(8, '\0');
    p[0] = char(txCounter++ & 0xFF);
    can.sendFrame(0x500, p);
}

