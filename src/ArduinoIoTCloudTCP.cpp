/*
   This file is part of ArduinoIoTCloud.

   Copyright 2019 ARDUINO SA (http://www.arduino.cc/)

   This software is released under the GNU General Public License version 3,
   which covers the main part of arduino-cli.
   The terms of this license can be found at:
   https://www.gnu.org/licenses/gpl-3.0.en.html

   You can be released from the requirements of the above licenses by purchasing
   a commercial license. Buying such a license is mandatory if you want to modify or
   otherwise use the software for commercial activities involving the Arduino
   software without disclosing the source code of your own applications. To purchase
   a commercial license, send an email to license@arduino.cc.
*/

/******************************************************************************
 * INCLUDE
 ******************************************************************************/

#include "ArduinoIoTCloud_Defines.h"

#ifdef HAS_TCP
#include <ArduinoIoTCloudTCP.h>
#include "utility/time/TimeService.h"
#ifdef BOARD_HAS_ECCX08
  #include <ArduinoECCX08.h>
  #include "utility/crypto/CryptoUtil.h"
  #include "utility/crypto/BearSSLTrustAnchor.h"
#endif

/******************************************************************************
   GLOBAL VARIABLES
 ******************************************************************************/

TimeService time_service;

/******************************************************************************
   GLOBAL CONSTANTS
 ******************************************************************************/

const static int CONNECT_SUCCESS							               = 1;
const static int CONNECT_FAILURE							               = 0;
const static int CONNECT_FAILURE_SUBSCRIBE					         = -1;

/******************************************************************************
   LOCAL MODULE FUNCTIONS
 ******************************************************************************/

static unsigned long getTime() {
  return time_service.getTime();
}

/******************************************************************************
   CTOR/DTOR
 ******************************************************************************/

ArduinoIoTCloudTCP::ArduinoIoTCloudTCP():
  _connection(NULL),
  _lastSyncRequestTickTime{0},
  _mqtt_data_buf{0},
  _mqtt_data_len{0},
  _mqtt_data_request_retransmit{false},
  _sslClient(NULL),
  #ifdef BOARD_ESP
  _password(""),
  #endif
  _mqttClient(NULL),
  _syncStatus{ArduinoIoTSynchronizationStatus::SYNC_STATUS_SYNCHRONIZED},
  _stdinTopic(""),
  _stdoutTopic(""),
  _shadowTopicOut(""),
  _shadowTopicIn(""),
  _dataTopicOut(""),
  _dataTopicIn("")
{}

ArduinoIoTCloudTCP::~ArduinoIoTCloudTCP() {
  delete _mqttClient; _mqttClient = NULL;
  delete _sslClient;  _sslClient = NULL;
}

/******************************************************************************
 * PUBLIC MEMBER FUNCTIONS
 ******************************************************************************/

int ArduinoIoTCloudTCP::begin(ConnectionHandler & connection, String brokerAddress, uint16_t brokerPort) {
  _connection = &connection;
  _brokerAddress = brokerAddress;
  _brokerPort = brokerPort;
  time_service.begin(&connection);
  return begin(_brokerAddress, _brokerPort);
}

int ArduinoIoTCloudTCP::begin(String brokerAddress, uint16_t brokerPort) {

  _brokerAddress = brokerAddress;
  _brokerPort = brokerPort;

  #ifdef BOARD_HAS_ECCX08
  if (!ECCX08.begin())                                                                                                                                                         { Debug.print(DBG_ERROR, "Cryptography processor failure. Make sure you have a compatible board."); return 0; }
  if (!CryptoUtil::readDeviceId(ECCX08, getDeviceId(), ECCX08Slot::DeviceId))                                                                                                  { Debug.print(DBG_ERROR, "Cryptography processor read failure."); return 0; }
  if (!CryptoUtil::reconstructCertificate(_eccx08_cert, getDeviceId(), ECCX08Slot::Key, ECCX08Slot::CompressedCertificate, ECCX08Slot::SerialNumberAndAuthorityKeyIdentifier)) { Debug.print(DBG_ERROR, "Cryptography certificate reconstruction failure."); return 0; }
  ArduinoBearSSL.onGetTime(getTime);
  _sslClient = new BearSSLClient(_connection->getClient(), ArduinoIoTCloudTrustAnchor, ArduinoIoTCloudTrustAnchor_NUM);
  _sslClient->setEccSlot(static_cast<int>(ECCX08Slot::Key), _eccx08_cert.bytes(), _eccx08_cert.length());
  #elif defined(BOARD_ESP)
  _sslClient = new WiFiClientSecure();
  _sslClient->setInsecure();
  #endif

  _mqttClient = new MqttClient(*_sslClient);
  #ifdef BOARD_ESP
  _mqttClient->setUsernamePassword(getDeviceId(), _password);
  #endif
  _mqttClient->onMessage(ArduinoIoTCloudTCP::onMessage);
  _mqttClient->setKeepAliveInterval(30 * 1000);
  _mqttClient->setConnectionTimeout(1500);
  _mqttClient->setId(getDeviceId().c_str());

  _stdinTopic     = getTopic_stdin();
  _stdoutTopic    = getTopic_stdout();
  _shadowTopicOut = getTopic_shadowout();
  _shadowTopicIn  = getTopic_shadowin();
  _dataTopicOut   = getTopic_dataout();
  _dataTopicIn    = getTopic_datain();

  _thing.begin();
  _thing.registerGetTimeCallbackFunc(getTime);
  return 1;
}

