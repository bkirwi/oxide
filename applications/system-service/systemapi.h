#ifndef SYSTEMAPI_H
#define SYSTEMAPI_H

#include <QObject>
#include <QMetaType>
#include <QMutableListIterator>
#include <QTimer>
#include <QMutex>

#include "apibase.h"
#include "buttonhandler.h"
#include "application.h"
#include "screenapi.h"
#include "digitizerhandler.h"
#include "login1_interface.h"


#define GESTURE_LENGTH 30

#define systemAPI SystemAPI::singleton()

typedef org::freedesktop::login1::Manager Manager;

struct Inhibitor {
    int fd;
    QString who;
    QString what;
    QString why;
    bool block;
    Inhibitor(Manager* systemd, QString what, QString who, QString why, bool block = false)
     : who(who), what(what), why(why), block(block) {
        QDBusUnixFileDescriptor reply = systemd->Inhibit(what, who, why, block ? "block" : "delay");
        fd = reply.takeFileDescriptor();
    }
    void release(){
        if(released()){
            return;
        }
        close(fd);
        fd = -1;
    }
    bool released() { return fd == -1; }
};

struct Touch {
    int slot = 0;
    int id = -1;
    int x = 0;
    int y = 0;
    bool active = false;
    bool existing = false;
    bool modified = true;
    int pressure = 0;
    int major = 0;
    int minor = 0;
    int orientation = 0;
    string debugString() const{
        return "<Touch " + to_string(id) + " (" + to_string(x) + ", " + to_string(y) + ") " + (active ? "pressed" : "released") + ">";
    }
};
#ifdef DEBUG
QDebug operator<<(QDebug debug, const Touch& touch);
QDebug operator<<(QDebug debug, Touch* touch);
#endif
Q_DECLARE_METATYPE(Touch)
Q_DECLARE_METATYPE(input_event)

