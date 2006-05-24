// $Id$

#ifndef ABSTRACTIDEDEVICE_HH
#define ABSTRACTIDEDEVICE_HH

#include "IDEDevice.hh"
#include <string>

namespace openmsx {

class EventDistributor;
class XMLElement;

class AbstractIDEDevice : public IDEDevice
{
public:
	virtual void reset(const EmuTime& time);

	virtual word readData(const EmuTime& time);
	virtual byte readReg(nibble reg, const EmuTime& time);

	virtual void writeData(word value, const EmuTime& time);
	virtual void writeReg(nibble reg, byte value, const EmuTime& time);

protected:
	// Bit flags for the status register:
	static const byte DRDY = 0x40;
	static const byte DSC = 0x10;
	static const byte DRQ = 0x08;
	static const byte ERR = 0x01;

	// Bit flags for the error register:
	static const byte UNC = 0x40;
	static const byte IDNF = 0x10;
	static const byte ABORT = 0x04;

	AbstractIDEDevice(
		EventDistributor& eventDistributor, const EmuTime& time
		);
	virtual ~AbstractIDEDevice();

	/** Is this device a packet (ATAPI) device?
	  * @return True iff this device supports the packet commands.
	  */
	virtual bool isPacketDevice() = 0;

	/** Gets the device name to insert as "model number" into the identify
	  * block.
	  * @return An ASCII string, up to 40 characters long.
	  */
	virtual const std::string& getDeviceName() = 0;

	/** Tells a subclass to fill the device specific parts of the identify
	  * block located in the buffer.
	  * The generic part is already written there.
	  * @param buffer Array of 512 bytes.
	  */
	virtual void fillIdentifyBlock(byte* buffer) = 0;

	/** Called when a block of read data should be buffered by the controller:
	  * when the buffer is empty or at the start of the transfer.
	  * @param buffer Array of 512 bytes.
	  */
	virtual void readBlockStart(byte* buffer) = 0;

	/** Called when a read transfer completes.
	  * The default implementation does nothing.
	  */
	virtual void readEnd();

	/** Called when a block of written data has been buffered by the controller:
	  * when the buffer is full or at the end of the transfer.
	  * @param buffer Array of 512 bytes.
	  */
	virtual void writeBlockComplete(byte* buffer) = 0;

	/** Starts execution of an IDE command.
	  * Override this to implement additional commands and make sure you call
	  * the superclass implementation for all commands that you don't handle.
	  */
	virtual void executeCommand(byte cmd);

	/** Indicates an error: sets error register, error flag, aborts transfers.
	  * @param error Value to be written to the error register.
	  */
	void setError(byte error);

	/** Creates an LBA sector address from the contents of the sectorNumReg,
	  * cylinderLowReg, cylinderHighReg and devHeadReg registers.
	  */
	unsigned getSectorNumber() const;

	/** Gets the number of sectors indicated by the sector count register.
	  */
	unsigned getNumSectors() const;

	/** Writes the interrupt reason register.
	  * This is the same as register as sector count, but serves a different
	  * purpose.
	  */
	void setInterruptReason(byte value);

	/** Indicates the start of a read data transfer.
	  * @param count Total number of words to transfer.
	  */
	void startReadTransfer(unsigned count);

	/** Aborts the read transfer in progress.
	  */
	void abortReadTransfer(byte error);

	/** Indicates the start of a write data transfer.
	  * @param count Total number of words to transfer.
	  */
	void startWriteTransfer(unsigned count);

	/** Aborts the write transfer in progress.
	  */
	void abortWriteTransfer(byte error);

private:
	/** Perform diagnostic and return result.
	  * Actually, just return success, because we don't emulate faulty hardware.
	  */
	byte diagnostic();

	/** Puts special values in the sector address, sector count and device
	  * registers to identify the type of device.
	  * @param preserveDevice If true, preserve the value of the DEV bit;
	  *   if false, set the DEV bit to 0.
	  */
	void createSignature(bool preserveDevice = false);

	/** Puts the output for the IDENTIFY DEVICE command in the buffer.
	  */
	void createIdentifyBlock();

	/** Indicates that a read transfer starts.
	  */
	void setTransferRead(bool status);

	/** Indicates that a write transfer starts.
	  */
	void setTransferWrite(bool status);

	byte errorReg;
	byte sectorCountReg;
	byte sectorNumReg;
	byte cylinderLowReg;
	byte cylinderHighReg;
	byte devHeadReg;
	byte statusReg;
	byte featureReg;
	byte buffer[512];

	/** True iff the current transfer is part of an IDENTIFY DEVICE command.
	  */
	bool transferIdentifyBlock;

	bool transferRead;
	bool transferWrite;
	unsigned transferCount;
	byte* transferPntr;

	EventDistributor& eventDistributor;
};

} // namespace openmsx

#endif // ABSTRACTIDEDEVICE_HH
