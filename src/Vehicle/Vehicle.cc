/*=====================================================================
 
 QGroundControl Open Source Ground Control Station
 
 (c) 2009 - 2014 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 
 This file is part of the QGROUNDCONTROL project
 
 QGROUNDCONTROL is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 QGROUNDCONTROL is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with QGROUNDCONTROL. If not, see <http://www.gnu.org/licenses/>.
 
 ======================================================================*/

#include "Vehicle.h"
#include "MAVLinkProtocol.h"
#include "FirmwarePluginManager.h"
#include "LinkManager.h"
#include "FirmwarePlugin.h"
#include "AutoPilotPluginManager.h"
#include "UASMessageHandler.h"
#include "UAS.h"
#include "JoystickManager.h"
#include "MissionManager.h"

QGC_LOGGING_CATEGORY(VehicleLog, "VehicleLog")

#define UPDATE_TIMER 50
#define DEFAULT_LAT  38.965767f
#define DEFAULT_LON -120.083923f

const char* Vehicle::_settingsGroup =               "Vehicle%1";        // %1 replaced with mavlink system id
const char* Vehicle::_joystickModeSettingsKey =     "JoystickMode";
const char* Vehicle::_joystickEnabledSettingsKey =  "JoystickEnabled";

Vehicle::Vehicle(LinkInterface* link, int vehicleId, MAV_AUTOPILOT firmwareType)
    : _id(vehicleId)
    , _active(false)
    , _firmwareType(firmwareType)
    , _firmwarePlugin(NULL)
    , _autopilotPlugin(NULL)
    , _joystickMode(JoystickModeRC)
    , _joystickEnabled(false)
    , _uas(NULL)
    , _mav(NULL)
    , _currentMessageCount(0)
    , _messageCount(0)
    , _currentErrorCount(0)
    , _currentWarningCount(0)
    , _currentNormalCount(0)
    , _currentMessageType(MessageNone)
    , _roll(0.0f)
    , _pitch(0.0f)
    , _heading(0.0f)
    , _altitudeAMSL(0.0f)
    , _altitudeWGS84(0.0f)
    , _altitudeRelative(0.0f)
    , _groundSpeed(0.0f)
    , _airSpeed(0.0f)
    , _climbRate(0.0f)
    , _navigationAltitudeError(0.0f)
    , _navigationSpeedError(0.0f)
    , _navigationCrosstrackError(0.0f)
    , _navigationTargetBearing(0.0f)
    , _latitude(DEFAULT_LAT)
    , _longitude(DEFAULT_LON)
    , _refreshTimer(new QTimer(this))
    , _batteryVoltage(0.0)
    , _batteryPercent(0.0)
    , _batteryConsumed(-1.0)
    , _systemArmed(false)
    , _currentHeartbeatTimeout(0)
    , _waypointDistance(0.0)
    , _currentWaypoint(0)
    , _satelliteCount(-1)
    , _satelliteLock(0)
    , _wpm(NULL)
    , _updateCount(0)
{
    _addLink(link);
    
    connect(MAVLinkProtocol::instance(), &MAVLinkProtocol::messageReceived, this, &Vehicle::_mavlinkMessageReceived);
    connect(this, &Vehicle::_sendMessageOnThread, this, &Vehicle::_sendMessage, Qt::QueuedConnection);
    
    _uas = new UAS(MAVLinkProtocol::instance(), this);
    
    setLatitude(_uas->getLatitude());
    setLongitude(_uas->getLongitude());
    
    connect(_uas, &UAS::latitudeChanged, this, &Vehicle::setLatitude);
    connect(_uas, &UAS::longitudeChanged, this, &Vehicle::setLongitude);
    
    _firmwarePlugin = FirmwarePluginManager::instance()->firmwarePluginForAutopilot(firmwareType);    
    _autopilotPlugin = AutoPilotPluginManager::instance()->newAutopilotPluginForVehicle(this);

    // Refresh timer
    connect(_refreshTimer, SIGNAL(timeout()), this, SLOT(_checkUpdate()));
    _refreshTimer->setInterval(UPDATE_TIMER);
    _refreshTimer->start(UPDATE_TIMER);
    emit heartbeatTimeoutChanged();
    
    _mav = uas();
    // Reset satellite count (no GPS)
    _satelliteCount = -1;
    emit satelliteCountChanged();
    // Reset connection lost (if any)
    _currentHeartbeatTimeout = 0;
    emit heartbeatTimeoutChanged();
    // Listen for system messages
    connect(UASMessageHandler::instance(), &UASMessageHandler::textMessageCountChanged, this, &Vehicle::_handleTextMessage);
    // Now connect the new UAS
    connect(_mav, SIGNAL(attitudeChanged                    (UASInterface*,double,double,double,quint64)),              this, SLOT(_updateAttitude(UASInterface*, double, double, double, quint64)));
    connect(_mav, SIGNAL(attitudeChanged                    (UASInterface*,int,double,double,double,quint64)),          this, SLOT(_updateAttitude(UASInterface*,int,double, double, double, quint64)));
    connect(_mav, SIGNAL(speedChanged                       (UASInterface*, double, double, quint64)),                  this, SLOT(_updateSpeed(UASInterface*, double, double, quint64)));
    connect(_mav, SIGNAL(altitudeChanged                    (UASInterface*, double, double, double, double, quint64)),  this, SLOT(_updateAltitude(UASInterface*, double, double, double, double, quint64)));
    connect(_mav, SIGNAL(navigationControllerErrorsChanged  (UASInterface*, double, double, double)),                   this, SLOT(_updateNavigationControllerErrors(UASInterface*, double, double, double)));
    connect(_mav, SIGNAL(statusChanged                      (UASInterface*,QString,QString)),                           this, SLOT(_updateState(UASInterface*, QString,QString)));
    connect(_mav, SIGNAL(armingChanged                      (bool)),                                                    this, SLOT(_updateArmingState(bool)));
    connect(_mav, &UASInterface::NavigationControllerDataChanged,   this, &Vehicle::_updateNavigationControllerData);
    connect(_mav, &UASInterface::heartbeatTimeout,                  this, &Vehicle::_heartbeatTimeout);
    connect(_mav, &UASInterface::batteryChanged,                    this, &Vehicle::_updateBatteryRemaining);
    connect(_mav, &UASInterface::batteryConsumedChanged,            this, &Vehicle::_updateBatteryConsumedChanged);
    connect(_mav, &UASInterface::modeChanged,                       this, &Vehicle::_updateMode);
    connect(_mav, &UASInterface::nameChanged,                       this, &Vehicle::_updateName);
    connect(_mav, &UASInterface::systemTypeSet,                     this, &Vehicle::_setSystemType);
    connect(_mav, &UASInterface::localizationChanged,               this, &Vehicle::_setSatLoc);
    _wpm = _mav->getWaypointManager();
    if (_wpm) {
        connect(_wpm, &UASWaypointManager::currentWaypointChanged,   this, &Vehicle::_updateCurrentWaypoint);
        connect(_wpm, &UASWaypointManager::waypointDistanceChanged,  this, &Vehicle::_updateWaypointDistance);
        connect(_wpm, SIGNAL(waypointViewOnlyListChanged(void)),     this, SLOT(_waypointViewOnlyListChanged(void)));
        connect(_wpm, SIGNAL(waypointViewOnlyChanged(int,MissionItem*)),this, SLOT(_updateWaypointViewOnly(int,MissionItem*)));
        _wpm->readWaypoints(true);
    }
    UAS* pUas = dynamic_cast<UAS*>(_mav);
    if(pUas) {
        _setSatelliteCount(pUas->getSatelliteCount(), QString(""));
        connect(pUas, &UAS::satelliteCountChanged, this, &Vehicle::_setSatelliteCount);
    }
    _setSystemType(_mav, _mav->getSystemType());
    _updateArmingState(_mav->isArmed());
    
    _waypointViewOnlyListChanged();
    
    _loadSettings();
    
    if (qgcApp()->useNewMissionEditor()) {
        _missionManager = new MissionManager(this);
    }
}