class SystemAPI : public APIBase {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", OXIDE_SYSTEM_INTERFACE)
    Q_PROPERTY(int autoSleep READ autoSleep WRITE setAutoSleep NOTIFY autoSleepChanged)
    Q_PROPERTY(bool sleepInhibited READ sleepInhibited NOTIFY sleepInhibitedChanged)
    Q_PROPERTY(bool powerOffInhibited READ powerOffInhibited NOTIFY powerOffInhibitedChanged)
public:
    enum SwipeDirection { None, Right, Left, Up, Down };
    Q_ENUM(SwipeDirection)
    static SystemAPI* singleton(SystemAPI* self = nullptr){
        static SystemAPI* instance;
        if(self != nullptr){
            instance = self;
        }
        return instance;
    }
    SystemAPI(QObject* parent)
     : APIBase(parent),
       suspendTimer(this),
       settings(this),
       sleepInhibitors(),
       powerOffInhibitors(),
       mutex(),
       touches(),
       swipeStates() {
        for(short i = Right; i <= Down; i++){
            swipeStates[(SwipeDirection)i] = true;
        }
        settings.sync();
        singleton(this);
        this->resumeApp = nullptr;
        systemd = new Manager("org.freedesktop.login1", "/org/freedesktop/login1", QDBusConnection::systemBus(), this);
        // Handle Systemd signals
        connect(systemd, &Manager::PrepareForSleep, this, &SystemAPI::PrepareForSleep);
        connect(&suspendTimer, &QTimer::timeout, this, &SystemAPI::timeout);

        auto autoSleep = settings.value("autoSleep", 1).toInt();
        m_autoSleep = autoSleep;
        if(autoSleep < 0) {
            m_autoSleep = 0;

        }else if(autoSleep > 10){
            m_autoSleep = 10;
        }
        if(autoSleep != m_autoSleep){
            m_autoSleep = autoSleep;
            settings.setValue("autoSleep", autoSleep);
            settings.sync();
            emit autoSleepChanged(autoSleep);
        }
        qDebug() << "Auto Sleep" << autoSleep;
        if(m_autoSleep){
            suspendTimer.start(m_autoSleep * 60 * 1000);
        }else if(!m_autoSleep){
            suspendTimer.stop();
        }
        // Ask Systemd to tell us nicely when we are about to suspend or resume
        inhibitSleep();
        inhibitPowerOff();
        qRegisterMetaType<input_event>();
        connect(touchHandler, &DigitizerHandler::inputEvent, this, &SystemAPI::touchEvent);
        connect(touchHandler, &DigitizerHandler::activity, this, &SystemAPI::activity);
        connect(wacomHandler, &DigitizerHandler::activity, this, &SystemAPI::activity);
        qDebug() << "System API ready to use";
    }
    ~SystemAPI(){
        qDebug() << "Removing all inhibitors";
        rguard(false);
        QMutableListIterator<Inhibitor> i(inhibitors);
        while(i.hasNext()){
            auto inhibitor = i.next();
            inhibitor.release();
            i.remove();
        }
        delete systemd;
    }
    void setEnabled(bool enabled){
        qDebug() << "System API" << enabled;
    }
    int autoSleep(){return m_autoSleep; }
    void setAutoSleep(int autoSleep);
    bool sleepInhibited(){ return sleepInhibitors.length(); }
    bool powerOffInhibited(){ return powerOffInhibitors.length(); }
    void uninhibitAll(QString name);
    void stopSuspendTimer(){
        qDebug() << "Suspend timer disabled";
        suspendTimer.stop();
    }
    void startSuspendTimer();
    void lock(){ mutex.lock(); }
    void unlock() { mutex.unlock(); }
    void setSwipeEnabled(SwipeDirection direction, bool enabled){ swipeStates[direction] = enabled; }
    bool getSwipeEnabled(SwipeDirection direction){ return swipeStates[direction]; }
    void toggleSwipeEnabled(SwipeDirection direction){ setSwipeEnabled(direction, !getSwipeEnabled(direction)); }
public slots:
    void suspend(){
        if(sleepInhibited()){
            qDebug() << "Unable to suspend. Action is currently inhibited.";
            return;
        }
        qDebug() << "Requesting Suspend...";
        systemd->Suspend(false).waitForFinished();
        qDebug() << "Suspend requested.";
    }
    void powerOff() {
        if(powerOffInhibited()){
            qDebug() << "Unable to power off. Action is currently inhibited.";
            return;
        }
        qDebug() << "Requesting Power off...";
        releasePowerOffInhibitors(true);
        rguard(false);
        systemd->PowerOff(false).waitForFinished();
        qDebug() << "Power off requested";
    }
    void reboot() {
        if(powerOffInhibited()){
            qDebug() << "Unable to reboot. Action is currently inhibited.";
            return;
        }
        qDebug() << "Requesting Reboot...";
        releasePowerOffInhibitors(true);
        rguard(false);
        systemd->Reboot(false).waitForFinished();
        qDebug() << "Reboot requested";
    }
    void activity();
    void inhibitSleep(QDBusMessage message){
        if(!sleepInhibited()){
            emit sleepInhibitedChanged(true);
        }
        suspendTimer.stop();
        sleepInhibitors.append(message.service());
        inhibitors.append(Inhibitor(systemd, "sleep:handle-suspend-key:idle", message.service(), "Application requested block", true));
    }
    void uninhibitSleep(QDBusMessage message);
    void inhibitPowerOff(QDBusMessage message){
        if(!powerOffInhibited()){
            emit powerOffInhibitedChanged(true);
        }
        powerOffInhibitors.append(message.service());
        inhibitors.append(Inhibitor(systemd, "shutdown:handle-power-key", message.service(), "Application requested block", true));
    }
    void uninhibitPowerOff(QDBusMessage message){
        if(!powerOffInhibited()){
            return;
        }
        powerOffInhibitors.removeAll(message.service());
        if(!powerOffInhibited()){
            emit powerOffInhibitedChanged(false);
        }
    }
    void toggleSwipes();
signals:
    void leftAction();
    void homeAction();
    void rightAction();
    void powerAction();
    void bottomAction();
    void topAction();
    void sleepInhibitedChanged(bool);
    void powerOffInhibitedChanged(bool);
    void autoSleepChanged(int);
    void deviceSuspending();
    void deviceResuming();

private slots:
    void PrepareForSleep(bool suspending);
    void timeout();
    void touchEvent(const input_event& event){
        switch(event.type){
            case EV_SYN:
                switch(event.code){
                    case SYN_REPORT:
                        // Always mark the current slot as modified
                        auto touch = getEvent(currentSlot);
                        touch->modified = true;
                        QList<Touch*> released;
                        QList<Touch*> pressed;
                        QList<Touch*> moved;
                        for(auto touch : touches.values()){
                            if(touch->id == -1){
                                touch->active = false;
                                released.append(touch);
                            }else if(!touch->active){
                                released.append(touch);
                            }else if(!touch->existing){
                                pressed.append(touch);
                            }else if(touch->modified){
                                moved.append(touch);
                            }
                        }
                        if(pressed.length()){
                            touchDown(pressed);
                        }
                        if(moved.length()){
                            touchMove(moved);
                        }
                        if(released.length()){
                            touchUp(released);
                        }
                        // Cleanup released touches
                        for(auto touch : released){
                            if(!touch->active){
                                touches.remove(touch->slot);
                                delete touch;
                            }
                        }
                        // Setup touches for next event set
                        for(auto touch : touches.values()){
                            touch->modified = false;
                            touch->existing = true;
                        }
                    break;
                }
            break;
            case EV_ABS:
                if(currentSlot == -1 && event.code != ABS_MT_SLOT){
                    return;
                }
                switch(event.code){
                    case ABS_MT_SLOT:{
                        currentSlot = event.value;
                        auto touch = getEvent(currentSlot);
                        touch->modified = true;
                    }break;
                    case ABS_MT_TRACKING_ID:{
                        auto touch = getEvent(currentSlot);
                        if(event.value == -1){
                            touch->active = false;
                            currentSlot = 0;
                        }else{
                            touch->active = true;
                            touch->id = event.value;
                        }
                    }break;
                    case ABS_MT_POSITION_X:{
                        auto touch = getEvent(currentSlot);
                        touch->x = event.value;
                    }break;
                    case ABS_MT_POSITION_Y:{
                        auto touch = getEvent(currentSlot);
                        touch->y = event.value;
                    }break;
                    case ABS_MT_PRESSURE:{
                        auto touch = getEvent(currentSlot);
                        touch->pressure = event.value;
                    }break;
                    case ABS_MT_TOUCH_MAJOR:{
                        auto touch = getEvent(currentSlot);
                        touch->major = event.value;
                    }break;
                    case ABS_MT_TOUCH_MINOR:{
                        auto touch = getEvent(currentSlot);
                        touch->minor = event.value;
                    }break;
                    case ABS_MT_ORIENTATION:{
                        auto touch = getEvent(currentSlot);
                        touch->orientation = event.value;
                    }break;
                }
            break;
        }
    }

private:
    Manager* systemd;
    QList<Inhibitor> inhibitors;
    Application* resumeApp;
    QTimer suspendTimer;
    QSettings settings;
    QStringList sleepInhibitors;
    QStringList powerOffInhibitors;
    QMutex mutex;
    QMap<int, Touch*> touches;
    int currentSlot = 0;
    int m_autoSleep;
    bool wifiWasOn = false;
    int swipeDirection = None;
    QPoint location;
    QPoint startLocation;
    QMap<SwipeDirection, bool> swipeStates;

