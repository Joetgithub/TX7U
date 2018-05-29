//Note: See large comment section at bottom of the code for an overview. 
//      Main engine is from https://github.com/eiannone/WS8610Receiver.  
//      Project: https://www.hackster.io/JoeTio/capture-of-wireless-lacrosse-433mhz-tx7u-weather-sensor-data-17e6db
#include "application.h"
#include "HttpClient.h"
#include "math.h"

#define RF_PIN 2
#define STOP_PIN 3

// Needed to tweak these intervals in order to get more consistent readings
#define PW_FIXED 1000       // Pulse width for the "fixed" part of signal
#define PW_SHORT 550        // Pulse width for the "short" part of signal(1 bit)
#define PW_LONG 1350        // Pulse width for the "long" part of signal (0 bit)
#define PW_TOLERANCE 300    // plus/minus range for valid FIXED, SHORT and LONG pulses
#define NOISE_THRESHOLD 180 // Typical noise pulse duration (in microseconds)
#define SIZE_BUFFER 88      // two pulses needed to determine each bit
#define SIZE_PACKETS_BUF 20 // Max number of undecoded pulse packets which can be stored in the buffer

String _serverTS = "api.thingspeak.com";               // ThingSpeak Server
String _serverWU = "weatherstation.wunderground.com";  // Weather Underground Server

static volatile uint32_t pulseBuffer[SIZE_BUFFER]; //length of time in uSec of each pulse
static volatile uint32_t packetBuffer[SIZE_PACKETS_BUF][SIZE_BUFFER]; //stores packets of pulses before decoding them
static volatile int packetPos = 0;
int _nextPacketPos = 0;

bool _running;
long _msLastLoop;
long _msNextDataPost = 0;
long _countDown;
static int DATA_POST_INTERVAL = 300000;  // milliseconds untill the next post of the readings

//This is really bad.  No easy way to do string enums in CB++!!
std::string toLowerx(std::string in); // Made my own lower case function that takes in a string
std::string _enumCrap;
// enum used by code
enum PhotonNames
{
    eDadPhoton1,
    ejoesPhoton,
    ejoesPhoton3,
    ejoesPhoton4,
    eNameNotFound
};
// map name of device to the enum
PhotonNames getNameEnum(std::string const &inString)
{
    std::string lower = toLowerx(inString);
    if (lower == "dadphoton1")
        return eDadPhoton1;
    if (lower == "joesphoton")
        return ejoesPhoton;
    if (lower == "joesphoton3")
        return ejoesPhoton3;
    if (lower == "joesphoton4")
        return ejoesPhoton4;
    return eNameNotFound;
}

// configuration data
int _NumberOfSensors = 0;
struct reading_t
{
    std::string location;
    int sensorId;
    int tsTempField;
    int tsHumidityField;
    float celsius;
    float farenheit;
    int humidity;
    float dewpoint;
} _readings[10];                       // Tried to make this dynamic sized but got real messy
reading_t *getReading(int sensorId_p); // has to be pre-declared for some reason

//Weather Underground uses HttpClient.h, can't get it to work with TCPClient!
int _outdoorSensorId = 0; // sensor to sent to WeatherUnderground
HttpClient _http;
http_header_t _headers[] = {
    {"Accept", "*/*"},
    {NULL, NULL} // NOTE: Always terminate _headers will NULL
};
http_request_t _request;
http_response_t _response;
String _wuStationId;
String _wuPassword;
const int BUFFER_SIZE = 300;
char _buffer[BUFFER_SIZE];
volatile bool _wuSend = false;

//ThingSpeak
TCPClient _client; // This is build into Particle firmware!?
unsigned int _tsChannelNumber = 0;
const char *_tsWriteAPIKey;

//Particle Cloud
std::string _lastIdsRead;
String _pvLastIdsReads;
//String _pvPhotonName;
static int PV_MAXSTRING = 500;