Vehicle::~Vehicle()
{
    // Stop listening for system messages
    disconnect(UASMessageHandler::instance(), &UASMessageHandler::textMessageCountChanged,  this, &Vehicle::_handleTextMessage);
    // Disconnect any previously connected active MAV
    disconnect(_mav, SIGNAL(attitudeChanged                     (UASInterface*, double,double,double,quint64)),             this, SLOT(_updateAttitude(UASInterface*, double, double, double, quint64)));
    disconnect(_mav, SIGNAL(attitudeChanged                     (UASInterface*, int,double,double,double,quint64)),         this, SLOT(_updateAttitude(UASInterface*,int,double, double, double, quint64)));
    disconnect(_mav, SIGNAL(speedChanged                        (UASInterface*, double, double, quint64)),                  this, SLOT(_updateSpeed(UASInterface*, double, double, quint64)));
    disconnect(_mav, SIGNAL(altitudeChanged                     (UASInterface*, double, double, double, double, quint64)),  this, SLOT(_updateAltitude(UASInterface*, double, double, double, double, quint64)));
    disconnect(_mav, SIGNAL(navigationControllerErrorsChanged   (UASInterface*, double, double, double)),                   this, SLOT(_updateNavigationControllerErrors(UASInterface*, double, double, double)));
    disconnect(_mav, SIGNAL(statusChanged                       (UASInterface*,QString,QString)),                           this, SLOT(_updateState(UASInterface*,QString,QString)));
    disconnect(_mav, SIGNAL(armingChanged                       (bool)),                                                    this, SLOT(_updateArmingState(bool)));
    disconnect(_mav, &UASInterface::NavigationControllerDataChanged, this, &Vehicle::_updateNavigationControllerData);
    disconnect(_mav, &UASInterface::heartbeatTimeout,                this, &Vehicle::_heartbeatTimeout);
    disconnect(_mav, &UASInterface::batteryChanged,                  this, &Vehicle::_updateBatteryRemaining);
    disconnect(_mav, &UASInterface::batteryConsumedChanged,          this, &Vehicle::_updateBatteryConsumedChanged);
    disconnect(_mav, &UASInterface::modeChanged,                     this, &Vehicle::_updateMode);
    disconnect(_mav, &UASInterface::nameChanged,                     this, &Vehicle::_updateName);
    disconnect(_mav, &UASInterface::systemTypeSet,                   this, &Vehicle::_setSystemType);
    disconnect(_mav, &UASInterface::localizationChanged,             this, &Vehicle::_setSatLoc);
    if (_wpm) {
        disconnect(_wpm, &UASWaypointManager::currentWaypointChanged,    this, &Vehicle::_updateCurrentWaypoint);
        disconnect(_wpm, &UASWaypointManager::waypointDistanceChanged,   this, &Vehicle::_updateWaypointDistance);
        disconnect(_wpm, SIGNAL(waypointViewOnlyListChanged(void)),      this, SLOT(_waypointViewOnlyListChanged(void)));
        disconnect(_wpm, SIGNAL(waypointViewOnlyChanged(int,MissionItem*)), this, SLOT(_updateWaypointViewOnly(int,MissionItem*)));
    }
    UAS* pUas = dynamic_cast<UAS*>(_mav);
    if(pUas) {
        disconnect(pUas, &UAS::satelliteCountChanged, this, &Vehicle::_setSatelliteCount);
    }
}