    void inhibitSleep(){
        inhibitors.append(Inhibitor(systemd, "sleep", qApp->applicationName(), "Handle sleep screen"));
    }
    void inhibitPowerOff(){
        inhibitors.append(Inhibitor(systemd, "shutdown", qApp->applicationName(), "Block power off from any other method", true));
        rguard(true);
    }
    void releaseSleepInhibitors(bool block = false){
        QMutableListIterator<Inhibitor> i(inhibitors);
        while(i.hasNext()){
            auto inhibitor = i.next();
            if(inhibitor.what.contains("sleep") && inhibitor.block == block){
                inhibitor.release();
            }
            if(inhibitor.released()){
                i.remove();
            }
        }
    }
    void releasePowerOffInhibitors(bool block = false){
        QMutableListIterator<Inhibitor> i(inhibitors);
        while(i.hasNext()){
            auto inhibitor = i.next();
            if(inhibitor.what.contains("shutdown") && inhibitor.block == block){
                inhibitor.release();
            }
            if(inhibitor.released()){
                i.remove();
            }
        }
    }
    void rguard(bool install){
        QProcess::execute("/opt/bin/rguard", QStringList() << (install ? "-1" : "-0"));
    }
    Touch* getEvent(int slot){
        if(slot == -1){
            return nullptr;
        }
        if(!touches.contains(slot)){
            touches.insert(slot, new Touch{
                .slot = slot
            });
        }
        return touches.value(slot);
    }
    int getCurrentFingers(){
        return std::count_if(touches.begin(), touches.end(), [](Touch* touch){
            return touch->active;
        });
    }

