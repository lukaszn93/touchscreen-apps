/*
 * ADS7846.cpp
 *
 * @date 05.12.2012
 * @author Armin Joachimsmeyer
 * armin.joachimsmeyer@gmx.de
 * @copyright LGPL v3 (http://www.gnu.org/licenses/lgpl.html)
 * @version 1.0.0
 *
 *      based on ADS7846.cpp with license
 *      https://github.com/watterott/mSD-Shield/blob/master/src/license.txt
 */

#include "ADS7846.h"
#include "HY32D.h"
#include "misc.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {

#include "timing.h"
#include "math.h"
#include "stm32f30xPeripherals.h"
#include "stm32f3_discovery.h"  // For LEDx
}
#endif

#define TOUCH_DELAY_AFTER_READ_MILLIS 5
#define TOUCH_DEBOUNCE_DELAY_MILLIS 10 // wait for debouncing in ISR - minimum 8 ms
#define TOUCH_SWIPE_RESOLUTION_MILLIS 20

#define CMD_START       (0x80)
#define CMD_12BIT       (0x00)
#define CMD_8BIT        (0x08)
#define CMD_DIFF        (0x00)
#define CMD_SINGLE      (0x04)

#define CHANNEL_MASK 0x70
// Power modes
#define CMD_PWD         (0x00)
#define ADC_ON    		(0x01)
#define REF_ON    		(0x02)
#define CMD_ALWAYSON    (0x03)

// Set 2,5V reference on. Only useful when using readChannel(), otherwise take CMD_PWD!
#define POWER_MODE CMD_ALWAYSON

char PosZ1[] = "Z Pos 1";
char PosZ2[] = "Z Pos 2";
char PosX[] = "X Pos";
char PosY[] = "Y Pos";
char Temperature0[] = "Temp. 0";
char Temperature1[] = "Temp. 1";
char Vcc[] = "VCC";
char Aux[] = "Aux In";
char * ADS7846ChannelStrings[] = { PosZ1, PosZ2, PosX, PosY, Temperature0, Temperature1, Vcc, Aux };
char ADS7846ChannelChars[] = { 'z', 'Z', 'X', 'Y', 't', 'T', 'V', 'A' };

// Channel number to text mapping
unsigned char ADS7846ChannelMapping[] = { 3, 4, 1, 5, 0, 7, 2, 6 };

// to start quick if backup battery was not assembled or empty
CAL_MATRIX tInitalMatrix = { 320300, -1400, -52443300, -3500, 237700, -21783300, 1857905 };

void callbackPeriodicTouch(void);

//-------------------- Constructor --------------------

ADS7846::ADS7846(void) {

}

// One instance of ADS7846 called TouchPanel
ADS7846 TouchPanel;

/**
 * This function is indirectly called by systick handler with 3. parameter true or
 * on end of touch with 3. parameter false if not called by systick before
 * so it is guaranteed that it is called once per touch
 * The return parameter of the callback function is not used yet
 */bool (*mLongTouchCallback)(int const, int const, bool const) = NULL;
volatile bool mLongTouchCallbackHappened = false; // Flag for ISR and timer callback to call mLongTouchCallback only once per touch
volatile bool mEndTouchCallbackHappened = false; // Flag for ISR and timer callback to call mEndTouchCallback only once per touch

bool (*mPeriodicTouchCallback)(int const, int const) = NULL; // return parameter not yet used
bool (*mEndTouchCallback)(uint32_t const, int const, int const) = NULL;
void callbackLongTouchTimeout(void);

//-------------------- Public --------------------

void ADS7846::init(void) {
	//init pins
	ADS7846_IO_initalize();

	//set vars
	tp_matrix.div = 0;
	mTouchActualPositionRaw.x = 0;
	mTouchActualPositionRaw.y = 0;
	mTouchLastCalibratedPosition.x = 0;
	mTouchLastCalibratedPosition.y = 0;
	mTouchActualPosition.x = 0;
	mTouchActualPosition.y = 0;
	mPressure = 0;
	ADS7846TouchActive = false;
	ADS7846TouchStart = false;
}