void Vehicle::_mavlinkMessageReceived(LinkInterface* link, mavlink_message_t message)
{
    if (message.sysid != _id && message.sysid != 0) {
        return;
    }
    
    if (!_containsLink(link)) {
        _addLink(link);
    }
    
    emit mavlinkMessageReceived(message);
    
    _uas->receiveMessage(message);
}

bool Vehicle::_containsLink(LinkInterface* link)
{
    foreach (SharedLinkInterface sharedLink, _links) {
        if (sharedLink.data() == link) {
            return true;
        }
    }
    
    return false;
}

void Vehicle::_addLink(LinkInterface* link)
{
    if (!_containsLink(link)) {
        _links += LinkManager::instance()->sharedPointerForLink(link);
        qCDebug(VehicleLog) << "_addLink:" << QString("%1").arg((ulong)link, 0, 16);
        connect(LinkManager::instance(), &LinkManager::linkDisconnected, this, &Vehicle::_linkDisconnected);
    }
}

void Vehicle::_linkDisconnected(LinkInterface* link)
{
    qCDebug(VehicleLog) << "_linkDisconnected:" << link->getName();
    qCDebug(VehicleLog) << "link count:" << _links.count();
    
    for (int i=0; i<_links.count(); i++) {
        if (_links[i].data() == link) {
            _links.removeAt(i);
            break;
        }
    }
    
    if (_links.count() == 0) {
        emit allLinksDisconnected(this);
    }
}