//used to get name of Photon device
void deviceNameCallback(const char *topic, const char *data)
{
    _enumCrap = data;
    //_pvPhotonName = String(data);
    Serial.println("received " + String(topic) + ": " + String(data));
}

void setup()
{
    Serial.begin(115200);

    // Do Particle cloud registrations early on in setup. Something to do with async registrations.
    Particle.variable("lastIdsRead", _pvLastIdsReads); // particle variable <=12 char length!
    //Particle.variable("deviceName", _pvPhotonName);
    Particle.variable("outSensorId", _outdoorSensorId); // particle variable <=12 char length!
    Particle.variable("countDown", _countDown);

    // this is how we get the device name, seems akward but this is how Particle says it's done.
    Particle.subscribe("spark/", deviceNameCallback);
    Particle.publish("spark/device/name");

    while (_enumCrap == "")
    {
        // beware that delays mess up the definition of Particle.variables from occuring afterwards
        // so to avoid problems we need to define these ahead of the delaying
        delay(100);
    }

    // Configurations
    switch (getNameEnum(_enumCrap))
    {
    case eDadPhoton1:
        Serial.println("case eDadPhoton1");
        _tsChannelNumber = 210556;
        _tsWriteAPIKey = "XXXXXXXXXXXX";
        _outdoorSensorId = 25;

        _wuStationId = "XXXXXXXXX";
        _wuPassword = "XXXXXXXXX";

        _readings[0].sensorId = 25;
        _readings[0].location = "outside";
        _readings[0].tsTempField = 1;
        _readings[0].tsHumidityField = 2;

        _readings[1].sensorId = 14;
        _readings[1].location = "living room";
        _readings[1].tsTempField = 3;
        _readings[1].tsHumidityField = 4;

        _NumberOfSensors = 2;
        break;
    case ejoesPhoton3:
        Serial.println("case ejoesPhoton3");
        _outdoorSensorId = 53;
        _tsChannelNumber = 210556;
        _tsWriteAPIKey = "XXXXXXXXXXX";

        _wuStationId = "XXXXXXX";
        _wuPassword = "XXXXXXXXX";

        _readings[0].sensorId = 18;
        _readings[0].location = "deadsensor";
        _readings[0].tsTempField = 1;
        _readings[0].tsHumidityField = 2;

        _readings[1].sensorId = 54;
        _readings[1].location = "basement";
        _readings[1].tsTempField = 5;
        _readings[1].tsHumidityField = 6;

        _readings[2].sensorId = 53;
        _readings[2].location = "outside";
        _readings[2].tsTempField = 3;
        _readings[2].tsHumidityField = 4;

        _readings[3].sensorId = 81;
        _readings[3].location = "attic";
        _readings[3].tsTempField = 7;
        _readings[3].tsHumidityField = 8;

        _NumberOfSensors = 4;
        break;
    case ejoesPhoton4:
        Serial.println("case ejoesPhoton4");
        _outdoorSensorId = 53;
        _tsChannelNumber = 210556;
        _tsWriteAPIKey = "XXXXXXXXXXX";

        _wuStationId = "XXXXXXXX";
        _wuPassword = "XXXXXXXXX";

        _readings[0].sensorId = 18;
        _readings[0].location = "deadsensor";
        _readings[0].tsTempField = 1;
        _readings[0].tsHumidityField = 2;

        _readings[1].sensorId = 54;
        _readings[1].location = "basement";
        _readings[1].tsTempField = 5;
        _readings[1].tsHumidityField = 6;

        _readings[2].sensorId = 53;
        _readings[2].location = "outside";
        _readings[2].tsTempField = 3;
        _readings[2].tsHumidityField = 4;

        _readings[3].sensorId = 81;
        _readings[3].location = "attic";
        _readings[3].tsTempField = 7;
        _readings[3].tsHumidityField = 8;

        _NumberOfSensors = 4;
        break;
    default:
        Serial.printlnf("Photon named %s is not configured!", _enumCrap.c_str());
        break;
    }

    pinMode(D7, OUTPUT);
    pinMode(RF_PIN, INPUT);
    pinMode(STOP_PIN, INPUT);
    _running = true;
    _msNextDataPost = millis() + DATA_POST_INTERVAL;
    attachInterrupt(RF_PIN, storePulse, CHANGE);

    Serial.print("\nSTART.\n");
}

