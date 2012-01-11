/*
The MIT License

Copyright (c) 2010 UCSC Autonomous Systems Lab

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*/

// ==============================================================
// This code provides a protocol decoder for the binary communications
// protocol used between the groundstation/HIL and the dsPIC in
// the Autoboat project. As most programming relies on Simulink and
// the Real-Time Workshop, retrieval functions here return arrays of
// data to be compatible (instead of much-nicer structs).
// A complete structure is passed byte-by-byte and assembled in an 
// internal buffer. This is then verified by its checksum and the 
//data pushed into the appropriate struct. This data can then be 
// retrieved via an accessor function.
//
// While this code was written specifically for the Autoboat and its
// protocol, it has been kept as modular as possible to be useful
// in other situations with the most minimal alterations.
// 
// Code by: Bryant W. Mairs
// First Revision: Aug 25 2010
// ==============================================================

#include "commProtocol.h"
#include <string.h>
#include "uart1.h"
#include "uart2.h"
#include "Rudder.h"
#include "types.h"

// Declaration of the relevant message structs used.
static struct {
	unsigned char newData;
	tUnsignedShortToChar timestamp;
} tHilData;

// This is the value of the BRG register for configuring different baud
// rates.
#define BAUD4800_BRG_REG 520
#define BAUD57600_BRG_REG 42
#define BAUD115200_BRG_REG 21

// These are local declarations of each of the message structs.
// They're populated with relevant data by buildAndcheckMessage().

// Keep track of how many messages were successfully received.
static unsigned long receivedMessageCount = 0;
// Keep track of how many fails we've run into
static unsigned long failedMessageCount = 0;
static unsigned char sameFailedMessageFlag = 0;

void cpInitCommunications() {
	//initUart1(BAUD57600_BRG_REG);
	initUart1(BAUD115200_BRG_REG);
	initUart2(BAUD4800_BRG_REG);
}

/**
 * This function builds a full message internally byte-by-byte,
 * verifies its checksum, and then pushes that data into the
 * appropriate struct.
 */
void buildAndCheckMessage(unsigned char characterIn) {
	static unsigned char message[64];
	static unsigned char messageIndex;
	static unsigned char messageState;

	// This contains the function's state of whether
	// it is currently building a message.
	// 0 - Awaiting header byte 0 (%)
	// 1 - Awaiting header byte 1 (&)
	// 2 - Building message
	// 3 - Awaiting header byte 0 (^)
	// 4 - Awaiting header byte 1 (&)
	// 5 - Reading checksum character
	
	// We start recording a new message if we see the header
	if (messageState == 0) {
		if (characterIn == '%') {
			message[messageIndex] = characterIn;
			messageIndex++;
			messageState = 1;
		} else {
			messageIndex = 0;
			messageState = 0;
			
			// Here we've failed parsing a message so count another failure.
			if (!sameFailedMessageFlag) {
				failedMessageCount++;
				sameFailedMessageFlag = 1;
			}
		}
	} else if (messageState == 1) {
		// If we don't find the necessary ampersand we start over
		// waiting for a new sentence
		if (characterIn == '&') {
			message[messageIndex] = characterIn;
			messageIndex++;
			messageState = 2;

		} else if (characterIn != '%'){
			messageIndex = 0;
			messageState = 0;
			
			// Here we've failed parsing a message so count another failure.
			if (!sameFailedMessageFlag) {
				failedMessageCount++;
				sameFailedMessageFlag = 1;
			}
		}
	} else if (messageState == 2) {
		// Record every character that comes in now that we're building a sentence.
		// Stop scanning once we've reached the message length of characters.
		message[messageIndex++] = characterIn;
		if (messageIndex > 3 && messageIndex == message[3] + 5) {
			if (characterIn == '^') {
				messageState = 3;
			} else {
				messageState = 0;
				messageIndex = 0;
				
				// Here we've failed parsing a message.
				failedMessageCount++;
				sameFailedMessageFlag = 1;
			}
		} else if (messageIndex == sizeof(message) - 3) {
			// If we've filled up the buffer, ignore the entire message as we can't store it all
			messageState = 0;
			messageIndex = 0;
			
			// Here we've failed parsing a message.
			failedMessageCount++;
			sameFailedMessageFlag = 1;
		}
	} else if (messageState == 3) {
		// If we don't find the necessary ampersand we continue
		// recording data as we haven't found the footer yet until
		// we've filled up the entire message (ends at 124 characters
		// as we need room for the 2 footer chars).
		message[messageIndex++] = characterIn;
		if (characterIn == '&') {
			messageState = 4;
		} else if (messageIndex == sizeof(message) - 2) {
			messageState = 0;
			messageIndex = 0;
		}
	} else if (messageState == 4) {
		// Record the checksum byte.
		message[messageIndex] = characterIn;

		// The checksum is now verified and if successful the message
		// is stored in the appropriate struct.
		if (message[messageIndex] == calculateChecksum(&message[2], messageIndex - 4)) {
			// We now memcpy all the data into our global data structs.
			// NOTE: message[4] is used to skip the header, message ID, and size chars.
			receivedMessageCount++;
			if (message[2] == 1) {
				// FIXME: We skip data 4 & 5 as it's unnecessary thorttle data.
				SetGpsData(&message[6]);
				SetRudderData(&message[33]);
				SetHilData(&message[41]);
			}

			// Now that we've successfully parsed a message, clear the flag.
			sameFailedMessageFlag = 0;
		} else {
			// Here we've failed parsing a message.
			failedMessageCount++;
			sameFailedMessageFlag = 1;
		}

		// We clear all state variables here regardless of success.
		messageIndex = 0;
		messageState = 0;
		int b;
		for (b = 0; b < sizeof(message); b++) {
			message[b] = 0;
		}
	}
}