void Vehicle::sendMessage(mavlink_message_t message)
{
    emit _sendMessageOnThread(message);
}

void Vehicle::_sendMessage(mavlink_message_t message)
{
    // Emit message on all links that are currently connected
    foreach (SharedLinkInterface sharedLink, _links) {
        LinkInterface* link = sharedLink.data();
        Q_ASSERT(link);
        
        if (link->isConnected()) {
            MAVLinkProtocol* mavlink = MAVLinkProtocol::instance();
            
            // Write message into buffer, prepending start sign
            uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
            int len = mavlink_msg_to_send_buffer(buffer, &message);
            static uint8_t messageKeys[256] = MAVLINK_MESSAGE_CRCS;
            mavlink_finalize_message_chan(&message, mavlink->getSystemId(), mavlink->getComponentId(), link->getMavlinkChannel(), message.len, messageKeys[message.msgid]);
            
            if (link->isConnected()) {
                link->writeBytes((const char*)buffer, len);
            } else {
                qWarning() << "Link not connected";
            }
        }
    }
}

QList<LinkInterface*> Vehicle::links(void)
{
    QList<LinkInterface*> list;
    
    foreach (SharedLinkInterface sharedLink, _links) {
        list += sharedLink.data();
    }
    
    return list;
}

void Vehicle::setLatitude(double latitude)
{
    _geoCoordinate.setLatitude(latitude);
    emit coordinateChanged(_geoCoordinate);
}

void Vehicle::setLongitude(double longitude){
    _geoCoordinate.setLongitude(longitude);
    emit coordinateChanged(_geoCoordinate);
}

void Vehicle::_updateAttitude(UASInterface*, double roll, double pitch, double yaw, quint64)
{
    if (isinf(roll)) {
        _roll = std::numeric_limits<double>::quiet_NaN();
    } else {
        float rolldeg = _oneDecimal(roll * (180.0 / M_PI));
        if (fabs(roll - rolldeg) > 0.25) {
            _roll = rolldeg;
            if(_refreshTimer->isActive()) {
                emit rollChanged();
            }
        }
        if(_roll != rolldeg) {
            _roll = rolldeg;
            _addChange(ROLL_CHANGED);
        }
    }
    if (isinf(pitch)) {
        _pitch = std::numeric_limits<double>::quiet_NaN();
    } else {
        float pitchdeg = _oneDecimal(pitch * (180.0 / M_PI));
        if (fabs(pitch - pitchdeg) > 0.25) {
            _pitch = pitchdeg;
            if(_refreshTimer->isActive()) {
                emit pitchChanged();
            }
        }
        if(_pitch != pitchdeg) {
            _pitch = pitchdeg;
            _addChange(PITCH_CHANGED);
        }
    }
    if (isinf(yaw)) {
        _heading = std::numeric_limits<double>::quiet_NaN();
    } else {
        yaw = _oneDecimal(yaw * (180.0 / M_PI));
        if (yaw < 0) yaw += 360;
        if (fabs(_heading - yaw) > 0.25) {
            _heading = yaw;
            if(_refreshTimer->isActive()) {
                emit headingChanged();
            }
        }
        if(_heading != yaw) {
            _heading = yaw;
            _addChange(HEADING_CHANGED);
        }
    }
}

void Vehicle::_updateAttitude(UASInterface* uas, int, double roll, double pitch, double yaw, quint64 timestamp)
{
    _updateAttitude(uas, roll, pitch, yaw, timestamp);
}

void Vehicle::_updateSpeed(UASInterface*, double groundSpeed, double airSpeed, quint64)
{
    groundSpeed = _oneDecimal(groundSpeed);
    if (fabs(_groundSpeed - groundSpeed) > 0.5) {
        _groundSpeed = groundSpeed;
        if(_refreshTimer->isActive()) {
            emit groundSpeedChanged();
        }
    }
    airSpeed = _oneDecimal(airSpeed);
    if (fabs(_airSpeed - airSpeed) > 0.5) {
        _airSpeed = airSpeed;
        if(_refreshTimer->isActive()) {
            emit airSpeedChanged();
        }
    }
    if(_groundSpeed != groundSpeed) {
        _groundSpeed = groundSpeed;
        _addChange(GROUNDSPEED_CHANGED);
    }
    if(_airSpeed != airSpeed) {
        _airSpeed = airSpeed;
        _addChange(AIRSPEED_CHANGED);
    }
}

