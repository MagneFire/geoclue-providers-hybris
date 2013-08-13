/*
    Copyright (C) 2013 Jolla Ltd.
    Contact: Aaron McCarthy <aaron.mccarthy@jollamobile.com>
*/

#include "hybrisprovider.h"
#include "position_adaptor.h"
#include "geoclue_adaptor.h"

namespace
{

HybrisProvider *staticProvider = 0;

const qint64 MaxLocationAge = 1000;
const int QuitIdleTime = 30000;
const quint32 MinimumInterval = 1000;
const quint32 PreferredAccuracy = 1;
const quint32 PreferredInitialFixTime = 0;

void locationCallback(GpsLocation *location)
{
    Location loc;

    loc.setTimestamp(location->timestamp);

    if (location->flags & GPS_LOCATION_HAS_LAT_LONG) {
        loc.setLatitude(location->latitude);
        loc.setLongitude(location->longitude);
    }

    if (location->flags & GPS_LOCATION_HAS_ALTITUDE)
        loc.setAltitude(location->altitude);

    if (location->flags & GPS_LOCATION_HAS_SPEED)
        loc.setSpeed(location->speed);

    if (location->flags & GPS_LOCATION_HAS_BEARING)
        loc.setDirection(location->bearing);

    if (location->flags & GPS_LOCATION_HAS_ACCURACY) {
        Accuracy accuracy;
        accuracy.setHorizontal(location->accuracy);
        accuracy.setVertical(location->accuracy);
    }

    QMetaObject::invokeMethod(staticProvider, "setLocation", Q_ARG(Location, loc));
}

void statusCallback(GpsStatus *status)
{
    qDebug() << Q_FUNC_INFO << status->status;
}

void svStatusCallback(GpsSvStatus *svStatus)
{
    qDebug() << Q_FUNC_INFO << svStatus->num_svs;
}

void nmeaCallback(GpsUtcTime timestamp, const char *nmea, int length)
{
    qDebug() << Q_FUNC_INFO << timestamp << nmea << length;
}

void setCapabilitiesCallback(uint32_t capabilities)
{
    qDebug() << Q_FUNC_INFO << hex << capabilities;
}

void acquireWakelockCallback()
{
    qDebug() << Q_FUNC_INFO;
}

void releaseWakelockCallback()
{
    qDebug() << Q_FUNC_INFO;
}

pthread_t createThreadCallback(const char *name, void (*start)(void *), void *arg)
{
    pthread_t threadId;
    pthread_attr_t attr;

    int error = pthread_attr_init(&attr);
    error = pthread_create(&threadId, &attr, (void*(*)(void*))start, arg);

    if (error)
      return 0;

    return threadId;
}

void requestUtcTimeCallback()
{
    qDebug() << Q_FUNC_INFO;
}

void networkLocationRequest(UlpNetworkRequestPos *req)
{
    qDebug() << Q_FUNC_INFO;
}

void requestPhoneContext(UlpPhoneContextRequest *req)
{
    qDebug() << Q_FUNC_INFO << req->context_type << req->request_type << req->interval_ms;
    QMetaObject::invokeMethod(staticProvider, "requestPhoneContext",
                              Q_ARG(UlpPhoneContextRequest *, req));
}

void agpsStatusCallback(AGpsStatus *status)
{
    qDebug() << Q_FUNC_INFO;
}

void gpsNiNotifyCallback(GpsNiNotification *notification)
{
    qDebug() << Q_FUNC_INFO;
}

void agpsRilRequestSetId(uint32_t flags)
{
    qDebug() << Q_FUNC_INFO << flags;
}

void agpsRilRequestRefLoc(uint32_t flags)
{
    qDebug() << Q_FUNC_INFO << flags;
}

void gpsXtraDownloadRequest()
{
    qDebug() << Q_FUNC_INFO;
}

}

GpsCallbacks gpsCallbacks = {
  sizeof(GpsCallbacks),
  locationCallback,
  statusCallback,
  svStatusCallback,
  nmeaCallback,
  setCapabilitiesCallback,
  acquireWakelockCallback,
  releaseWakelockCallback,
  createThreadCallback,
  requestUtcTimeCallback
};

UlpNetworkLocationCallbacks ulpNetworkCallbacks = {
    networkLocationRequest
};

UlpPhoneContextCallbacks ulpPhoneContextCallbacks = {
    requestPhoneContext
};

AGpsCallbacks agpsCallbacks = {
    agpsStatusCallback,
    createThreadCallback
};

GpsNiCallbacks gpsNiCallbacks = {
    gpsNiNotifyCallback,
    createThreadCallback
};

