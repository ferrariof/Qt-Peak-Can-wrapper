#include "console.h"

#include <cstdio>

#include <QTextStream>

ConsoleReader::ConsoleReader(QObject *parent)
    : QObject(parent),
      m_thread(new QThread(this))
{
    // readLine() is blocking, so do it on its own thread. No receiver
    // context on purpose: the lambda then runs IN the worker thread.
    connect(m_thread, &QThread::started, [this]() { run(); });
    // Auto connection: emitted from the worker, received on the main thread,
    // so the prompt is always printed from the main thread.
    connect(this, &ConsoleReader::promptReady, this, &ConsoleReader::handlePrompt);
}

void ConsoleReader::start()
{
    m_thread->start();
}

void ConsoleReader::handlePrompt()
{
    if (m_stop)
        return;
    // fputs keeps this identical on Qt 5.14 .. 6.x (no stream enum churn).
    std::fputs("can> ", stdout);
    std::fflush(stdout);
}

void ConsoleReader::run()
{
    // Stack object on purpose: this is the only thread touching stdin, the
    // stream dies with the loop, so shutdown has nothing to clean up.
    QTextStream in(stdin);

    emit promptReady();
    while (!m_stop) {
        const QString line = in.readLine();
        if (line.isNull()) {          // EOF (Ctrl+Z on Windows)
            emit endOfInput();
            break;
        }
        emit lineEntered(line);
        emit promptReady();
    }
}