bool ADS7846::setCalibration(CAL_POINT *aTargetValues, CAL_POINT *aRawValues) {
	tp_matrix.div = ((aRawValues[0].x - aRawValues[2].x) * (aRawValues[1].y - aRawValues[2].y))
			- ((aRawValues[1].x - aRawValues[2].x) * (aRawValues[0].y - aRawValues[2].y));

	if (tp_matrix.div == 0) {
		return false;
	}

	tp_matrix.a = ((aTargetValues[0].x - aTargetValues[2].x) * (aRawValues[1].y - aRawValues[2].y))
			- ((aTargetValues[1].x - aTargetValues[2].x) * (aRawValues[0].y - aRawValues[2].y));

	tp_matrix.b = ((aRawValues[0].x - aRawValues[2].x) * (aTargetValues[1].x - aTargetValues[2].x))
			- ((aTargetValues[0].x - aTargetValues[2].x) * (aRawValues[1].x - aRawValues[2].x));

	tp_matrix.c = (aRawValues[2].x * aTargetValues[1].x - aRawValues[1].x * aTargetValues[2].x) * aRawValues[0].y
			+ (aRawValues[0].x * aTargetValues[2].x - aRawValues[2].x * aTargetValues[0].x) * aRawValues[1].y
			+ (aRawValues[1].x * aTargetValues[0].x - aRawValues[0].x * aTargetValues[1].x) * aRawValues[2].y;

	tp_matrix.d = ((aTargetValues[0].y - aTargetValues[2].y) * (aRawValues[1].y - aRawValues[2].y))
			- ((aTargetValues[1].y - aTargetValues[2].y) * (aRawValues[0].y - aRawValues[2].y));

	tp_matrix.e = ((aRawValues[0].x - aRawValues[2].x) * (aTargetValues[1].y - aTargetValues[2].y))
			- ((aTargetValues[0].y - aTargetValues[2].y) * (aRawValues[1].x - aRawValues[2].x));

	tp_matrix.f = (aRawValues[2].x * aTargetValues[1].y - aRawValues[1].x * aTargetValues[2].y) * aRawValues[0].y
			+ (aRawValues[0].x * aTargetValues[2].y - aRawValues[2].x * aTargetValues[0].y) * aRawValues[1].y
			+ (aRawValues[1].x * aTargetValues[0].y - aRawValues[0].x * aTargetValues[1].y) * aRawValues[2].y;

	return true;
}

#define CALIBRATION_MAGIC_NUMBER 0x5A5A5A5A
bool ADS7846::writeCalibration(CAL_MATRIX aMatrix) {
	if (aMatrix.div != 0) {
		PWR_BackupAccessCmd(ENABLE);
		// Write magic number
		RTC_WriteBackupRegister(RTC_BKP_DR0, CALIBRATION_MAGIC_NUMBER);
		// Write data
		RTC_WriteBackupRegister(RTC_BKP_DR1, aMatrix.a);
		RTC_WriteBackupRegister(RTC_BKP_DR2, aMatrix.b);
		RTC_WriteBackupRegister(RTC_BKP_DR3, aMatrix.c);
		RTC_WriteBackupRegister(RTC_BKP_DR4, aMatrix.d);
		RTC_WriteBackupRegister(RTC_BKP_DR5, aMatrix.e);
		RTC_WriteBackupRegister(RTC_BKP_DR6, aMatrix.f);
		RTC_WriteBackupRegister(RTC_BKP_DR7, aMatrix.div);
		PWR_BackupAccessCmd(DISABLE);
		return true;
	}

	return false;
}

/**
 *
 * @return true if calibration data valid and matrix is set
 */bool ADS7846::readCalibration(CAL_MATRIX *aMatrix) {
	uint32_t tMagic;
	// Read magic number
	tMagic = RTC_ReadBackupRegister(RTC_BKP_DR0);
	if (tMagic == CALIBRATION_MAGIC_NUMBER) {
		aMatrix->a = RTC_ReadBackupRegister(RTC_BKP_DR1);
		aMatrix->b = RTC_ReadBackupRegister(RTC_BKP_DR2);
		aMatrix->c = RTC_ReadBackupRegister(RTC_BKP_DR3);
		aMatrix->d = RTC_ReadBackupRegister(RTC_BKP_DR4);
		aMatrix->e = RTC_ReadBackupRegister(RTC_BKP_DR5);
		aMatrix->f = RTC_ReadBackupRegister(RTC_BKP_DR6);
		aMatrix->div = RTC_ReadBackupRegister(RTC_BKP_DR7);
		return true;
	}
	return false;
}

/**
 * touch panel calibration routine
 */