int ArduinoIoTCloudTCP::connect() {

  if (!_mqttClient->connect(_brokerAddress.c_str(), _brokerPort)) {
    return CONNECT_FAILURE;
  }
  if (_mqttClient->subscribe(_stdinTopic) == 0) {
    return CONNECT_FAILURE_SUBSCRIBE;
  }
  if (_mqttClient->subscribe(_dataTopicIn) == 0) {
    return CONNECT_FAILURE_SUBSCRIBE;
  }
  if (_shadowTopicIn != "") {
    if (_mqttClient->subscribe(_shadowTopicIn) == 0) {
      return CONNECT_FAILURE_SUBSCRIBE;
    }
    _syncStatus = ArduinoIoTSynchronizationStatus::SYNC_STATUS_WAIT_FOR_CLOUD_VALUES;
    _lastSyncRequestTickTime = 0;
  }

  return CONNECT_SUCCESS;
}


bool ArduinoIoTCloudTCP::disconnect() {
  _mqttClient->stop();

  return true;
}

void ArduinoIoTCloudTCP::update() {
  // Check if a primitive property wrapper is locally changed
  _thing.updateTimestampOnLocallyChangedProperties();

  if(checkPhyConnection()   != NetworkConnectionState::CONNECTED)     return;
  if(checkCloudConnection() != ArduinoIoTConnectionStatus::CONNECTED) return;

  if(_mqtt_data_request_retransmit && (_mqtt_data_len > 0)) {
    write(_dataTopicOut, _mqtt_data_buf, _mqtt_data_len);
    _mqtt_data_request_retransmit = false;
  }

  // MTTQClient connected!, poll() used to retrieve data from MQTT broker
  _mqttClient->poll();

  switch (_syncStatus) {
    case ArduinoIoTSynchronizationStatus::SYNC_STATUS_SYNCHRONIZED: {
        sendPropertiesToCloud();
      }
      break;
    case ArduinoIoTSynchronizationStatus::SYNC_STATUS_WAIT_FOR_CLOUD_VALUES: {
        if (millis() - _lastSyncRequestTickTime > TIMEOUT_FOR_LASTVALUES_SYNC) {
          requestLastValue();
          _lastSyncRequestTickTime = millis();
        }
      }
      break;
    case ArduinoIoTSynchronizationStatus::SYNC_STATUS_VALUES_PROCESSED: {
        execCloudEventCallback(_on_sync_event_callback);
        _syncStatus = ArduinoIoTSynchronizationStatus::SYNC_STATUS_SYNCHRONIZED;
      }
      break;
  }
}

void ArduinoIoTCloudTCP::printDebugInfo() {
  Debug.print(DBG_INFO, "***** Arduino IoT Cloud - configuration info *****");
  Debug.print(DBG_INFO, "Device ID: %s", getDeviceId().c_str());
  Debug.print(DBG_INFO, "Thing ID: %s", getThingId().c_str());
  Debug.print(DBG_INFO, "MQTT Broker: %s:%d", _brokerAddress.c_str(), _brokerPort);
}

int ArduinoIoTCloudTCP::reconnect() {
  if (_mqttClient->connected()) {
    _mqttClient->stop();
  }

  // Connect to the broker
  return connect();
}


int ArduinoIoTCloudTCP::connected() {
  return _mqttClient->connected();
}

/******************************************************************************
 * PRIVATE MEMBER FUNCTIONS
 ******************************************************************************/

void ArduinoIoTCloudTCP::onMessage(int length) {
  ArduinoCloud.handleMessage(length);
}

void ArduinoIoTCloudTCP::handleMessage(int length) {
  String topic = _mqttClient->messageTopic();

  byte bytes[length];

  for (int i = 0; i < length; i++) {
    bytes[i] = _mqttClient->read();
  }

  if (_stdinTopic == topic) {
    CloudSerial.appendStdin((uint8_t*)bytes, length);
  }
  if (_dataTopicIn == topic) {
    _thing.decode((uint8_t*)bytes, length);
  }
  if ((_shadowTopicIn == topic) && _syncStatus == ArduinoIoTSynchronizationStatus::SYNC_STATUS_WAIT_FOR_CLOUD_VALUES) {
    _thing.decode((uint8_t*)bytes, length, true);
    sendPropertiesToCloud();
    _syncStatus = ArduinoIoTSynchronizationStatus::SYNC_STATUS_VALUES_PROCESSED;
  }
}

