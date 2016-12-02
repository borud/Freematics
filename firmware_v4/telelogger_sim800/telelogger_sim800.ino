/******************************************************************************
* Reference sketch for a vehicle data feed for Freematics Hub
* Works with Freematics ONE with SIM800 GSM/GPRS module
* Developed by Stanley Huang https://www.facebook.com/stanleyhuangyc
* Distributed under BSD license
* Visit http://freematics.com/hub/api for Freematics Hub API reference
* Visit http://freematics.com/products/freematics-one for hardware information
* To obtain your Freematics Hub server key, contact support@freematics.com.au
* 
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
******************************************************************************/

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <FreematicsONE.h>
#include "config.h"
#include "datalogger.h"

// logger states
#define STATE_SD_READY 0x1
#define STATE_OBD_READY 0x2
#define STATE_GPS_READY 0x4
#define STATE_MEMS_READY 0x8
#define STATE_GSM_READY 0x10
#define STATE_SLEEPING 0x20
#define STATE_CONNECTED 0x40

uint16_t lastUTC = 0;
uint8_t lastGPSDay = 0;
uint32_t nextConnTime = 0;
uint16_t connCount = 0;
byte accCount = 0; // count of accelerometer readings
long accSum[3] = {0}; // sum of accelerometer data
int accCal[3]; // calibrated reference accelerometer data
byte deviceTemp = 0; // device temperature
int lastSpeed = 0;
uint32_t lastSpeedTime = 0;
uint32_t distance = 0;

typedef enum {
    GPRS_DISABLED = 0,
    GPRS_IDLE,
    GPRS_HTTP_RECEIVING,
    GPRS_HTTP_ERROR,
} GPRS_STATES;

typedef enum {
  HTTP_GET = 0,
  HTTP_POST,
} HTTP_METHOD;

typedef struct {
  float lat;
  float lng;
  uint8_t year; /* year past 2000, e.g. 15 for 2015 */
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
} GSM_LOCATION;