AGpsRilCallbacks agpsRilCallbacks = {
    agpsRilRequestSetId,
    agpsRilRequestRefLoc,
    createThreadCallback
};

GpsXtraCallbacks gpsXtraCallbacks = {
    gpsXtraDownloadRequest,
    createThreadCallback
};


QDBusArgument &operator<<(QDBusArgument &argument, const Accuracy &accuracy)
{
    const qint32 GeoclueAccuracyLevelDetailed = 6;

    argument.beginStructure();
    argument << GeoclueAccuracyLevelDetailed << accuracy.horizontal() << accuracy.vertical();
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, Accuracy &accuracy)
{
    qint32 level;
    double a;

    argument.beginStructure();
    argument >> level;
    argument >> a;
    accuracy.setHorizontal(a);
    argument >> a;
    accuracy.setVertical(a);
    argument.endStructure();
    return argument;
}


HybrisProvider::HybrisProvider(QObject *parent)
:   QObject(parent), m_gps(0), m_ulpNetwork(0), m_ulpPhoneContext(0), m_agps(0), m_agpsril(0),
    m_gpsni(0), m_xtra(0)
{
    if (staticProvider)
        qFatal("Only a single instance of HybrisProvider is supported.");

    qRegisterMetaType<UlpPhoneContextRequest *>();
    qRegisterMetaType<Location>();
    qDBusRegisterMetaType<Accuracy>();

    staticProvider = this;

    new PositionAdaptor(this);
    new GeoclueAdaptor(this);

    m_watcher = new QDBusServiceWatcher(this);
    m_watcher->setConnection(QDBusConnection::sessionBus());
    m_watcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(m_watcher, SIGNAL(serviceUnregistered(QString)),
            this, SLOT(serviceUnregistered(QString)));

    m_idleTimer = startTimer(QuitIdleTime);

    const hw_module_t *hwModule;

    int error = hw_get_module(GPS_HARDWARE_MODULE_ID, &hwModule);
    if (error) {
        qWarning("Android GPS interface not found, error %d\n", error);
        return;
    }

    qWarning("Android GPS hardware module \"%s\" \"%s\" %u.%u\n", hwModule->id, hwModule->name,
             hwModule->module_api_version, hwModule->hal_api_version);

    error = hwModule->methods->open(hwModule, GPS_HARDWARE_MODULE_ID,
                                    reinterpret_cast<hw_device_t **>(&m_gpsDevice));
    if (error) {
        qWarning("Failed to open GPS device, error %d\n", error);
        return;
    }

    m_gps = m_gpsDevice->get_gps_interface(m_gpsDevice);

    qWarning("Initialising GPS interface\n");
    error = m_gps->init(&gpsCallbacks);
    if (error) {
        qWarning("Failed to initialise GPS interface, error %d\n", error);
        return;
    }

    m_ulpNetwork = static_cast<const UlpNetworkInterface *>(m_gps->get_extension(ULP_NETWORK_INTERFACE));
    if (m_ulpNetwork) {
        qWarning("Initialising ULP Network Interface\n");
        error = m_ulpNetwork->init(&ulpNetworkCallbacks);
        if (error)
            qWarning("ULP Network Interface init failed, error %d\n", error);
    }

    m_ulpPhoneContext = static_cast<const UlpPhoneContextInterface *>(m_gps->get_extension(ULP_PHONE_CONTEXT_INTERFACE));
    if (m_ulpPhoneContext) {
        qWarning("Initialising ULP Phone Context Interface\n");
        error = m_ulpPhoneContext->init(&ulpPhoneContextCallbacks);
        if (error)
            qWarning("ULP Phone Context Interface init failed, error %d\n", error);
    }

    m_agps = static_cast<const AGpsInterface *>(m_gps->get_extension(AGPS_INTERFACE));
    if (m_agps) {
        qWarning("Initialising AGPS Interface\n");
        m_agps->init(&agpsCallbacks);
    }

    m_gpsni = static_cast<const GpsNiInterface *>(m_gps->get_extension(GPS_NI_INTERFACE));
    if (m_gpsni) {
        qWarning("Initialising GPS NI Interface\n");
        m_gpsni->init(&gpsNiCallbacks);
    }

    m_agpsril = static_cast<const AGpsRilInterface *>(m_gps->get_extension(AGPS_RIL_INTERFACE));
    if (m_agpsril) {
        qWarning("Initialising AGPS RIL Interface\n");
        m_agpsril->init(&agpsRilCallbacks);
    }

    m_xtra = static_cast<const GpsXtraInterface *>(m_gps->get_extension(GPS_XTRA_INTERFACE));
    if (m_xtra) {
        qWarning("Initialising GPS Xtra Interface\n");
        error = m_xtra->init(&gpsXtraCallbacks);
        if (error)
            qWarning("GPS Xtra Interface init failed, error %d\n", error);
    }

    m_debug = static_cast<const GpsDebugInterface *>(m_gps->get_extension(GPS_DEBUG_INTERFACE));
}