void loop()
{
    if (_running && (millis() - _msLastLoop) > 1000)
    {
        Serial.print(".");
        _msLastLoop = millis();
        _countDown = (_msNextDataPost - _msLastLoop) / 1000;
        while(_nextPacketPos != packetPos) decodeTimings();

        // millis() will roll over every 49 days
        if (millis() > _msNextDataPost)
        {
            noInterrupts();
            onSendWeatherData();
            _msNextDataPost = millis() + DATA_POST_INTERVAL;
            interrupts();
        }
    }
}

// IMPORTANT: This is an interrupt routine, so its duration should be as short as possible
//            to avoid missing calls (if a pulse is received while we are still processing
//            the previous one, we will miss it!).
//            So: avoid Serial.print() and complex math like decoding timings
void storePulse()
{
    static int pulsePos = 0;         // remember: static retains it's value between calls
    static uint32_t timePrev = 0;    // remember: static retains it's value between calls
    static uint32_t lastSync = 0;    // Number of pulses since last sync signal
    static uint32_t noiseTiming = 0; // Timing interpolation for noise filter    

    uint32_t timeNow = micros();            // 16Mhz Uno board has 4uSec resolution for micros()
    uint32_t interval = timeNow - timePrev; // long 32bit value required for 1,000,000 uSec micros() resolution
    timePrev = timeNow;

    if (interval < NOISE_THRESHOLD) {
        // Probably this short pulse is noise, so we ignore it
        pulseBuffer[pulsePos] += interval / 2;
        noiseTiming += interval / 2;
        return;
    }
    else if (noiseTiming > 0) {
        interval += noiseTiming;
        noiseTiming = 0;
    }

    //continously roll over the buffer
    if (++pulsePos >= SIZE_BUFFER)
        pulsePos = 0;
    
    //every pulse is stored in the buffer
    pulseBuffer[pulsePos] = interval;
    //keeps track of last synchronization signal (= long delay)
    lastSync++;

    //long delay triggers storing the buffer in a "pule packet", which will be analyzed later
    //setting a lower minimum time means there will be more false alarms of evaluating noise
    //but setting it higher means legitimate data _readings will be missed.  Seems that the
    //minimum time delay interval floats around for each sensor.
    if (interval > 5000)
    { // in microseconds
        // Sync signal must be at least one packet away from the previous one
        if (lastSync > SIZE_BUFFER) {
            for(int p = 0; p < SIZE_BUFFER; p++) {
                if (++pulsePos == SIZE_BUFFER) pulsePos = 0;
                packetBuffer[packetPos][p] = pulseBuffer[pulsePos];
            }
            if (++packetPos == SIZE_PACKETS_BUF) packetPos = 0;
        }
        lastSync = 1;        
    }
}

int convertPulsesToBit(uint32_t pulse1, uint32_t pulse2)
{
    // Check second pulse (fixed width)
    uint32_t pw_diff = (pulse2 > PW_FIXED) ? (pulse2 - PW_FIXED) : (PW_FIXED - pulse2);
    if (pw_diff > PW_TOLERANCE)
        return -1; // fixed tolerance exceeded

    // Check first pulse (long or short)
    if (pulse1 < PW_SHORT)
        return ((PW_SHORT - pulse1) < PW_TOLERANCE) ? 1 : -2; // short tolerance exceeded
    if (pulse1 > PW_LONG)
        return ((pulse1 - PW_LONG) < PW_TOLERANCE) ? 0 : -3; // long tolerance exceeded
    // pulse1 width is between PW_SHORT and PW_LONG
    if (pulse1 - PW_SHORT < PW_TOLERANCE)
        return 1; //short detected
    if (PW_LONG - pulse1 < PW_TOLERANCE)
        return 0; //long detected
    return -4;    // got confused
}

