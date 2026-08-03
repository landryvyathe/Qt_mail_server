#include "ServiceNotifier.h"
#include <QDebug>
#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

void ServiceNotifier::notifyReady() {
#ifdef HAVE_SYSTEMD
    // The sd-daemon headers are C, so they work out of the box in C++
    int ret = sd_notify(0, "READY=1");
    if (ret > 0) {
        qDebug() << "Successfully notified systemd of READY state.";
    } else if (ret == 0) {
        qDebug() << "NOT_NOTIFY_SOCKET: Application not started by systemd.";
    } else {
        qWarning() << "Failed to notify systemd. Error code:" << ret;
    }
#else
    qDebug() << "Systemd notification not supported or disabled on this platform.";
#endif
}

void ServiceNotifier::notifyWatchdog() {
#ifdef HAVE_SYSTEMD
    sd_notify(0, "WATCHDOG=1");
#endif
}