class COBDGSM : public COBDSPI {
public:
    COBDGSM():gprsState(GPRS_DISABLED),connErrors(0) { buffer[0] = 0; }
    void toggleGSM()
    {
        setTarget(TARGET_OBD);
        sendCommand("ATGSMPWR\r", buffer, sizeof(buffer));
    }
    bool initGSM()
    {
      // init xBee module serial communication
      xbBegin();
      // discard any stale data
      xbPurge();
      for (;;) {
        // try turning on GSM
        toggleGSM();
        delay(2000);
        if (sendGSMCommand("ATE0\r") != 0) {
          break;
        }
      }
      //sendGSMCommand("ATE0\r");
    }
    bool setupGPRS(const char* apn)
    {
      uint32_t t = millis();
      bool success = false;
      do {
        success = sendGSMCommand("AT+CREG?\r", 5000, "+CREG: 0,") != 0;
        Serial.print('.'); 
      } while (!success && millis() - t < MAX_CONN_TIME);
      sendGSMCommand("AT+CGATT?\r");
      sendGSMCommand("AT+SAPBR=3,1,\"Contype\",\"GPRS\"\r");
      sprintf_P(buffer, PSTR("AT+SAPBR=3,1,\"APN\",\"%s\"\r"), apn);
      sendGSMCommand(buffer, 15000);
      do {
        sendGSMCommand("AT+SAPBR=1,1\r", 5000);
        sendGSMCommand("AT+SAPBR=2,1\r", 5000);
        success = !strstr_P(buffer, PSTR("0.0.0.0")) && !strstr_P(buffer, PSTR("ERROR"));
      } while (!success && millis() - t < MAX_CONN_TIME);
      return success;
    }
    int getSignal()
    {
        if (sendGSMCommand("AT+CSQ\r", 500)) {
            char *p = strchr(buffer, ':');
            if (p) {
              p += 2;
              int db = atoi(p) * 10;
              p = strchr(p, '.');
              if (p) db += *(p + 1) - '0';
              return db;
            }
        }
        return -1;
    }
    bool getOperatorName()
    {
        // display operator name
        if (sendGSMCommand("AT+COPS?\r") == 1) {
            char *p = strstr(buffer, ",\"");
            if (p) {
                p += 2;
                char *s = strchr(p, '\"');
                if (s) *s = 0;
                strcpy(buffer, p);
                return true;
            }
        }
        return false;
    }
    void httpUninit()
    {
      sendGSMCommand("AT+HTTPTERM\r");
    }
    bool httpInit()
    {
      if (!sendGSMCommand("AT+HTTPINIT\r", 3000) || !sendGSMCommand("AT+HTTPPARA=\"CID\",1\r", 3000)) {
        gprsState = GPRS_DISABLED;
        return false;
      }
      gprsState = GPRS_IDLE;
      return true;
    }
    void httpConnect(HTTP_METHOD method)
    {
        // 0 for GET, 1 for POST
        char cmd[17];
        sprintf_P(cmd, PSTR("AT+HTTPACTION=%c\r"), '0' + method);
        setTarget(TARGET_BEE);
        write(cmd);
        gprsState = GPRS_HTTP_RECEIVING;
        bytesRecv = 0;
        checkTimer = millis();
    }
    bool httpIsConnected()
    {
        // may check for "ACTION: 0" for GET and "ACTION: 1" for POST
        byte ret = checkbuffer("ACTION:", MAX_CONN_TIME);
        if (ret == 1) {
          // success
          connErrors = 0;
          return true;
        } else if (ret == 2) {
          // timeout
          gprsState = GPRS_HTTP_ERROR;
          connErrors++;
        }
        return false;
    }
    bool httpRead()
    {
        if (sendGSMCommand("AT+HTTPREAD\r", MAX_CONN_TIME) && strstr(buffer, "+HTTPREAD:")) {
          gprsState = GPRS_IDLE;
          return true;
        } else {
          Serial.println("READ ERROR");
          gprsState = GPRS_HTTP_ERROR;
          return false;
        }
    }
    bool getLocation(GSM_LOCATION* loc)
    {
      if (sendGSMCommand("AT+CIPGSMLOC=1,1\r", 3000)) do {
        char *p;
        if (!(p = strchr(buffer, ':'))) break;
        if (!(p = strchr(p, ','))) break;
        loc->lng = atof(++p);
        if (!(p = strchr(p, ','))) break;
        loc->lat = atof(++p);
        if (!(p = strchr(p, ','))) break;
        loc->year = atoi(++p) - 2000;
        if (!(p = strchr(p, '/'))) break;
        loc->month = atoi(++p);
        if (!(p = strchr(p, '/'))) break;
        loc->day = atoi(++p);
        if (!(p = strchr(p, ','))) break;
        loc->hour = atoi(++p);
        if (!(p = strchr(p, ':'))) break;
        loc->minute = atoi(++p);
        if (!(p = strchr(p, ':'))) break;
        loc->second = atoi(++p);
        return true;
      } while(0);
      return false;
    }
    byte checkbuffer(const char* expected, unsigned int timeout = 2000)
    {
      byte ret = xbReceive(buffer, sizeof(buffer), 0, expected) != 0;
      if (ret == 0) {
        // timeout
        return (millis() - checkTimer < timeout) ? 0 : 2;
      } else {
        return ret;
      }
    }
    bool sendGSMCommand(const char* cmd, unsigned int timeout = 2000, const char* expected = "OK")
    {
      xbWrite(cmd);
      delay(10);
      return xbReceive(buffer, sizeof(buffer), timeout, expected) != 0;
    }
    bool setPostPayload(const char* payload, int bytes)
    {
        // set HTTP POST payload data
        char cmd[24];
        sprintf_P(cmd, PSTR("AT+HTTPDATA=%d,1000\r"), bytes);
        if (!sendGSMCommand(cmd, 1000, "DOWNLOAD")) {
          return false;
        }
        // send cached data
        return sendGSMCommand(payload, 1000);
    }
    char buffer[128];
    byte bytesRecv;
    uint32_t checkTimer;
    byte gprsState;
    byte connErrors;
};