HybrisProvider::~HybrisProvider()
{
    m_gps->cleanup();

    if (m_gpsDevice->common.close)
        m_gpsDevice->common.close(reinterpret_cast<hw_device_t *>(m_gpsDevice));

    if (staticProvider == this)
        staticProvider = 0;
}

void HybrisProvider::AddReference()
{
    qDebug() << Q_FUNC_INFO;

    if (!calledFromDBus())
        qFatal("AddReference must only be called from DBus");

    const QString service = message().service();
    if (!m_watchedServices.contains(service))
        m_watcher->addWatchedService(service);

    m_watchedServices.append(service);

    startPositioningIfNeeded();
}

void HybrisProvider::RemoveReference()
{
    qDebug() << Q_FUNC_INFO;

    if (!calledFromDBus())
        qFatal("RemoveReference must only be called from DBus");

    const QString service = message().service();
    m_watchedServices.removeOne(service);
    if (!m_watchedServices.contains(service))
        m_watcher->removeWatchedService(service);

    stopPositioningIfNeeded();
}

QString HybrisProvider::GetProviderInfo(QString &description)
{
    description = tr("Android GPS provider");
    return QLatin1String("Hybris");
}

int HybrisProvider::GetStatus()
{
    qDebug() << Q_FUNC_INFO;
    if (!m_gps && !m_agps && !m_agpsril)
        return StatusUnavailable;

    return StatusAcquiring;
}

void HybrisProvider::SetOptions(const QVariantMap &options)
{
    qDebug() << Q_FUNC_INFO << options;
}

int HybrisProvider::GetPosition(int &timestamp, double &latitude, double &longitude,
                                double &altitude, Accuracy &accuracy)
{
    qDebug() << Q_FUNC_INFO;

    PositionFields positionFields = NoPositionFields;

    if (m_currentLocation.timestamp() < QDateTime::currentMSecsSinceEpoch() - MaxLocationAge) {
        // Current position is too old, wait for update.
        setDelayedReply(true);
        m_pendingCalls.append(message());
        startPositioningIfNeeded();
    } else {
        timestamp = m_currentLocation.timestamp();
        if (!qIsNaN(m_currentLocation.latitude()))
            positionFields |= LatitudePresent;
        latitude = m_currentLocation.latitude();
        if (!qIsNaN(m_currentLocation.longitude()))
            positionFields |= LongitudePresent;
        longitude = m_currentLocation.longitude();
        if (!qIsNaN(m_currentLocation.altitude()))
            positionFields |= AltitudePresent;
        altitude = m_currentLocation.altitude();
        accuracy = m_currentLocation.accuracy();
    }

    return positionFields;
}

int HybrisProvider::GetVelocity(int &timestamp, double &speed, double &direction, double &climb)
{
    qDebug() << Q_FUNC_INFO;

    VelocityFields velocityFields = NoVelocityFields;

    if (m_currentLocation.timestamp() < QDateTime::currentMSecsSinceEpoch() - MaxLocationAge) {
        // Current position is too old, wait for update.
        setDelayedReply(true);
        m_pendingCalls.append(message());
        startPositioningIfNeeded();
    } else {
        timestamp = m_currentLocation.timestamp();
        if (!qIsNaN(m_currentLocation.speed()))
            velocityFields |= SpeedPresent;
        speed = m_currentLocation.speed();
        if (!qIsNaN(m_currentLocation.direction()))
            velocityFields |= DirectionPresent;
        direction = m_currentLocation.direction();
        if (!qIsNaN(m_currentLocation.climb()))
            velocityFields |= ClimbPresent;
        climb = m_currentLocation.climb();
    }

    return velocityFields;
}

void HybrisProvider::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_idleTimer) {
        killTimer(m_idleTimer);
        m_idleTimer = -1;
        qApp->quit();
    } else {
        QObject::timerEvent(event);
    }
}

void HybrisProvider::requestPhoneContext(UlpPhoneContextRequest *req)
{
    qDebug() << Q_FUNC_INFO;
    m_settings.context_type = req->context_type;
    m_settings.is_gps_enabled = true;
    m_settings.is_network_position_available = false;
    m_settings.is_wifi_setting_enabled = false;
    m_settings.is_battery_charging = false;
    m_settings.is_agps_enabled = false;
    m_settings.is_enh_location_services_enabled = false;

    int error = m_ulpPhoneContext->ulp_phone_context_settings_update(&m_settings);
    if (error)
        qWarning("ULP Phone Context Settings update failed, error %d", error);
}