void stopRecord()
{
    detachInterrupt(RF_PIN); //Photon
    //detachInterrupt(digitalPinToInterrupt(RF_PIN)); // arduino
    _running = false;
    Serial.println("\nSTOP.");
}

void decodeTimings()
{
    volatile uint32_t *packet = &packetBuffer[_nextPacketPos];
    if (++_nextPacketPos == SIZE_PACKETS_BUF) _nextPacketPos = 0;

    // Decode and pack the bits into an array of bytes
    uint8_t bytes[6] = {0};
    int decodedBit;
    packet[SIZE_BUFFER - 1] = PW_FIXED;
    for(int b = 0; b < SIZE_BUFFER; b += 2) {
        decodedBit = convertPulsesToBit(packet[b], packet[b+1]);
        if (decodedBit < 0)
        {
            Serial.print("\nerror bit #");
            Serial.print(b);
            Serial.print("=");
            Serial.print(decodedBit);
            Serial.print(", pulsePos=");
            Serial.print(b);
            Serial.print(" pulse1:"); // should be LONG or SHORT duration
            Serial.print(packet[b]);
            Serial.print(" pulse2:"); // should be FIXED duration
            Serial.print(packet[b+1]);
            Serial.print("\n");
            return;
        }

        //crazy math trick shortcut to load each of the 44 bits one at a time across the five bytes in the array
        bytes[b / 16] <<= 1;
        //if the next bit is 0 then we are done as the above shift took care of that
        if (decodedBit == 1)
            bytes[b / 16]++; //otherwise set that bit to 1
    }

    // check for the correct start sequence of the 44 bit temp/humidty data
    if (bytes[0] != 0x0A) {
        Serial.println("\nWrong start sequence");
        return;
    }

    // Check parity. Parity decodedBit is #19 and it makes data bits (from #19 to #31) even
    uint8_t bits = (bytes[2] & 0x1F) ^ bytes[3];
    bits ^= bits >> 4;
    bits ^= bits >> 2;
    bits ^= bits >> 1;
    if (bits & 1) {
        Serial.println("\nWrong parity: ");
        printPacket(bytes);
        return; // Parity error
    }

    // Checksum
    uint8_t checksum = 0;
    for (int b = 0; b < 5; b++)
        checksum += (bytes[b] & 0xF) + (bytes[b] >> 4);
    if ((checksum & 0xF) != bytes[5])
    {
        Serial.print("\nWrong checksum: ");
        Serial.println(checksum & 0xF, BIN);
        printPacket(bytes);
        return;
    }


    Serial.println();
    printPacket(bytes);
    decodeBits(bytes);
}

void printPacket(uint8_t *packets)
{ // "* means pointer / pass by reference ?
    for (int b = 0; b < SIZE_PACKETS; b++)
    {
        Serial.print("[");
        Serial.print(b);
        Serial.print("]");
        Serial.print(packets[b], BIN);
        Serial.print(" ");
    }
    Serial.println();
}

