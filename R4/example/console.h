// Minimal stdin reader so the example can stay responsive to received
// frames while waiting for typed commands. A worker thread blocks on
// stdin and hands each line over to the main thread via signals.
//
// Shutdown: a blocking stdin read cannot be cancelled, and QThread has no
// detach() (that is std::thread), while ~QThread() on a still running
// thread calls qFatal(). This class therefore has no teardown path - it is
// created with new() and deliberately left alive until the process ends
// (see main.cpp). requestStop() only keeps it quiet: the worker notices
// the flag after its next line, or at EOF.
//
// Qt 5.14 .. 6.x compatible (QTextStream::readLine(), plain connect syntax).

#ifndef CANDLL_EXAMPLE_CONSOLE_H
#define CANDLL_EXAMPLE_CONSOLE_H

#include <atomic>

#include <QObject>
#include <QThread>
#include <QString>

class ConsoleReader : public QObject
{
    Q_OBJECT
public:
    explicit ConsoleReader(QObject *parent = nullptr);

    void start();

    /** Quiets the reader; the worker sees it after its next line or at EOF. */
    void requestStop() { m_stop = true; }

signals:
    void promptReady();     // main thread prints the "can> " prompt
    void lineEntered(const QString &line);
    void endOfInput();      // stdin closed (Ctrl+Z / pipe ended)

private slots:
    void handlePrompt();

private:
    void run();                             // executes in the worker thread

    QThread *m_thread;                      // child object, freed by the OS
    std::atomic<bool> m_stop{ false };      // main -> worker: stay quiet
};

#endif // CANDLL_EXAMPLE_CONSOLE_H