void Vehicle::_updateAltitude(UASInterface*, double altitudeAMSL, double altitudeWGS84, double altitudeRelative, double climbRate, quint64) {
    altitudeAMSL = _oneDecimal(altitudeAMSL);
    if (fabs(_altitudeAMSL - altitudeAMSL) > 0.5) {
        _altitudeAMSL = altitudeAMSL;
        if(_refreshTimer->isActive()) {
            emit altitudeAMSLChanged();
        }
    }
    altitudeWGS84 = _oneDecimal(altitudeWGS84);
    if (fabs(_altitudeWGS84 - altitudeWGS84) > 0.5) {
        _altitudeWGS84 = altitudeWGS84;
        if(_refreshTimer->isActive()) {
            emit altitudeWGS84Changed();
        }
    }
    altitudeRelative = _oneDecimal(altitudeRelative);
    if (fabs(_altitudeRelative - altitudeRelative) > 0.5) {
        _altitudeRelative = altitudeRelative;
        if(_refreshTimer->isActive()) {
            emit altitudeRelativeChanged();
        }
    }
    climbRate = _oneDecimal(climbRate);
    if (fabs(_climbRate - climbRate) > 0.5) {
        _climbRate = climbRate;
        if(_refreshTimer->isActive()) {
            emit climbRateChanged();
        }
    }
    if(_altitudeAMSL != altitudeAMSL) {
        _altitudeAMSL = altitudeAMSL;
        _addChange(ALTITUDEAMSL_CHANGED);
    }
    if(_altitudeWGS84 != altitudeWGS84) {
        _altitudeWGS84 = altitudeWGS84;
        _addChange(ALTITUDEWGS84_CHANGED);
    }
    if(_altitudeRelative != altitudeRelative) {
        _altitudeRelative = altitudeRelative;
        _addChange(ALTITUDERELATIVE_CHANGED);
    }
    if(_climbRate != climbRate) {
        _climbRate = climbRate;
        _addChange(CLIMBRATE_CHANGED);
    }
}

void Vehicle::_updateNavigationControllerErrors(UASInterface*, double altitudeError, double speedError, double xtrackError) {
    _navigationAltitudeError   = altitudeError;
    _navigationSpeedError      = speedError;
    _navigationCrosstrackError = xtrackError;
}

void Vehicle::_updateNavigationControllerData(UASInterface *uas, float, float, float, float targetBearing, float) {
    if (_mav == uas) {
        _navigationTargetBearing = targetBearing;
    }
}

/*
 * Internal
 */

bool Vehicle::_isAirplane() {
    if (_mav)
        return _mav->isAirplane();
    return false;
}

void Vehicle::_addChange(int id)
{
    if(!_changes.contains(id)) {
        _changes.append(id);
    }
}

float Vehicle::_oneDecimal(float value)
{
    int i = (value * 10);
    return (float)i / 10.0;
}

void Vehicle::_checkUpdate()
{
    // Update current location
    if(_mav) {
        if(_latitude != _mav->getLatitude()) {
            _latitude = _mav->getLatitude();
            emit latitudeChanged();
        }
        if(_longitude != _mav->getLongitude()) {
            _longitude = _mav->getLongitude();
            emit longitudeChanged();
        }
    }
    // The timer rate is 20Hz for the coordinates above. These below we only check
    // twice a second.
    if(++_updateCount > 9) {
        _updateCount = 0;
        // Check for changes
        // Significant changes, that is, whole number changes, are updated immediatelly.
        // For every message however, we set a flag for what changed and this timer updates
        // them to bring everything up-to-date. This prevents an avalanche of UI updates.
        foreach(int i, _changes) {
            switch (i) {
                case ROLL_CHANGED:
                    emit rollChanged();
                    break;
                case PITCH_CHANGED:
                    emit pitchChanged();
                    break;
                case HEADING_CHANGED:
                    emit headingChanged();
                    break;
                case GROUNDSPEED_CHANGED:
                    emit groundSpeedChanged();
                    break;
                case AIRSPEED_CHANGED:
                    emit airSpeedChanged();
                    break;
                case CLIMBRATE_CHANGED:
                    emit climbRateChanged();
                    break;
                case ALTITUDERELATIVE_CHANGED:
                    emit altitudeRelativeChanged();
                    break;
                case ALTITUDEWGS84_CHANGED:
                    emit altitudeWGS84Changed();
                    break;
                case ALTITUDEAMSL_CHANGED:
                    emit altitudeAMSLChanged();
                    break;
                default:
                    break;
            }
        }
        _changes.clear();
    }
}