void decodeBits(uint8_t *packets)
{
    //packets[1]
    uint8_t sensorAddress = ((packets[1] << 3) & 0x7F) + (packets[2] >> 5);
    uint8_t measureType = packets[1] >> 4;

    //float measureUnits = 0;
    float celsius = 0;
    float farenheit = 0;
    int humidity = 0;
    reading_t *reading = getReading(sensorAddress);

    if (measureType == 0)
    {
        // get celsius
        celsius = (packets[2] & 0xF) * 10 + (packets[3] >> 4) + (packets[3] & 0xF) / 10.0 - 50;
        // weird C++ way of loading value into a struct
        reading->celsius = celsius;
        // convert to farenheit
        farenheit = celsius * 9 / 5 + 32;
        // weird C++ way of loading value into a struct
        reading->farenheit = farenheit;
        //_pvLastIdsReads[0] = String::format("#%i\t%f °F\n",sensorAddress,reading->farenheit);
    }
    else
    {
        humidity = (packets[2] & 0xF) * 10 + (packets[3] >> 4);
        reading->humidity = humidity; // weird C++ way of loading value into a struct
        //_pvLastIdsReads[0] = Serial.printlnf("#%i\t%i %rh\n",sensorAddress,reading->humidity);
    }

    Serial.print("#");
    Serial.print(sensorAddress);
    Serial.print(": ");
    Serial.print((measureType == 0) ? reading->farenheit : reading->humidity);
    Serial.println((measureType == 0) ? " °F" : " %rh");

    if (sensorAddress == _outdoorSensorId && reading->farenheit != 0 && reading->humidity != 0)
    {
        _wuSend = true;
    }

    Serial.printlnf("_outdoorSensorId=%d, sensorAddress=%d, farenheit=%f, humidity=%d, _wuSend %d",
                    _outdoorSensorId, sensorAddress, reading->farenheit, reading->humidity, _wuSend);
    saveReadings(measureType, sensorAddress, reading->farenheit, reading->humidity);
}

void saveReadings(int measureType, int sensorAddress, float farenheit, int humidity)
{
    if (measureType == 0)
    {
        rollingReading(String::format("#%i %f F,", sensorAddress, farenheit));
    }
    else
    {
        rollingReading(String::format("#%i %i rh,", sensorAddress, humidity));
    }
    //Serial.printf("_pvLastIdsReads:%s",_pvLastIdsReads.c_str());
}

//Constantly remove from front of string and add to back of string. FIFO.
void rollingReading(String input)
{
    int total = _lastIdsRead.size() + input.length();
    if (total < PV_MAXSTRING)
    {
        _lastIdsRead.append(input);
    }
    else
    {
        _lastIdsRead.erase(0, total - PV_MAXSTRING);
        _lastIdsRead.append(input);
    }
    _pvLastIdsReads = _lastIdsRead.c_str();
}

void onSendWeatherData()
{
    Serial.printlnf("\nonSendWeatherData() _wuSend %d", _wuSend);

    if (_wuSend)
    {
        // delay calculation of dewpoint as *might* be CPU intensive
        for (int i = 0; i < _NumberOfSensors; i++)
        {
            double dewpointCelsius = computeDewPoint2(_readings[i].celsius, _readings[i].humidity);
            double dewpointFarenheit = dewpointCelsius * 9 / 5 + 32;
            // This is bad way to get struc reference (I don't know what I'm doing!!)
            reading_t *reading = getReading(_readings[i].sensorId);
            reading->dewpoint = dewpointFarenheit;
        }

        digitalWrite(D7, HIGH);
        sendThingSpeak();
        sendWUData(_outdoorSensorId);
        digitalWrite(D7, LOW);

        reading_t *reading = getReading(_outdoorSensorId);
        reading->farenheit = 0;
        reading->humidity = 0;
    }
}

void sendThingSpeak()
{
    if (_tsChannelNumber == 0)
        return;

    //build up part1: GET /update?api_key=[API KEY]
    int charsOut = 0;
    int totCharsOut = 0;
    charsOut = snprintf(_buffer, BUFFER_SIZE, "GET /update?api_key=%s", _tsWriteAPIKey);
    totCharsOut = totCharsOut + charsOut;

    //build up part2: &field1=78.619995&field2=71.960342&field5=0.000000&field6=nan&field3=0.000000&field4=nan&field7=84.739998&field8=69.34550
    for (int i = 0; i < _NumberOfSensors; i++)
    {
        charsOut =
            snprintf(_buffer + totCharsOut, BUFFER_SIZE - totCharsOut, "&field%d=%f&field%d=%f",
                     _readings[i].tsTempField, _readings[i].farenheit, _readings[i].tsHumidityField, _readings[i].dewpoint);
        totCharsOut = totCharsOut + charsOut;
    }

    _client.stop();  // Close any connection before sending a new request
    if (_client.connect(_serverTS, 80))
    {
        _client.println(_buffer);
        _client.println("Host: " + _serverTS);
        _client.println();
    }
    else
    {
        Particle.publish("Failure", "Failed to update ThingSpeak channel");
    }
    delay(2000); // Wait to receive the response
    _client.parseFloat();
    String resp = String(_client.parseInt());
    Serial.printlnf("Response code=%s", resp.c_str());
    _buffer[0] = '\0'; // Reinitialise buffer
}

