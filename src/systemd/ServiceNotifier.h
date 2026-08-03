#ifndef SERVICENOTIFIER_H
#define SERVICENOTIFIER_H

class ServiceNotifier {
public:
    static void notifyReady();
    static void notifyWatchdog();
};

#endif // SERVICENOTIFIER_H
