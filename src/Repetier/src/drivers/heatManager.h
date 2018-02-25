/*
    This file is part of Repetier-Firmware.

    Repetier-Firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Repetier-Firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Repetier-Firmware.  If not, see <http://www.gnu.org/licenses/>.

*/
enum HeaterError {
    NO_ERROR = 0,      // No error detected
    SENSOR_DEFECT = 1, // Sensor was reported defect
    NO_HEATUP = 2,     // Heating up does not change temperature
    LEAVING_RANGE = 3  // Loosing temperature
};

enum DecoupleMode {
    NO_HEATING = 0,  // Heaters are off
    FAST_RISING = 1, // Full power until control range is reached
    SWING_IN = 2,    // Closing target temperature
    HOLDING = 3,     // Holding temperature
    COOLING = 4,     // Target was dropped but not off
    CALIBRATING = 5, // Signal that no updates should happen
    PAUSED = 6
};

class GCode;
class HeatManager {
protected:
    HeaterError error;
    float targetTemperature;
    float currentTemperature;
    float maxTemperature;
    IOTemperature* input;
    PWMHandler* output;
    fast8_t maxPWM;
    float decoupleVariance;
    millis_t decouplePeriod;

    millis_t lastDecoupleTest; ///< Last time of decoupling sensor-heater test
    float lastDecoupleTemp;    ///< Temperature on last test
    millis_t preheatStartTime; ///< Time (in milliseconds) when heat up was started
    int16_t preheatTemperature;
    DecoupleMode decoupleMode; // 0 = no heating, 1
    fast8_t errorCount;
    fast8_t wasOutsideRange; // 1 = if was above range, 2 = was below range
    char heaterType;         // E = Extruder, B = bed, C = Chamber, O = Other
public:
    HeatManager(char htType, IOTemperature* i, PWMHandler* o, float maxTemp, fast8_t maxPwm, float decVariance, millis_t decPeriod)
        : error(HeaterError::NO_ERROR)
        , targetTemperature(0)
        , currentTemperature(0)
        , maxTemperature(maxTemp)
        , input(i)
        , output(o)
        , maxPWM(maxPwm)
        , decoupleVariance(decVariance)
        , decouplePeriod(decPeriod)
        , decoupleMode(DecoupleMode::NO_HEATING)
        , errorCount(0)
        , heaterType(htType) {}
    virtual void setTargetTemperature(float temp) {
        if (temp > maxTemperature) {
            Com::printWarningF(PSTR("Selected temp. was higher then max. temperaure. Max. Temp:"));
            Com::print(maxTemperature);
            Com::println();
            return;
        }
        if (temp == 0) {
            decoupleMode = DecoupleMode::NO_HEATING;
        } else if (temp < currentTemperature) {
            decoupleMode = DecoupleMode::COOLING;
        } else {
            decoupleMode = DecoupleMode::FAST_RISING;
            lastDecoupleTest = HAL::timeInMilliseconds();
            lastDecoupleTemp = currentTemperature;
        }
        targetTemperature = temp;
    }
    inline bool isEnabled() {
        return decoupleMode != DecoupleMode::PAUSED && targetTemperature > MAX_ROOM_TEMPERATURE;
    }
    inline void pause() {
        decoupleMode = DecoupleMode::PAUSED;
        output->set(0);
    }
    inline void unpause() {
        setTargetTemperature(targetTemperature);
    }
    inline float getTargetTemperature() { return targetTemperature; }
    inline void setCurrentTemperature(float temp) {
        currentTemperature = temp;
    }
    inline float getCurrentTemperature() { return currentTemperature; }
    inline float getPreheatTemperature() { return preheatTemperature; }
    inline HeaterError getError() { return error; }
    inline void resetError() { error = HeaterError::NO_ERROR; }
    inline void setError(HeaterError err) {
        error = err;
        Com::printFLN(PSTR("setError:"), (int)err);
        if (err != HeaterError::NO_ERROR) {
            output->set(0);
            decoupleMode = DecoupleMode::NO_HEATING;
        }
    }
    virtual float getP() { return 0; }
    virtual float getI() { return 0; }
    virtual float getD() { return 0; }
    virtual void setPID(float p, float i, float d) {}

    virtual void updateLocal(float tempError) = 0;
    int eepromSize() {
        return eepromSizeLocal() + 9;
    }
    void eepromHandle(int adr) {

        eepromHandleLocal(adr + eepromSize());
    }
    virtual void eepromHandleLocal(int adr) = 0;
    virtual void eepromResetLocal() = 0;
    virtual int eepromSizeLocal() = 0;
    void update();
    virtual void updateDerived() {}
    /** Waits until the set target temperature is reached */
    void waitForTargetTemperature();
    inline float getMaxTemperature() { return maxTemperature;}
    void reportTemperature(char c, int idx);
    virtual void autocalibrate(GCode* g) {
        Com::printWarningFLN(PSTR("Autocalibration for this tool not supported!"));
    }
    bool isExtruderHeater() { return heaterType == 'E'; }
    bool isBedHeater() { return heaterType == 'B'; }
    bool isChamberHeater() { return heaterType == 'C'; }
    bool isOtherHeater() { return heaterType == 'O'; }

    static bool reportTempsensorError();
    static void disableAllHeaters();
    static void resetAllErrorStates();
};

class HeatManagerBangBang : public HeatManager {

public:
    void updateLocal(float tempError) {
        output->set(currentTemperature > targetTemperature ? 0 : maxPWM);
    }
    int eepromSize() {
        return 1;
    }
    void resetFromConfig(fast8_t _maxPwm, float decVariance, millis_t decPeriod) {
        maxPWM = _maxPwm;
        decoupleVariance = decVariance;
        decouplePeriod = decPeriod;
    }
    void eepromHandleLocal(int adr);
    void eepromResetLocal();
    int eepromSizeLocal();
};

class HeatManagerPID : public HeatManager {
    float P, I, D;
    float IMax, IMin;
    float IState;
    float driveMin, driveMax;
    float lastTemperature;
    float actTemperature;
    fast8_t counter;

public:
    HeatManagerPID(char htType, IOTemperature* input, PWMHandler* output, float maxTemp, fast8_t maxPwm, float decVariance, millis_t decPeriod,
                   float p, float i, float d, float _driveMin, float _driveMax)
        : HeatManager(htType, input,
                      output, maxTemp, maxPwm, decVariance, decPeriod)
        , P(p)
        , I(i)
        , D(d)
        , driveMin(_driveMin)
        , driveMax(_driveMax) {
        IState = 0;
        actTemperature = lastTemperature = 20;
        counter = 0;
        updateDerived();
    }
    void updateLocal(float tempError);
    void updateDerived();
    void resetFromConfig(fast8_t _maxPwm, float decVariance, millis_t decPeriod,
                         float p, float i, float d, float _driveMin, float _driveMax);
    void eepromHandleLocal(int adr);
    void eepromResetLocal();
    int eepromSizeLocal();
    void autocalibrate(GCode* g);
    float getP() { return P; }
    float getI() { return I; }
    float getD() { return D; }
    void setPID(float p, float i, float d);
};

extern HeatManager* heaters[];