void ArduinoIoTCloudTCP::sendPropertiesToCloud() {
  uint8_t data[MQTT_TRANSMIT_BUFFER_SIZE];
  int const length = _thing.encode(data, sizeof(data));
  if (length > 0)
  {
    /* If properties have been encoded store them in the back-up buffer
     * in order to allow retransmission in case of failure.
     */
    _mqtt_data_len = length;
    memcpy(_mqtt_data_buf, data, _mqtt_data_len);
    /* Transmit the properties to the MQTT broker */
    write(_dataTopicOut, _mqtt_data_buf, _mqtt_data_len);
  }
}

void ArduinoIoTCloudTCP::requestLastValue() {
  // Send the getLastValues CBOR message to the cloud
  // [{0: "r:m", 3: "getLastValues"}] = 81 A2 00 63 72 3A 6D 03 6D 67 65 74 4C 61 73 74 56 61 6C 75 65 73
  // Use http://cbor.me to easily generate CBOR encoding
  const uint8_t CBOR_REQUEST_LAST_VALUE_MSG[] = { 0x81, 0xA2, 0x00, 0x63, 0x72, 0x3A, 0x6D, 0x03, 0x6D, 0x67, 0x65, 0x74, 0x4C, 0x61, 0x73, 0x74, 0x56, 0x61, 0x6C, 0x75, 0x65, 0x73 };
  write(_shadowTopicOut, CBOR_REQUEST_LAST_VALUE_MSG, sizeof(CBOR_REQUEST_LAST_VALUE_MSG));
}

NetworkConnectionState ArduinoIoTCloudTCP::checkPhyConnection()
{
  NetworkConnectionState const connect_state = _connection->check();

  if (_connection->check() != NetworkConnectionState::CONNECTED)
  {
    if (_iotStatus == ArduinoIoTConnectionStatus::CONNECTED)
    {
      disconnect();
    }
  }

  return connect_state;
}

ArduinoIoTConnectionStatus ArduinoIoTCloudTCP::checkCloudConnection()
{
  switch (_iotStatus) {
    case ArduinoIoTConnectionStatus::IDLE: {
        _iotStatus = ArduinoIoTConnectionStatus::CONNECTING;
        printConnectionStatus(_iotStatus);
      }
      break;
    case ArduinoIoTConnectionStatus::ERROR: {
        _iotStatus = ArduinoIoTConnectionStatus::RECONNECTING;
        printConnectionStatus(_iotStatus);
      }
      break;
    case ArduinoIoTConnectionStatus::CONNECTED: {
        if (!_mqttClient->connected()) {
          _iotStatus = ArduinoIoTConnectionStatus::DISCONNECTED;
          _mqtt_data_request_retransmit = true;
          printConnectionStatus(_iotStatus);
          execCloudEventCallback(_on_disconnect_event_callback);
        }
      }
      break;
    case ArduinoIoTConnectionStatus::DISCONNECTED: {
        _iotStatus = ArduinoIoTConnectionStatus::RECONNECTING;
        printConnectionStatus(_iotStatus);
      }
      break;
    case ArduinoIoTConnectionStatus::RECONNECTING: {
        int const ret_code_reconnect = reconnect();
        Debug.print(DBG_INFO, "ArduinoCloud.reconnect(): %d", ret_code_reconnect);
        if (ret_code_reconnect == CONNECT_SUCCESS) {
          _iotStatus = ArduinoIoTConnectionStatus::CONNECTED;
          printConnectionStatus(_iotStatus);
          execCloudEventCallback(_on_connect_event_callback);
          CloudSerial.begin(9600);
          CloudSerial.println("Hello from Cloud Serial!");
        }
      }
      break;
    case ArduinoIoTConnectionStatus::CONNECTING: {
        int const ret_code_connect = connect();
        Debug.print(DBG_VERBOSE, "ArduinoCloud.connect(): %d", ret_code_connect);
        if (ret_code_connect == CONNECT_SUCCESS) {
          _iotStatus = ArduinoIoTConnectionStatus::CONNECTED;
          printConnectionStatus(_iotStatus);
          execCloudEventCallback(_on_connect_event_callback);
          CloudSerial.begin(9600);
          CloudSerial.println("Hello from Cloud Serial!");
        } else if (ret_code_connect == CONNECT_FAILURE_SUBSCRIBE) {
          Debug.print(DBG_INFO, "ERROR - Please verify your THING ID");
        }
      }
      break;
  }

  return _iotStatus;
}

int ArduinoIoTCloudTCP::write(String const topic, byte const data[], int const length)
{
  if (_mqttClient->beginMessage(topic, length, false, 0)) {
    if (_mqttClient->write(data, length)) {
      if (_mqttClient->endMessage()) {
        return 1;
      }
    }
  }
  return 0;
}

/******************************************************************************
 * EXTERN DEFINITION
 ******************************************************************************/

ArduinoIoTCloudTCP ArduinoCloud;

#endif