void sendWUData(int sensorId)
{
    if (_wuStationId == "")
        return;
    reading_t *reading = getReading(sensorId);
    //WARNING! THIS printlnf KILLS PHOTON AND I FRIGGEN DON'T KNOW WHY! FLASHING RED LIGHT
    //Serial.printlnf("sendWUData> station=%s, password=%s, sensor=%i, farenheit=%f, humidity=%i",
    //    _wuStationId.c_str(), _wuPassword.c_str(), reading->sensorId, reading->farenheit, reading->humidity);

    _request.hostname = _serverWU;
    _request.port = 80;

    //This is NOT a display to console. Using sprint to format a string instead.
    snprintf(_buffer, 200,
             "/weatherstation/updateweatherstation.php?action=updateraw&dateutc=now&ID=%s&PASSWORD=%s&tempf=%f&humidity=%i&dewptf=%f",
             _wuStationId.c_str(), _wuPassword.c_str(), reading->farenheit, reading->humidity, reading->dewpoint);

    _request.path = _buffer;
    //Serial.printlnf("\nsendWUData> %s%s",_request.hostname.c_str(), _request.path.c_str());
    // Get _request
    _http.get(_request, _response, _headers);
    _buffer[0] = '\0'; // Reinitialise buffer
    Serial.print("sendWUData> Response status: ");
    Serial.println(_response.status);
    Serial.print("sendWUData> Response Body: ");
    Serial.println(_response.body);
    _wuSend = false;
}

// Can't get to work using built in TCPClient
void sendWUData2(int sensorId)
{
    if (_wuStationId == "") return;
    reading_t *reading = getReading(sensorId);

    _client.stop();  // Close any connection before sending a new request
    snprintf(_buffer, BUFFER_SIZE,
             "GET /weatherstation/updateweatherstation.php?action=updateraw&dateutc=now&ID=%s&PASSWORD=%s&tempf=%f&humidity=%i&dewptf=%f",
             _wuStationId.c_str(), _wuPassword.c_str(), reading->farenheit, reading->humidity, reading->dewpoint);
    Serial.printlnf(_buffer);

    if (_client.connect(_serverWU, 80))
    {
        _client.println(_buffer);
        _client.println("Host: " + _serverWU);
        _client.println("Accept: */");
        _client.println();
    }
    else
    {
        Particle.publish("Failure", "Failed to send to WeatherUnderground");
    }

    delay(2000); // Wait to receive the response
    _client.parseFloat();
    String resp = String(_client.parseInt());
    Serial.printlnf("Response code=%s", resp.c_str());
    _buffer[0] = '\0'; // Reinitialise buffer
}

// If no match found then unitialized array should be returned - UGLY!
// Beware of C++ by reference weirdness here
reading_t *getReading(int sensorId_p)
{ // return pointer
    int i;
    // awfulness of no easy way to know when the array ends
    for (i = 0; i < _NumberOfSensors + 1; i++)
    {
        if (_readings[i].sensorId == sensorId_p)
            break;
    }
    return &_readings[i]; // returns address
}

std::string toLowerx(std::string in)
{
    int i = 0;
    while (in[i])
    {
        in[i] = tolower(in[i]);
        i++;
    }
    return in;
}