QString Vehicle::getMavIconColor()
{
    // TODO: Not using because not only the colors are ghastly, it doesn't respect dark/light palette
    if(_mav)
        return _mav->getColor().name();
    else
        return QString("black");
}

void Vehicle::_updateArmingState(bool armed)
{
    if(_systemArmed != armed) {
        _systemArmed = armed;
        emit systemArmedChanged();
    }
}

void Vehicle::_updateBatteryRemaining(UASInterface*, double voltage, double, double percent, int)
{
    
    if(percent < 0.0) {
        percent = 0.0;
    }
    if(voltage < 0.0) {
        voltage = 0.0;
    }
    if (_batteryVoltage != voltage) {
        _batteryVoltage = voltage;
        emit batteryVoltageChanged();
    }
    if (_batteryPercent != percent) {
        _batteryPercent = percent;
        emit batteryPercentChanged();
    }
}

void Vehicle::_updateBatteryConsumedChanged(UASInterface*, double current_consumed)
{
    if(_batteryConsumed != current_consumed) {
        _batteryConsumed = current_consumed;
        emit batteryConsumedChanged();
    }
}


void Vehicle::_updateState(UASInterface*, QString name, QString)
{
    if (_currentState != name) {
        _currentState = name;
        emit currentStateChanged();
    }
}

void Vehicle::_updateMode(int, QString name, QString)
{
    if (name.size()) {
        QString shortMode = name;
        shortMode = shortMode.replace("D|", "");
        shortMode = shortMode.replace("A|", "");
        if (_currentMode != shortMode) {
            _currentMode = shortMode;
            emit currentModeChanged();
        }
    }
}

void Vehicle::_updateName(const QString& name)
{
    if (_systemName != name) {
        _systemName = name;
        emit systemNameChanged();
    }
}

/**
 * The current system type is represented through the system icon.
 *
 * @param uas Source system, has to be the same as this->uas
 * @param systemType type ID, following the MAVLink system type conventions
 * @see http://pixhawk.ethz.ch/software/mavlink
 */
void Vehicle::_setSystemType(UASInterface*, unsigned int systemType)
{
    _systemPixmap = "qrc:/res/mavs/";
    switch (systemType) {
        case MAV_TYPE_GENERIC:
            _systemPixmap += "Generic";
            break;
        case MAV_TYPE_FIXED_WING:
            _systemPixmap += "FixedWing";
            break;
        case MAV_TYPE_QUADROTOR:
            _systemPixmap += "QuadRotor";
            break;
        case MAV_TYPE_COAXIAL:
            _systemPixmap += "Coaxial";
            break;
        case MAV_TYPE_HELICOPTER:
            _systemPixmap += "Helicopter";
            break;
        case MAV_TYPE_ANTENNA_TRACKER:
            _systemPixmap += "AntennaTracker";
            break;
        case MAV_TYPE_GCS:
            _systemPixmap += "Groundstation";
            break;
        case MAV_TYPE_AIRSHIP:
            _systemPixmap += "Airship";
            break;
        case MAV_TYPE_FREE_BALLOON:
            _systemPixmap += "FreeBalloon";
            break;
        case MAV_TYPE_ROCKET:
            _systemPixmap += "Rocket";
            break;
        case MAV_TYPE_GROUND_ROVER:
            _systemPixmap += "GroundRover";
            break;
        case MAV_TYPE_SURFACE_BOAT:
            _systemPixmap += "SurfaceBoat";
            break;
        case MAV_TYPE_SUBMARINE:
            _systemPixmap += "Submarine";
            break;
        case MAV_TYPE_HEXAROTOR:
            _systemPixmap += "HexaRotor";
            break;
        case MAV_TYPE_OCTOROTOR:
            _systemPixmap += "OctoRotor";
            break;
        case MAV_TYPE_TRICOPTER:
            _systemPixmap += "TriCopter";
            break;
        case MAV_TYPE_FLAPPING_WING:
            _systemPixmap += "FlappingWing";
            break;
        case MAV_TYPE_KITE:
            _systemPixmap += "Kite";
            break;
        default:
            _systemPixmap += "Unknown";
            break;
    }
    emit systemPixmapChanged();
}