class CTeleLogger : public COBDGSM, public CDataLogger
#if USE_MPU6050
,public CMPU6050
#elif USE_MPU9250
,public CMPU9250
#endif
{
public:
    CTeleLogger():state(0),feedid(0) {}
    void setup()
    {
        // this will init SPI communication
        begin();
 
#if USE_MPU6050 || USE_MPU9250
        // start I2C communication 
        Wire.begin();
#if USE_MPU6050
        Serial.print("#MPU6050...");
#else
        Serial.print("#MPU9250...");
#endif
        if (memsInit()) {
          state |= STATE_MEMS_READY;
          Serial.println("OK");
        } else {
          Serial.println("NO");
        }
#endif

        // initialize OBD communication
        Serial.print("#OBD..");
        for (uint32_t t = millis(); millis() - t < OBD_INIT_TIMEOUT; ) {
            Serial.print('.');
            if (init()) {
              state |= STATE_OBD_READY;
              break;              
            }
        }
        if (state & STATE_OBD_READY) {
          Serial.println("OK");
        } else {
          Serial.println("NO"); 
          calibrateMEMS();
          reconnect();
          reboot();
        }

#if USE_GPS
        // start serial communication with GPS receive
        Serial.print("#GPS...");
        if (initGPS(GPS_SERIAL_BAUDRATE)) {
          state |= STATE_GPS_READY;
          Serial.println("OK");
        } else {
          Serial.println("NO");
        }
#endif

        // initialize SIM800L xBee module (if present)
        Serial.print("#GSM...");
        xbBegin(XBEE_BAUDRATE);
        if (initGSM()) {
          state |= STATE_GSM_READY;
          Serial.println("OK");
        } else {
          Serial.println(buffer);
          standby();
          reboot();
        }

        Serial.print("#GPRS(APN:");
        Serial.print(APN);
        Serial.print(")...");
        if (setupGPRS(APN)) {
          Serial.println("OK");
        } else {
          Serial.println(buffer);
          standby();
          reboot();
        }
        
        // init HTTP
        Serial.print("#HTTP...");
        while (!httpInit()) {
          Serial.print('.');
          httpUninit();
          delay(3000);
          if (readSpeed() == -1) {
            reconnect();
            reboot();
          }
        }
        Serial.println("OK");

        // sign in server, will block if not successful
        regDataFeed(0);
        state |= STATE_CONNECTED;

        calibrateMEMS();
    }
    void calibrateMEMS()
    {
        // get accelerometer calibration reference data
        accCal[0] = accSum[0] / accCount;
        accCal[1] = accSum[1] / accCount;
        accCal[2] = accSum[2] / accCount;
        Serial.print("#ACC:");
        Serial.print(accCal[0]);
        Serial.print('/');
        Serial.print(accCal[1]);
        Serial.print('/');
        Serial.println(accCal[2]);
    }
    void regDataFeed(byte action)
    {
      // action == 0 for registering a data feed, action == 1 for de-registering a data feed

      char vin[20] = {0};
      int signal = 0;
      if (action == 0) {
        // retrieve VIN
        if (getVIN(buffer, sizeof(buffer))) {
          strncpy(vin, buffer, sizeof(vin) - 1);
          Serial.print("#VIN:");
          Serial.println(vin);
        }
        Serial.print("#SIGNAL:");
        signal = getSignal();
        Serial.println(signal);
      } else {
        if (feedid == 0) return; 
      }
      
      gprsState = GPRS_IDLE;
      for (byte n = 0; ;n++) {
        if (action == 0) {
          // error handling
          if (readSpeed() == -1 || n >= MAX_CONN_ERRORS) {
            standby();
            reboot();
          }
        } else {
          if (n >= MAX_CONN_ERRORS) {
            return;
          } 
        }
        char *p = buffer;
        p += sprintf_P(buffer, PSTR("AT+HTTPPARA=\"URL\",\"%s/reg?"), SERVER_URL);
        if (action == 0) {
          Serial.print("#SERVER");
          sprintf_P(p, PSTR("CSQ=%d&vin=%s\"\r"), signal, vin);
        } else {
          sprintf_P(p, PSTR("id=%u&off=1\"\r"), feedid);
        }
        if (!sendGSMCommand(buffer)) {
          Serial.println(buffer);
          delay(3000);
          continue;
        }
        httpConnect(HTTP_GET);
        do {
          delay(500);
          Serial.print('.');
        } while (!httpIsConnected());
        if (action != 0) {
          Serial.println(); 
          return;
        }
        if (gprsState != GPRS_HTTP_ERROR && httpRead()) {
          char *p = strstr(buffer, "\"id\":");
          if (p) {
            int m = atoi(p + 5);
            if (m > 0) {
              feedid = m;
              Serial.println(m);
              state |= STATE_CONNECTED;
              break;
            }
          }            
        }
        Serial.println(buffer);
        delay(5000);
      }
    }
    void loop()
    {
        uint32_t start = millis();

        if (state & STATE_OBD_READY) {
          processOBD();
        }

#if USE_MPU6050 || USE_MPU9250
        if (state & STATE_MEMS_READY) {
            processMEMS();  
        }
#endif

        if (state & STATE_GPS_READY) {
#if USE_GPS
          processGPS();
#endif
#if USE_GSM_LOCATION
        } else {
          processGSMLocation();
#endif
        }

        // read and log car battery voltage , data in 0.01v
        int v = getVoltage() * 100;
        dataTime = millis();
        logData(PID_BATTERY_VOLTAGE, v);

        do {
          if (millis() > nextConnTime) {
#if USE_GSM_LOCATION
            if (!(state & STATE_GPS_READY)) {
              if (gprsState == GPRS_IDLE) {
                // get GSM location if GPS not present
                if (getLocation(&loc)) {
                  //Serial.print(buffer);
                }
              }
            }
#endif
            // process HTTP state machine
            processGPRS();

            // continously read speed for calculating trip distance
            if (state & STATE_OBD_READY) {
               readSpeed();
            }
            // error and exception handling
            if (gprsState == GPRS_IDLE) {
              if (errors > 10) {
                reconnect();
                reboot();
              } else if (deviceTemp >= COOLING_DOWN_TEMP) {
                // device too hot, slow down communication a bit
                Serial.println("Cool down");
                delay(3000);
              }
            }          
          }
        } while (millis() - start < MIN_LOOP_TIME);

        // error handling
        if (connErrors >= MAX_CONN_ERRORS) {
          // reset GPRS 
          Serial.print(connErrors);
          Serial.println("Reset GPRS...");
          initGSM();
          setupGPRS(APN);
          if (httpInit()) {
            Serial.println("OK"); 
          } else {
            Serial.println(buffer); 
          }
          connErrors = 0;
        }
    }
private:
    void processGPRS()
    {
        switch (gprsState) {
        case GPRS_IDLE:
            if (state & STATE_CONNECTED) {
#if !ENABLE_DATA_CACHE
                char cache[16];
                int v = getVoltage() * 100;
                byte cacheBytes = sprintf(cache, "#%lu,%u,%d ", millis(), PID_BATTERY_VOLTAGE, v);
                Serial.println(v);
#endif
                // generate URL
                sprintf_P(buffer, PSTR("AT+HTTPPARA=\"URL\",\"%s/post?id=%u\"\r"), SERVER_URL, feedid);
                if (!sendGSMCommand(buffer)) {
                  break;
                }
                // replacing last space with
                cache[cacheBytes - 1] = '\r';
                if (setPostPayload(cache, cacheBytes - 1)) {
                  // success
                  Serial.print(cacheBytes - 1);
                  Serial.println(" bytes");
                  // output payload data to serial
                  //Serial.println(cache);               
#if ENABLE_DATA_CACHE
                  purgeCache();
#endif
                  delay(50);
                  Serial.println(deviceTemp);
                  Serial.print("Sending #");
                  Serial.print(++connCount);
                  Serial.print("...");
                  httpConnect(HTTP_POST);
                  nextConnTime = millis() + 500;
                } else {
                  Serial.println(buffer);
                  gprsState = GPRS_HTTP_ERROR;
                }
            }
            break;        
        case GPRS_HTTP_RECEIVING:
            if (httpIsConnected()) {
                if (httpRead()) {
                  // success
                  Serial.println("OK");
                  //Serial.println(buffer);
                  break;
                }
            }
            nextConnTime = millis() + 200; 
            break;
        case GPRS_HTTP_ERROR:
            Serial.println("HTTP error");
            Serial.println(buffer);
            connCount = 0;
            xbPurge();
            httpUninit();
            delay(500);
            httpInit();
            gprsState = GPRS_IDLE;
            nextConnTime = millis() + 500;
            break;
        }
    }
    void processOBD()
    {
        int speed = readSpeed();
        if (speed == -1) {
          return;
        }
        logData(0x100 | PID_SPEED, speed);
        logData(PID_TRIP_DISTANCE, distance);
        // poll more PIDs
        byte pids[]= {0, PID_RPM, PID_ENGINE_LOAD, PID_THROTTLE};
        const byte pidTier2[] = {PID_INTAKE_TEMP, PID_COOLANT_TEMP};
        static byte index2 = 0;
        int values[sizeof(pids)] = {0};
        // read multiple OBD-II PIDs, tier2 PIDs are less frequently read
        pids[0] = pidTier2[index2 = (index2 + 1) % sizeof(pidTier2)];
        byte results = readPID(pids, sizeof(pids), values);
        if (results == sizeof(pids)) {
          for (byte n = 0; n < sizeof(pids); n++) {
            logData(0x100 | pids[n], values[n]);
          }
        }
    }
    int readSpeed()
    {
        int value;
        if (readPID(PID_SPEED, value)) {
           dataTime = millis();
           distance += (value + lastSpeed) * (dataTime - lastSpeedTime) / 3600 / 2;
           lastSpeedTime = dataTime;
           lastSpeed = value;
           return value;
        } else {
          return -1; 
        }
    }
#if USE_MPU6050 || USE_MPU9250
    void processMEMS()
    {
         // log the loaded MEMS data
        if (accCount) {
          logData(PID_ACC, accSum[0] / accCount / ACC_DATA_RATIO, accSum[1] / accCount / ACC_DATA_RATIO, accSum[2] / accCount / ACC_DATA_RATIO);
          accSum[0] = 0;
          accSum[1] = 0;
          accSum[2] = 0;
          accCount = 0;
        }
    }
#endif
    void processGPS()
    {
        GPS_DATA gd = {0};
        // read parsed GPS data
        if (getGPSData(&gd)) {
            if (lastUTC != (uint16_t)gd.time) {
              dataTime = millis();
              byte day = gd.date / 10000;
              logData(PID_GPS_TIME, gd.time);
              if (lastGPSDay != day) {
                logData(PID_GPS_DATE, gd.date);
                lastGPSDay = day;
              }
              logCoordinate(PID_GPS_LATITUDE, gd.lat);
              logCoordinate(PID_GPS_LONGITUDE, gd.lng);
              logData(PID_GPS_ALTITUDE, gd.alt);
              logData(PID_GPS_SPEED, gd.speed);
              logData(PID_GPS_SAT_COUNT, gd.sat);
              lastUTC = (uint16_t)gd.time;
            }
            //Serial.print("#UTC:"); 
            //Serial.println(gd.time);
        } else {
          Serial.println("No GPS data");
          xbPurge();
        }
    }
    void processGSMLocation()
    {
        uint32_t t = (uint32_t)loc.hour * 1000000 + (uint32_t)loc.minute * 10000 + (uint32_t)loc.second * 100;
        if (lastUTC != (uint16_t)t) {
          dataTime = millis();
          logData(PID_GPS_TIME, t);
          if (lastGPSDay != loc.day) {
            logData(PID_GPS_DATE, (uint32_t)loc.day * 10000 + (uint32_t)loc.month * 100 + loc.year);
            lastGPSDay = loc.day;
          }
          logCoordinate(PID_GPS_LATITUDE, loc.lat * 1000000);
          logCoordinate(PID_GPS_LONGITUDE, loc.lng * 1000000);
          lastUTC = (uint16_t)t;
        }
    }
    void reconnect()
    {
        // try to re-connect to OBD
        for (byte n = 0; n < 3; n++) {
          if (init()) {
            // reconnected
            return; 
          }
          delay(1000);
        }
        standby();
    }
    void standby()
    {
        if (state & STATE_GSM_READY) {
          regDataFeed(1); // de-register
          toggleGSM(); // turn off GSM power
        }
#if USE_GPS
        initGPS(0); // turn off GPS power
#endif
        state &= ~(STATE_OBD_READY | STATE_GPS_READY | STATE_GSM_READY | STATE_CONNECTED);
        state |= STATE_SLEEPING;
        // put OBD chips into low power mode
        Serial.println("Standby");
        enterLowPowerMode();
        for (;;) {            
          accSum[0] = 0;
          accSum[1] = 0;
          accSum[2] = 0;
          for (accCount = 0; accCount < 10; ) {
            dataIdleLoop();
            delay(50);
          }
          // calculate relative movement
          unsigned long motion = 0;
          for (byte i = 0; i < 3; i++) {
            long n = accSum[i] / accCount - accCal[i];
            motion += n * n;
          }
          // check movement
          //Serial.println(motion);
          if (motion > START_MOTION_THRESHOLD) {
            Serial.println("Check OBD");
            // try OBD reading
            leaveLowPowerMode();
            if (readSpeed() != -1) {
              // a successful readout
              break;
            }
            enterLowPowerMode();
            // calibrate MEMS again in case the device posture changed
            calibrateMEMS();
          }
        }
        // we are able to get OBD data again
        state &= ~STATE_SLEEPING;
    }
    void reboot()
    {
        // reset device
        void(* resetFunc) (void) = 0; //declare reset function at address 0
        resetFunc();
    }
#if USE_MPU6050 || USE_MPU9250
    void dataIdleLoop()
    {
      // do something while waiting for data on SPI
      if (state & STATE_MEMS_READY) {
        // load accelerometer and temperature data
        int acc[3] = {0};
        int temp; // device temperature (in 0.1 celcius degree)
        memsRead(acc, 0, 0, &temp);
        if (accCount >= 250) {
          accSum[0] >>= 1;
          accSum[1] >>= 1;
          accSum[2] >>= 1;
          accCount >>= 1;
        }
        accSum[0] += acc[0];
        accSum[1] += acc[1];
        accSum[2] += acc[2];
        accCount++;
        deviceTemp = temp / 10;
      }
      delay(10);
    }
#endif
    byte state;
    uint16_t feedid;
    GSM_LOCATION loc;
};

CTeleLogger logger;

void setup()
{
    // initialize hardware serial (for USB or BLE)
    Serial.begin(115200);
    delay(500);
    // perform initializations
    logger.setup();
}

void loop()
{
    logger.loop();
}