void ADS7846::doCalibration(bool aCheckRTC) {
	CAL_POINT tReferencePoints[3] = { CAL_POINT1, CAL_POINT2, CAL_POINT3 }; //calibration point positions
	CAL_POINT tRawPoints[3];

	if (aCheckRTC) {
		//calibration data in CMOS RAM?
		bool tCalDataValid = readCalibration(&tp_matrix);
		if (!tCalDataValid) {
			// not valid -> set initial values
			writeCalibration(tInitalMatrix);
			// store in datastructure
			readCalibration(&tp_matrix);
		}
		return;
	}

	//show calibration points
	for (uint8_t i = 0; i < 3;) {
		//clear screen and wait for touch release
		clearDisplay(COLOR_WHITE);
		drawText((DISPLAY_WIDTH / 2) - 50, (DISPLAY_HEIGHT / 2) - 10, StringCalibration, 1, COLOR_BLACK, COLOR_WHITE);

		//draw points
		drawCircle(tReferencePoints[i].x, tReferencePoints[i].y, 2, RGB(0, 0, 0));
		drawCircle(tReferencePoints[i].x, tReferencePoints[i].y, 5, RGB(0, 0, 0));
		drawCircle(tReferencePoints[i].x, tReferencePoints[i].y, 10, RGB(255, 0, 0));

		// wait for touch to become active
		while (!TouchPanel.wasTouched()) {
			delayMillis(5);
		}
		// wait for stabilizing data
		delayMillis(10);
		//get new data
		rd_data(4 * ADS7846_READ_OVERSAMPLING_DEFAULT);
		//press detected -> save point
		fillCircle(tReferencePoints[i].x, tReferencePoints[i].y, 2, RGB(255, 0, 0));
		tRawPoints[i].x = getXRaw();
		tRawPoints[i].y = getYRaw();
		// reset touched flag
		TouchPanel.wasTouched();
		i++;
	}

	//Calculate calibration matrix
	setCalibration(tReferencePoints, tRawPoints);
	//save calibration matrix
	writeCalibration(tp_matrix);
	clearDisplay(COLOR_WHITE);

	return;
}

/**
 * convert raw to calibrated position in mTouchActualPosition
 */
void ADS7846::calibrate(void) {
	long x, y;

	//calc x pos
	if (mTouchActualPositionRaw.x != mTouchLastCalibratedPosition.x) {
		mTouchLastCalibratedPosition.x = mTouchActualPositionRaw.x;
		x = mTouchActualPositionRaw.x;
		y = mTouchActualPositionRaw.y;
		x = ((tp_matrix.a * x) + (tp_matrix.b * y) + tp_matrix.c) / tp_matrix.div;
		if (x < 0) {
			x = 0;
		} else if (x >= DISPLAY_WIDTH) {
			x = DISPLAY_WIDTH - 1;
		}
		mTouchActualPosition.x = x;
	}

	//calc y pos
	if (mTouchActualPositionRaw.y != mTouchLastCalibratedPosition.y) {
		mTouchLastCalibratedPosition.y = mTouchActualPositionRaw.y;
		x = mTouchActualPositionRaw.x;
		y = mTouchActualPositionRaw.y;
		y = ((tp_matrix.d * x) + (tp_matrix.e * y) + tp_matrix.f) / tp_matrix.div;
		if (y < 0) {
			y = 0;
		} else if (y >= DISPLAY_HEIGHT) {
			y = DISPLAY_HEIGHT - 1;
		}
		mTouchActualPosition.y = y;
	}

	return;
}

int ADS7846::getXRaw(void) {
	return mTouchActualPositionRaw.x;
}

int ADS7846::getYRaw(void) {
	return mTouchActualPositionRaw.y;
}

int ADS7846::getXActual(void) {
	return mTouchActualPosition.x;
}

int ADS7846::getYActual(void) {
	return mTouchActualPosition.y;
}

int ADS7846::getXFirst(void) {
	return mTouchFirstPosition.x;
}

int ADS7846::getYFirst(void) {
	return mTouchFirstPosition.y;
}

int ADS7846::getPressure(void) {
	return mPressure;
}