void Vehicle::_heartbeatTimeout(bool timeout, unsigned int ms)
{
    unsigned int elapsed = ms;
    if (!timeout)
    {
        elapsed = 0;
    }
    if(elapsed != _currentHeartbeatTimeout) {
        _currentHeartbeatTimeout = elapsed;
        emit heartbeatTimeoutChanged();
    }
}

void Vehicle::_setSatelliteCount(double val, QString)
{
    // I'm assuming that a negative value or over 99 means there is no GPS
    if(val < 0.0)  val = -1.0;
    if(val > 99.0) val = -1.0;
    if(_satelliteCount != (int)val) {
        _satelliteCount = (int)val;
        emit satelliteCountChanged();
    }
}

void Vehicle::_setSatLoc(UASInterface*, int fix)
{
    // fix 0: lost, 1: at least one satellite, but no GPS fix, 2: 2D lock, 3: 3D lock
    if(_satelliteLock != fix) {
        _satelliteLock = fix;
        emit satelliteLockChanged();
    }
}

void Vehicle::_updateWaypointDistance(double distance)
{
    if (_waypointDistance != distance) {
        _waypointDistance = distance;
        emit waypointDistanceChanged();
    }
}

void Vehicle::_updateCurrentWaypoint(quint16 id)
{
    if (_currentWaypoint != id) {
        _currentWaypoint = id;
        emit currentWaypointChanged();
    }
}

void Vehicle::_updateWaypointViewOnly(int, MissionItem* /*wp*/)
{
    /*
     bool changed = false;
     for(int i = 0; i < _waypoints.count(); i++) {
     if(_waypoints[i].sequenceNumber() == wp->sequenceNumber()) {
     _waypoints[i] = *wp;
     changed = true;
     break;
     }
     }
     if(changed) {
     emit waypointListChanged();
     }
     */
}

void Vehicle::_waypointViewOnlyListChanged()
{
    if(_wpm) {
        const QList<MissionItem*>& newMisionItems = _wpm->getWaypointViewOnlyList();
        _missionItems.clear();
        qCDebug(VehicleLog) << QString("Loading %1 mission items").arg(newMisionItems.count());
        for(int i = 0; i < newMisionItems.count(); i++) {
            MissionItem* itemToCopy = newMisionItems[i];
            MissionItem* item = new MissionItem(*itemToCopy);
            item->setParent(this);
            _missionItems.append(item);
        }
    }
}

void Vehicle::_handleTextMessage(int newCount)
{
    // Reset?
    if(!newCount) {
        _currentMessageCount = 0;
        _currentNormalCount  = 0;
        _currentWarningCount = 0;
        _currentErrorCount   = 0;
        _messageCount        = 0;
        _currentMessageType  = MessageNone;
        emit newMessageCountChanged();
        emit messageTypeChanged();
        emit messageCountChanged();
        return;
    }
    
    UASMessageHandler* pMh = UASMessageHandler::instance();
    Q_ASSERT(pMh);
    MessageType_t type = newCount ? _currentMessageType : MessageNone;
    int errorCount     = _currentErrorCount;
    int warnCount      = _currentWarningCount;
    int normalCount    = _currentNormalCount;
    //-- Add current message counts
    errorCount  += pMh->getErrorCount();
    warnCount   += pMh->getWarningCount();
    normalCount += pMh->getNormalCount();
    //-- See if we have a higher level
    if(errorCount != _currentErrorCount) {
        _currentErrorCount = errorCount;
        type = MessageError;
    }
    if(warnCount != _currentWarningCount) {
        _currentWarningCount = warnCount;
        if(_currentMessageType != MessageError) {
            type = MessageWarning;
        }
    }
    if(normalCount != _currentNormalCount) {
        _currentNormalCount = normalCount;
        if(_currentMessageType != MessageError && _currentMessageType != MessageWarning) {
            type = MessageNormal;
        }
    }
    int count = _currentErrorCount + _currentWarningCount + _currentNormalCount;
    if(count != _currentMessageCount) {
        _currentMessageCount = count;
        // Display current total new messages count
        emit newMessageCountChanged();
    }
    if(type != _currentMessageType) {
        _currentMessageType = type;
        // Update message level
        emit messageTypeChanged();
    }
    // Update message count (all messages)
    if(newCount != _messageCount) {
        _messageCount = newCount;
        emit messageCountChanged();
    }
    QString errMsg = pMh->getLatestError();
    if(errMsg != _latestError) {
        _latestError = errMsg;
        emit latestErrorChanged();
    }
}

