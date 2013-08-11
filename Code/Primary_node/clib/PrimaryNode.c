#include "Node.h"
#include "MavlinkGlue.h"
#include "Acs300.h"
#include "Uart1.h"
#include "Ecan1.h"
#include "PrimaryNode.h"
#include "DataStore.h"
#include "EcanSensors.h"

#include <pps.h>
#include <adc.h>
// Clearing the TRIS bit for a pin specifies it as an output
#define OUTPUT 0

// Declare some macros for helping setting bit values
#define ON  1
#define OFF 0

// Specify how long after startup the node should stay in reset to let things stabilize. Units are
// centiseconds.
#define STARTUP_RESET_TIME 200

// A counter used for tracking the blinking of the automode LED.
static uint8_t autoModeBlinkCounter = 0;

void PrimaryNodeInit(void)
{
	// Set the ID for the primary node.
	nodeId = CAN_NODE_PRIMARY_CONTROLLER;

	// Initialize UART1 to 115200 for groundstation communications.
	Uart1Init(BAUD115200_BRG_REG);

	// Initialize the EEPROM for non-volatile data storage. DataStoreInit() also takes care of
	// initializing the onboard data store to the current parameter values so all subsequent calls
	// to DataStoreLoadAllParameters() should work.
	if (!DataStoreInit()) {
		FATAL_ERROR();
	}

	// Initialize ECAN1
	Ecan1Init();

	// Initialize the MAVLink communications channel
	MavLinkInit();

	// Set up the ADC
	PrimaryNodeAdcInit();

	// Finally perform the necessary pin mappings:
	PPSUnLock;
	// To enable ECAN1 pins: TX on 7, RX on 4
	PPSOutput(OUT_FN_PPS_C1TX, OUT_PIN_PPS_RP7);
	PPSInput(PPS_C1RX, PPS_RP4);

	// To enable UART1 pins: TX on 11, RX on 13
	PPSOutput(OUT_FN_PPS_U1TX, OUT_PIN_PPS_RP11);
	PPSInput(PPS_U1RX, PPS_RP13);

	// To enable UART2 pins: TX on 8, RX on 9
	PPSOutput(OUT_FN_PPS_U2TX, OUT_PIN_PPS_RP8);
	PPSInput(PPS_U2RX, PPS_RP9);
	PPSLock;

	// And specify input/output settings for digital I/O ports:
	// A3 (output): Red LED on the CANode, blinks at 2Hz when the system is in reset, and is solid when the system hit a fatal error, otherwise off.
	_TRISA3 = OUTPUT;
	// A4 (output): Amber LED on the CANode, blinks at 1Hz for status.
	_TRISA4 = OUTPUT;
	// B12 (output): Amber automode LED on the CANode Primary Shield, on when system is autonomous, blinking at 4Hz when in manual override, and off otherwise.
	_TRISB12 = OUTPUT;
	// B15 (output): Amber GPS LED on the CANode Primary Shield, on when GPS is active & receiving good data.
	_TRISB15 = OUTPUT;

	// Now before we start everything, make sure we have our state correct given that we just started
	// up. Every sensor is assumed to be online, but just expiring on initialization, so here we call
	// the necessary code to trigger the timeout event for every sensor.
	UpdateSensorsAvailability();
}

/**
 * Perform main timed loop at 100Hz.
 */
void PrimaryNode100HzLoop(void)
{
	// Keep an internal counter around so that other processes can occur at less than 100Hz.
	static uint8_t internalCounter = 0;

	// Process incoming ECAN messages.
	ProcessAllEcanMessages();

	// Check for new MaVLink messages.
	MavLinkReceive();

	// Send any necessary messages for this timestep.
	MavLinkTransmit();

	// At 2Hz transmit a NODE_STATUS ECAN message.
	if (internalCounter == 0 || internalCounter == 50) {
		NodeTransmitStatus();
	}

	// Set a reset signal for the first 2 seconds, allowing things to stabilize a bit before the
	// system responds. This is especially crucial because it can take up to a second for sensors to
	// timeout and appear as offline.
	if (nodeSystemTime == 0) {
		nodeErrors |= PRIMARY_NODE_RESET_STARTUP;
	} else if (nodeSystemTime == STARTUP_RESET_TIME) {
		nodeErrors &= ~PRIMARY_NODE_RESET_STARTUP;
	}

	// Set the autonomous mode LED dependent on whether we are in autonomous mode, manual override,
	// or regular manual mode. LED is solid for autonomous mode, flashing for manual override, and
	// off for regular manual control.
	if (nodeErrors & PRIMARY_NODE_RESET_MANUAL_OVERRIDE) {
		if (autoModeBlinkCounter == 0) {
			_LATB12 = ON;
			autoModeBlinkCounter = 1;
		} else if (autoModeBlinkCounter == 24) {
			_LATB12 = OFF;
			++autoModeBlinkCounter;
		} else if (autoModeBlinkCounter == 48) {
			autoModeBlinkCounter = 0;
		} else {
			++autoModeBlinkCounter;
		}
	} else if (nodeStatus & PRIMARY_NODE_STATUS_AUTOMODE) {
		_LATB12 = ON;
		autoModeBlinkCounter = 0;
	} else {
		_LATB12 = OFF;
		autoModeBlinkCounter = 0;
	}

	// Update the onboard system time counter.
	++nodeSystemTime;

	// Update the internal counter.
	if (internalCounter == 99) {
		internalCounter = 0;
	} else {
		++internalCounter;
	}
}

void PrimaryNodeAdcInit(void)
{
	// FIXME: Add ADC initialization here.
}