/**
 * This function should be called continously. Each timestep
 * it runs through the most recently received data, parsing
 * it for sensor data. Once a complete message has been parsed
 * the data inside will be returned through the message
 * array.
 */
void processNewCommData(unsigned char* message) {
	while (GetLength(&uart2RxBuffer) > 0) {
		unsigned char c;
		Read(&uart2RxBuffer, &c);
		buildAndCheckMessage(c);
	}
}

/**
 * This function sets the proper UART2
 * baud rate depending on the HIL mode.
 * A mode value of 0 will set the baud
 * rate back to the default 1200. A value
 * of 1 will set it to 115200.
 */
void setHilMode(unsigned char mode) {
	static unsigned char oldMode = 0;

	// Detect a change to HIL
	if (!oldMode && mode) {
		changeUart2BaudRate(BAUD115200_BRG_REG);
		oldMode = mode;
	} else if (oldMode && !mode) {
		changeUart2BaudRate(BAUD4800_BRG_REG);
		oldMode = mode;
	}
}

inline void enableHil() {
	setHilMode(1);
}

inline void disableHil() {
	setHilMode(0);
}

/**
 * This function calculates the checksum of some bytes in an
 * array by XORing all of them.
 */
unsigned char calculateChecksum(unsigned char* sentence, unsigned char size) {

	unsigned char checkSum = 0;
	unsigned char i;
	for (i = 0; i < size; i++) {
		checkSum ^= sentence[i];
	}

	return checkSum;
}

void SetHilData(unsigned char *data) {
	tHilData.timestamp.chData[0] = data[0];
	tHilData.timestamp.chData[1] = data[1];
	tHilData.newData = 1;
}

unsigned short GetCurrentTimestamp() {
	return tHilData.timestamp.usData;
}

unsigned char IsNewHilData() {
	return tHilData.newData;
}

/**
 * Add all 27 data + 7 header/footer bytes of the actuator struct to UART2's transmission queue.
 */
inline void uart2EnqueueActuatorData(unsigned char *data) {
	uart2EnqueueData(data, 34);
}

/**
 * Add all 137 data + 7 header/footer bytes of the actuator struct to UART1's transmission queue.
 */
inline void uart1EnqueueStateData(unsigned char *data) {
	uart1EnqueueData(data, 160);
}