//https://gist.github.com/Mausy5043/4179a715d616e6ad8a4eababee7e0281
// reference: http://wahiduddin.net/calc/density_algorithms.htm
double computeDewPoint2(double celsius, double humidity)
{
    //Serial.printlnf("computeDewPoint2 celsius=%f, humidity=%f", celsius, humidity);
    double RATIO = 373.15 / (273.15 + celsius); // RATIO was originally named A0, possibly confusing in Arduino context
    double SUM = -7.90298 * (RATIO - 1);
    SUM += 5.02808 * log10(RATIO);
    SUM += -1.3816e-7 * (pow(10, (11.344 * (1 - 1 / RATIO))) - 1);
    SUM += 8.1328e-3 * (pow(10, (-3.49149 * (RATIO - 1))) - 1);
    SUM += log10(1013.246);
    double VP = pow(10, SUM - 3) * humidity;
    double T = log(VP / 0.61078); // temp var
    return (241.88 * T) / (17.558 - T);
}

// delta max = 0.6544 wrt dewPoint()
// 5x faster than dewPoint()
// reference: http://en.wikipedia.org/wiki/Dew_point
double dewPointFast(double celsius, double humidity)
{
    double a = 17.271;
    double b = 237.7;
    double temp = (a * celsius) / (b + celsius) + log(humidity * 0.01);
    double Td = (b * temp) / (a - temp);
    return Td;
}

/*
OVERVIEW OF MAIN LOGIC

Continously loads pulses into a rolling buffer that is sized to hold one temp
or humidity reading. About every 57 seconds the TX4 and TX7 sensors send a
data transmission consisting of a 44 bit temperature sequence followed by a
repeat of that same 44 bit temperature sequence followed by a 44 bit humidity
sequence.  A relativly long delay occurs after each temp/humidity 44 bit sequence
and that delay is used as the trigger to evaluate the contents of the buffer and
obtain the data values from the respective sequence.

A pulse is the time in microseconds between changes in the data pin. The pulses
have to follow a rule of being a LONG or SHORT followed by FIXED.
A TOLERANCE value specifies the allowable range from the hard coded LONG, SHORT
and FIXED pulse times. A 1 bit is a SHORT followed by a FIXED.  A 0 bit is a LONG
followed by a SHORT.

Here is example of a 10101. Note the variations in the SHORT, LONG and FIXED times.

          SHORT  LONG SHORT  LONG  LONG
           505   1300  595   1275  1395
           ┌-┐  ┌---┐  ┌-┐  ┌---┐  ┌-┐
           |1|  | 0 |  |1|  | 0 |  |1|
         --┘ └--┘   └--┘ └--┘   └--┘ └-
FIXED        980    1030 1105   950

Example showing two timings needed for each pulse
        t2          t4
         \           \              t2-t1=pulse1  FIXED
          ┌----┐     ┌----┐         t3-t2=pulse2  LONG or SHORT
          |    |     |    |     |   t4-t3=pulse3  FIXED
      ----┘    └-----┘    └-----┘   t5-t4=pulse4  LONG or SHORT
     /         /          /
    t1        t3         t5
Because two timings are needed for each bit a total of 88 pulses are needed
to decode the 44 bits.

The pulses are converted into bits and stored in a six byte array as follows:
[0]00001010 [1]11101110 [2]11110100 [3]10010000 [4]01001001 [5]1111
                 |   \      / |  \      |   |       |   |       |
   00001010    1110  1110111  1  0100  1001 0000   0100 1001   1111
bits: 8         4     4+3=7   1    4    4    4       4    4      4
key: (1)       (2)     (3)   (4)  (5)  (6)  (7)     (8)  (9)   (10)
    header    sensor sensor parity 10s  1s  10ths   10s  10th  check
               type    ID    bit                                sum

key: 1) Start Sequence is always 0x0A
     2) sensor 0000 = temp; 1110 = humidity
     3) sensor id
     4) parity bit
     5) ones
     6) tens
     7) tenths (for temp)
     8) repeat ones
     9) repeat tens
     10) checksum
_http://www.f6fbb.org/domo/sensors/tx3_th.php
*/