void Vehicle::resetMessages()
{
    // Reset Counts
    int count = _currentMessageCount;
    MessageType_t type = _currentMessageType;
    _currentErrorCount   = 0;
    _currentWarningCount = 0;
    _currentNormalCount  = 0;
    _currentMessageCount = 0;
    _currentMessageType = MessageNone;
    if(count != _currentMessageCount) {
        emit newMessageCountChanged();
    }
    if(type != _currentMessageType) {
        emit messageTypeChanged();
    }
}

int Vehicle::manualControlReservedButtonCount(void)
{
    return _firmwarePlugin->manualControlReservedButtonCount();
}

void Vehicle::_loadSettings(void)
{
    QSettings settings;
    
    settings.beginGroup(QString(_settingsGroup).arg(_id));
    
    bool convertOk;
    
    _joystickMode = (JoystickMode_t)settings.value(_joystickModeSettingsKey, JoystickModeRC).toInt(&convertOk);
    if (!convertOk) {
        _joystickMode = JoystickModeRC;
    }
    
    _joystickEnabled = settings.value(_joystickEnabledSettingsKey, false).toBool();
}

void Vehicle::_saveSettings(void)
{
    QSettings settings;
    
    settings.beginGroup(QString(_settingsGroup).arg(_id));
    
    settings.setValue(_joystickModeSettingsKey, _joystickMode);
    settings.setValue(_joystickEnabledSettingsKey, _joystickEnabled);
}

int Vehicle::joystickMode(void)
{
    return _joystickMode;
}

void Vehicle::setJoystickMode(int mode)
{
    if (mode < 0 || mode >= JoystickModeMax) {
        qCWarning(VehicleLog) << "Invalid joystick mode" << mode;
        return;
    }
    
    _joystickMode = (JoystickMode_t)mode;
    _saveSettings();
    emit joystickModeChanged(mode);
}

QStringList Vehicle::joystickModes(void)
{
    QStringList list;
    
    list << "Normal" << "Attitude" << "Position" << "Force" << "Velocity";
    
    return list;
}

bool Vehicle::joystickEnabled(void)
{
    return _joystickEnabled;
}

void Vehicle::setJoystickEnabled(bool enabled)
{
    Fact* fact = _autopilotPlugin->getParameterFact(FactSystem::defaultComponentId, "COM_RC_IN_MODE");
    if (!fact) {
        qCWarning(JoystickLog) << "Missing COM_RC_IN_MODE parameter";
    }
    
    if (fact->value().toInt() != 2) {
        fact->setValue(enabled ? 1 : 0);
    }
    
    _joystickEnabled = enabled;
    _startJoystick(_joystickEnabled);
    _saveSettings();
}

void Vehicle::_startJoystick(bool start)
{
#ifndef __mobile__
    Joystick* joystick = JoystickManager::instance()->activeJoystick();
    if (joystick) {
        if (start) {
            if (_joystickEnabled) {
                joystick->startPolling(this);
            }
        } else {
            joystick->stopPolling();
        }
    }
#else
    Q_UNUSED(start);
#endif
}

bool Vehicle::active(void)
{
    return _active;
}

void Vehicle::setActive(bool active)
{
    _active = active;
    
    _startJoystick(_active);
}

QmlObjectListModel* Vehicle::missionItemsModel(void)
{
    if (qgcApp()->useNewMissionEditor()) {
        return missionManager()->missionItems();
    } else {
        return &_missionItems;
    }
}