void HybrisProvider::setLocation(const Location &location)
{
    m_currentLocation = location;
    emitLocationChanged();
}

void HybrisProvider::serviceUnregistered(const QString &service)
{
    qDebug() << Q_FUNC_INFO;
    m_watchedServices.removeAll(service);
    m_watcher->removeWatchedService(service);
    stopPositioningIfNeeded();
}

void HybrisProvider::emitLocationChanged()
{
    qDebug() << Q_FUNC_INFO;
    PositionFields positionFields = NoPositionFields;

    if (!qIsNaN(m_currentLocation.latitude()))
        positionFields |= LatitudePresent;
    if (!qIsNaN(m_currentLocation.longitude()))
        positionFields |= LongitudePresent;
    if (!qIsNaN(m_currentLocation.altitude()))
        positionFields |= AltitudePresent;

    VelocityFields velocityFields = NoVelocityFields;

    if (!qIsNaN(m_currentLocation.speed()))
        velocityFields |= SpeedPresent;
    if (!qIsNaN(m_currentLocation.direction()))
        velocityFields |= DirectionPresent;
    if (!qIsNaN(m_currentLocation.climb()))
        velocityFields |= ClimbPresent;

    emit PositionChanged(positionFields, m_currentLocation.timestamp(),
                         m_currentLocation.latitude(), m_currentLocation.longitude(),
                         m_currentLocation.altitude(), m_currentLocation.accuracy());

    emit VelocityChanged(velocityFields, m_currentLocation.timestamp(), m_currentLocation.speed(),
                         m_currentLocation.direction(), m_currentLocation.climb());

    if (!m_pendingCalls.isEmpty()) {
        QVariantList positionArguments;
        positionArguments.append(QVariant::fromValue<qint32>(positionFields));
        positionArguments.append(int(m_currentLocation.timestamp()));
        positionArguments.append(m_currentLocation.latitude());
        positionArguments.append(m_currentLocation.longitude());
        positionArguments.append(m_currentLocation.altitude());
        positionArguments.append(QVariant::fromValue(m_currentLocation.accuracy()));

        QVariantList velocityArguments;
        velocityArguments.append(QVariant::fromValue<qint32>(velocityFields));
        velocityArguments.append(m_currentLocation.speed());
        velocityArguments.append(m_currentLocation.direction());
        velocityArguments.append(m_currentLocation.climb());

        QDBusConnection connection = QDBusConnection::sessionBus();
        foreach (const QDBusMessage &message, m_pendingCalls) {
            if (message.member() == QStringLiteral("GetPosition"))
                connection.send(message.createReply(positionArguments));
            else if (message.member() == QStringLiteral("GetVelocity"))
                connection.send(message.createReply(velocityArguments));
            else
                qWarning("Unknown method for pending call, %s", qPrintable(message.member()));
        }

        m_pendingCalls.clear();
    }
}

void HybrisProvider::startPositioningIfNeeded()
{
    qDebug() << Q_FUNC_INFO;

    if (m_watchedServices.length() + m_pendingCalls.length() != 1)
        return;

    if (m_idleTimer != -1) {
        qDebug() << "Stopping idle timer";
        killTimer(m_idleTimer);
        m_idleTimer = -1;
    }

    qDebug() << "Setting positioning mode";

    int error = m_gps->set_position_mode(GPS_POSITION_MODE_STANDALONE,
                                         GPS_POSITION_RECURRENCE_PERIODIC, MinimumInterval,
                                         PreferredAccuracy, PreferredInitialFixTime);
    if (error) {
        qWarning("Failed to set position mode, error %d\n", error);
        return;
    }

    qDebug() << "Starting positioning";

    error = m_gps->start();
    if (error) {
        qWarning("Failed to start positioning, error %d\n", error);
        return;
    }
}

void HybrisProvider::stopPositioningIfNeeded()
{
    qDebug() << Q_FUNC_INFO;

    if (!m_watchedServices.isEmpty() || !m_pendingCalls.isEmpty())
        return;

    qDebug() << "Stoping positioning";

    int error = m_gps->stop();
    if (error)
        qWarning("Failed to stop positioning, error %d\n", error);

    qDebug() << "Going to quit in" << QuitIdleTime << "ms";
    m_idleTimer = startTimer(QuitIdleTime);
}