// read individual A/D channels like temperature or Vcc
uint16_t ADS7846::readChannel(uint8_t channel, bool use12Bit, bool useDiffMode, int numberOfReadingsToIntegrate) {
	channel <<= 4;
	// mask for channel 0 to 7
	channel &= CHANNEL_MASK;
	uint16_t tRetValue = 0;
	uint8_t low, high, i;

	//SPI speed-down
	uint16_t tPrescaler = SPI1_getPrescaler();
	SPI1_setPrescaler(SPI_BaudRatePrescaler_256);

	// disable interrupts for some ms in order to wait for the interrupt line to go high - minimum 0,5 ms
	changeDelayCallback(&ADS7846_clearAndEnableInterrupt, TOUCH_DELAY_AFTER_READ_MILLIS);
	ADS7846_disableInterrupt(); // only needed for X, Y + Z channel

	//read channel
	ADS7846_CSEnable();
	uint8_t tMode = CMD_SINGLE;
	if (useDiffMode) {
		tMode = CMD_DIFF;
	}
	for (i = numberOfReadingsToIntegrate; i != 0; i--) {
		if (use12Bit) {
			SPI1_sendReceiveFast(CMD_START | CMD_12BIT | tMode | channel);
			high = SPI1_sendReceive(0);
			low = SPI1_sendReceive(0);
			tRetValue += (high << 5) | (low >> 3);
		} else {
			SPI1_sendReceiveFast(CMD_START | CMD_8BIT | tMode | channel);
			tRetValue += SPI1_sendReceive(0);
		}
	}
	ADS7846_CSDisable();

	//restore SPI settings
	SPI1_setPrescaler(tPrescaler);

	return tRetValue / numberOfReadingsToIntegrate;
}

void ADS7846::rd_data(void) {
	rd_data(ADS7846_READ_OVERSAMPLING_DEFAULT);
}

void ADS7846::rd_data(int aOversampling) {
	uint8_t a, b, i;
	uint32_t tXValue, tYValue;

	Set_DebugPin();
	//SPI speed-down
	// optimal is CLK < 125kHz (40-80 kHz) => use 72 MHz / 1024
	uint16_t tPrescaler = SPI1_getPrescaler();
	SPI1_setPrescaler(SPI_BaudRatePrescaler_256); // 280 kHz

	/*
	 * Disable interrupt for debouncing after ADS7846 was read and int line went low - minimum 0,5 ms
	 * Since on oversampling each SPI read triggers another interrupt, 5 ms are only reasonable till oversampling factor 28
	 * after this the SPI gets an error because of ISR calling itself rd_data and accessing SPI
	 */
	int tDelayValue = ((aOversampling / 16) + 1) * TOUCH_DELAY_AFTER_READ_MILLIS;
	changeDelayCallback(&ADS7846_clearAndEnableInterrupt, tDelayValue);
	ADS7846_disableInterrupt();

	//get pressure
	ADS7846_CSEnable();
	SPI1_sendReceiveFast(CMD_START | CMD_8BIT | CMD_DIFF | CMD_Z1_POS);
	a = SPI1_sendReceiveFast(0);
	SPI1_sendReceiveFast(CMD_START | CMD_8BIT | CMD_DIFF | CMD_Z2_POS);
	b = SPI1_sendReceiveFast(0);
	b = 127 - b; // 127 is maximum reading of CMD_Z2_POS!
	int tPressure = a + b;

	mPressure = 0;
	ADS7846TouchActive = false;

	if (tPressure >= MIN_REASONABLE_PRESSURE) {
		// n times oversampling
		int j = 0;
		for (tXValue = 0, tYValue = 0, i = aOversampling; i != 0; i--) {
			//get X data
			SPI1_sendReceiveFast(CMD_START | CMD_12BIT | CMD_DIFF | CMD_X_POS);
			a = SPI1_sendReceiveFast(0);
			b = SPI1_sendReceiveFast(0);
			uint32_t tX = (a << 5) | (b >> 3); //12bit: ((a<<5)|(b>>3)) //10bit: ((a<<3)|(b>>5))
			if (tX >= 4000) {
				// no reasonable value
				break;
			}
			//get Y data
			SPI1_sendReceiveFast(CMD_START | CMD_12BIT | CMD_DIFF | CMD_Y_POS);
			a = SPI1_sendReceiveFast(0);
			b = SPI1_sendReceiveFast(0);
			uint32_t tY = (a << 5) | (b >> 3); //12bit: ((a<<5)|(b>>3)) //10bit: ((a<<3)|(b>>5))
			if (tY <= 100) {
				// no reasonable value
				break;
			}
			tXValue += 4048 - tX;
			tYValue += tY;
			j += 2; // +2 to get 11 bit values at the end
		}
		if (j == (aOversampling * 2)) {
			// scale down to 11 bit because calibration does not work with 12 bit values
			tXValue /= j;
			tYValue /= j;

			// plausi check is pressure still greater than 7/8 start pressure
			SPI1_sendReceiveFast(CMD_START | CMD_8BIT | CMD_DIFF | CMD_Z1_POS);
			a = SPI1_sendReceiveFast(0);
			SPI1_sendReceiveFast(CMD_START | CMD_8BIT | CMD_DIFF | CMD_Z2_POS);
			b = SPI1_sendReceiveFast(0);
			b = 127 - b; // 127 is maximum reading of CMD_Z2_POS!

			// plausi check - x raw value is from 130 to 3900 here x is (4048 - x)/2, y raw is from 150 to 3900 - low is upper right corner
			if (((a + b) > (tPressure - (tPressure >> 3))) && (tXValue >= 10) && (tYValue >= 10)) {
				mTouchActualPositionRaw.x = tXValue;
				mTouchActualPositionRaw.y = tYValue;
				calibrate();
				mPressure = tPressure;
				ADS7846TouchActive = true;
			}
		}
	}
	ADS7846_CSDisable();
	//restore SPI settings
	SPI1_setPrescaler(tPrescaler);
	Reset_DebugPin();
	return;
}