    void touchDown(QList<Touch*> touches){
#ifdef DEBUG
        qDebug() << "DOWN" << touches;
#endif
        if(getCurrentFingers() != 1){
            return;
        }
        auto touch = touches.first();
        if(swipeDirection != None){
            return;
        }
        int offset = 20;
        if(deviceSettings.getDeviceType() == DeviceSettings::RM2){
            offset = 40;
        }
        if(touch->y <= offset){
            swipeDirection = Up;
        }else if(touch->y > (deviceSettings.getTouchHeight() - offset)){
            swipeDirection = Down;
        }else if(touch->x <= offset){
            if(deviceSettings.getDeviceType() == DeviceSettings::RM2){
                swipeDirection = Right;
            }else{
                swipeDirection = Left;
            }
        }else if(touch->x > (deviceSettings.getTouchWidth() - offset)){
            if(deviceSettings.getDeviceType() == DeviceSettings::RM2){
                swipeDirection = Left;
            }else{
                swipeDirection = Right;
            }
        }else{
            return;
        }
        //touchHandler->grab();
#ifdef DEBUG
            qDebug() << "Swipe started" << swipeDirection;
#endif
        startLocation = location = QPoint(touch->x, touch->y);
    }
    void touchUp(QList<Touch*> touches){
#ifdef DEBUG
        qDebug() << "UP" << touches;
#endif
        if(swipeDirection == None){
#ifdef DEBUG
            qDebug() << "Not swiping";
#endif
            if(touchHandler->grabbed()){
                for(auto touch : touches){
                    writeTouchUp(touch);
                }
                touchHandler->ungrab();
            }
            return;
        }
        if(getCurrentFingers()){
#ifdef DEBUG
            qDebug() << "Still swiping";
#endif
            if(touchHandler->grabbed()){
                for(auto touch : touches){
                    writeTouchUp(touch);
                }
            }
            return;
        }
        if(touches.length() > 1){
#ifdef DEBUG
            qDebug() << "Too many fingers";
#endif
            if(touchHandler->grabbed()){
                for(auto touch : touches){
                    writeTouchUp(touch);
                }
                touchHandler->ungrab();
            }
            swipeDirection = None;
            return;
        }
        auto touch = touches.first();
        if(swipeDirection == Up){
            if(!swipeStates[Up] || touch->y < location.y() || touch->y - startLocation.y() < GESTURE_LENGTH){
                // Must end swiping up and having gone far enough
                cancelSwipe(touch);
                return;
            }
            emit bottomAction();
        }else if(swipeDirection == Down){
            if(!swipeStates[Down] || touch->y > location.y() || startLocation.y() - touch->y < GESTURE_LENGTH){
                // Must end swiping down and having gone far enough
                cancelSwipe(touch);
                return;
            }
            emit topAction();
        }else if(swipeDirection == Right || swipeDirection == Left){
            auto isRM2 = deviceSettings.getDeviceType() == DeviceSettings::RM2;
            auto invalidLeft = !swipeStates[Left] || touch->x < location.x() || touch->x - startLocation.x() < GESTURE_LENGTH;
            auto invalidRight = !swipeStates[Right] || touch->x > location.x() || startLocation.x() - touch->x < GESTURE_LENGTH;
            if(swipeDirection == Right && (isRM2 ? invalidLeft : invalidRight)){
                // Must end swiping right and having gone far enough
                cancelSwipe(touch);
                return;
            }else if(swipeDirection == Left && (isRM2 ? invalidRight : invalidLeft)){
                // Must end swiping left and having gone far enough
                cancelSwipe(touch);
                return;
            }
            if(swipeDirection == Left){
                emit rightAction();

            }else{
                emit leftAction();
            }
        }
        swipeDirection = None;
        touchHandler->ungrab();
        touch->x = -1;
        touch->y = -1;
        writeTouchUp(touch);
#ifdef DEBUG
            qDebug() << "Swipe direction" << swipeDirection;
#endif
    }
    void touchMove(QList<Touch*> touches){
#ifdef DEBUG
        qDebug() << "MOVE" << touches;
#endif
        if(swipeDirection == None){
            if(touchHandler->grabbed()){
                for(auto touch : touches){
                    writeTouchMove(touch);
                }
                touchHandler->ungrab();
            }
            return;
        }
        if(touches.length() > 1){
#ifdef DEBUG
            qDebug() << "Too many fingers";
#endif
            if(touchHandler->grabbed()){
                for(auto touch : touches){
                    writeTouchMove(touch);
                }
                touchHandler->ungrab();
            }
            swipeDirection = None;
            return;
        }
        auto touch = touches.first();
        if(touch->y > location.y()){
            location = QPoint(touch->x, touch->y);
        }
    }
    void cancelSwipe(Touch* touch){
#ifdef DEBUG
        qDebug() << "Swipe Cancelled";
#endif
        swipeDirection = None;
        touchHandler->ungrab();
        writeTouchUp(touch);
    }
    void writeTouchUp(Touch* touch){
        bool grabbed = touchHandler->grabbed();
        if(grabbed){
            touchHandler->ungrab();
        }
        writeTouchMove(touch);
#ifdef DEBUG
        qDebug() << "Write touch up" << touch;
#endif
        int size = sizeof(input_event) * 3;
        input_event* events = (input_event*)malloc(size);
        events[0] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_SLOT, touch->slot);
        events[1] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_TRACKING_ID, -1);
        events[2] = DigitizerHandler::createEvent(EV_SYN, 0, 0);
        touchHandler->write(events, size);
        free(events);
        if(grabbed){
            touchHandler->grab();
        }
    }
    void writeTouchMove(Touch* touch){
        bool grabbed = touchHandler->grabbed();
        if(grabbed){
            touchHandler->ungrab();
        }
#ifdef DEBUG
        qDebug() << "Write touch move" << touch;
#endif
        int size = sizeof(input_event) * 8;
        input_event* events = (input_event*)malloc(size);
        events[2] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_SLOT, touch->slot);
        events[2] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_POSITION_X, touch->x);
        events[2] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_POSITION_Y, touch->y);
        events[2] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_PRESSURE, touch->pressure);
        events[2] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_TOUCH_MAJOR, touch->major);
        events[2] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_TOUCH_MINOR, touch->minor);
        events[2] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_ORIENTATION, touch->orientation);
        events[2] = DigitizerHandler::createEvent(EV_SYN, 0, 0);
        touchHandler->write(events, size);
        free(events);
        if(grabbed){
            touchHandler->grab();
        }
    }
    void fn(){
        auto n = 512 * 8;
        auto num_inst = 4;
        input_event* ev = (input_event *)malloc(sizeof(struct input_event) * n * num_inst);
        memset(ev, 0, sizeof(input_event) * n * num_inst);
        auto i = 0;
        while (i < n) {
            ev[i++] = DigitizerHandler::createEvent(EV_ABS, ABS_DISTANCE, 1);
            ev[i++] = DigitizerHandler::createEvent(EV_SYN, 0, 0);
            ev[i++] = DigitizerHandler::createEvent(EV_ABS, ABS_DISTANCE, 2);
            ev[i++] = DigitizerHandler::createEvent(EV_SYN, 0, 0);
        }
    }
};

#endif // SYSTEMAPI_H