//show touchpanel data
void ADS7846::printTPData(int x, int y, uint16_t aColor, uint16_t aBackColor) {
	snprintf(StringBuffer, sizeof StringBuffer, "X:%03i|%04i Y:%03i|%04i P:%03i", TouchPanel.getXActual(), TouchPanel.getXRaw(),
			TouchPanel.getYActual(), TouchPanel.getYRaw(), TouchPanel.getPressure());
	drawText(x, y, StringBuffer, 1, aColor, aBackColor);
}

float ADS7846::getSwipeAmount(void) {
	int tDeltaX = mTouchFirstPosition.x - mTouchLastPosition.x;
	int tDeltaY = mTouchFirstPosition.y - mTouchLastPosition.y;
	return sqrtf(tDeltaX * tDeltaX + tDeltaY * tDeltaY);
}

/**
 * This handler is called on both edge of touch interrupt signal
 * actually the ADS7846 IRQ signal bounces on rising edge
 * This can happen up to 8 milliseconds after initial transition
 */
extern "C" void EXTI1_IRQHandler(void) {
	STM_EVAL_LEDToggle(LED9); // BLUE Front

	// wait for stable reading
	delayMillis(TOUCH_DEBOUNCE_DELAY_MILLIS);
	bool tLevel = ADS7846_getInteruptLineLevel();
	if (!tLevel) {
		// pressed - line input is low, touch just happened
		TouchPanel.rd_data(ADS7846_READ_OVERSAMPLING_DEFAULT); // this disables interrupt for additional TOUCH_DELAY_AFTER_READ_MILLIS
		if (TouchPanel.ADS7846TouchActive) {
			TouchPanel.ADS7846TouchStart = true;

			if (mEndTouchCallback != NULL) {
				mEndTouchCallbackHappened = false; // reset flag
				/*
				 * Register swipe recognition. If another periodic callback is registered, no swipe recognition is done
				 */
				mPeriodicTouchCallback = NULL; // to indicate swipe recognition
				changeDelayCallback(&callbackPeriodicTouch, TOUCH_SWIPE_RESOLUTION_MILLIS);
				TouchPanel.mPeriodicCallbackPeriodMillis = TOUCH_SWIPE_RESOLUTION_MILLIS;
			}

			TouchPanel.ADS7846TouchStartMillis = getMillisSinceBoot();
			TouchPanel.mTouchFirstPosition = TouchPanel.mTouchActualPosition;
			if (mLongTouchCallback != NULL) {
				mLongTouchCallbackHappened = false; // reset flag
				changeDelayCallback(&callbackLongTouchTimeout, TouchPanel.mLongTouchTimeoutMillis); // enable timeout
			}
		}
	} else {
		// released - line input is high
		// ensure that callback is only called once per touch
		if (mLongTouchCallback != NULL && !mLongTouchCallbackHappened) {
			mLongTouchCallbackHappened = true; // set flag
			mLongTouchCallback(TouchPanel.mTouchFirstPosition.x, TouchPanel.mTouchFirstPosition.y, false); // short touch
		}
		if (mEndTouchCallback != NULL && !mEndTouchCallbackHappened) {
			mEndTouchCallbackHappened = true; // set flag
			mEndTouchCallback(getMillisSinceBoot() - TouchPanel.ADS7846TouchStartMillis,
					TouchPanel.mTouchFirstPosition.x - TouchPanel.mTouchLastPosition.x,
					TouchPanel.mTouchFirstPosition.y - TouchPanel.mTouchLastPosition.y);
		}
	}
	resetBacklightTimeout();
	/* Clear the EXTI line pending bit */
	ADS7846_ClearITPendingBit();
}

/**
 * is called by main loops
 * return only one times a "true" per touch
 */bool ADS7846::wasTouched(void) {
	if (ADS7846TouchStart) {
		// reset => return only one true per touch
		ADS7846TouchStart = false;
		return true;
	}
	return false;
}

/**
 * Callback routine for SysTick handler
 */
void callbackLongTouchTimeout(void) {
	assert_param(mLongTouchCallback != NULL);
// ensure that callback is only called once
	if (!mLongTouchCallbackHappened) {
		mLongTouchCallbackHappened = true; // set flag
		mLongTouchCallback(TouchPanel.mTouchFirstPosition.x, TouchPanel.mTouchFirstPosition.y, true); // long touch
	}
}
/**
 * Register a callback routine which is called every CallbackPeriod milliseconds while screen is touched
 */
void ADS7846::registerLongTouchCallback(bool (*aLongTouchCallback)(int const, int const, bool const),
		const uint32_t aLongTouchTimeoutMillis) {
	if (aLongTouchCallback == NULL) {
		changeDelayCallback(&callbackLongTouchTimeout, DISABLE_TIMER_DELAY_VALUE); // disable timeout
	}
	mLongTouchCallback = aLongTouchCallback;
	mLongTouchTimeoutMillis = aLongTouchTimeoutMillis;
}

/**
 * Callback routine for SysTick handler
 */
void callbackPeriodicTouch(void) {
	TouchPanel.rd_data();
	if (TouchPanel.ADS7846TouchActive) {
		if (mPeriodicTouchCallback != NULL) {
			mPeriodicTouchCallback(TouchPanel.mTouchActualPosition.x, TouchPanel.mTouchActualPosition.y);
		} else {
			// swipe recognition here -> store last position
			TouchPanel.mTouchLastPosition = TouchPanel.mTouchActualPosition;
		}
		registerDelayCallback(&callbackPeriodicTouch, TouchPanel.mPeriodicCallbackPeriodMillis);
	} else {
		mPeriodicTouchCallback = NULL;
		/*
		 * touch released
		 * check also here and not only in EXTI1_IRQHandler because sometimes the rising edge does not generate an interrupt
		 */
		// ensure that callback is only called once per touch
		if (mLongTouchCallback != NULL && !mLongTouchCallbackHappened) {
			mLongTouchCallbackHappened = true; // set flag
			mLongTouchCallback(TouchPanel.mTouchFirstPosition.x, TouchPanel.mTouchFirstPosition.y, false); // short touch
		}
		if (mEndTouchCallback != NULL && !mEndTouchCallbackHappened) {
			mEndTouchCallbackHappened = true; // set flag
			mEndTouchCallback(getMillisSinceBoot() - TouchPanel.ADS7846TouchStartMillis,
					TouchPanel.mTouchFirstPosition.x - TouchPanel.mTouchLastPosition.x,
					TouchPanel.mTouchFirstPosition.y - TouchPanel.mTouchLastPosition.y);
		}
	}
}

/**
 * Register a callback routine which is called every CallbackPeriod milliseconds while screen is touched
 */
void ADS7846::registerPeriodicTouchCallback(bool (*aPeriodicTouchCallback)(int const, int const),
		const uint32_t aCallbackPeriodMillis) {
	mPeriodicTouchCallback = aPeriodicTouchCallback;
	changeDelayCallback(&callbackPeriodicTouch, aCallbackPeriodMillis);
	mPeriodicCallbackPeriodMillis = aCallbackPeriodMillis;
}

/**
 * Register a callback routine which is called when screen touch ends
 */
void ADS7846::registerEndTouchCallback(bool (*aTouchEndCallback)(uint32_t const, int const, int const)) {
	mEndTouchCallback = aTouchEndCallback;
}
/**
 * set CallbackPeriod
 */
void ADS7846::setCallbackPeriod(const uint32_t aCallbackPeriod) {
	mPeriodicCallbackPeriodMillis = aCallbackPeriod;
}

