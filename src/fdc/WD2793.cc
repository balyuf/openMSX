// $Id$

#include "WD2793.hh"
#include "DiskDrive.hh"
#include "MSXException.hh"
#include "serialize.hh"
#include "unreachable.hh"
#include <cstring>

namespace openmsx {

// Status register
static const int BUSY             = 0x01;
static const int INDEX            = 0x02;
static const int S_DRQ            = 0x02;
static const int TRACK00          = 0x04;
static const int LOST_DATA        = 0x04;
static const int CRC_ERROR        = 0x08;
static const int SEEK_ERROR       = 0x10;
static const int RECORD_NOT_FOUND = 0x10;
static const int HEAD_LOADED      = 0x20;
static const int RECORD_TYPE      = 0x20;
static const int WRITE_PROTECTED  = 0x40;
static const int NOT_READY        = 0x80;

// Command register
static const int STEP_SPEED = 0x03;
static const int V_FLAG     = 0x04;
static const int E_FLAG     = 0x04;
static const int H_FLAG     = 0x08;
static const int T_FLAG     = 0x10;
static const int M_FLAG     = 0x10;
static const int N2R_IRQ    = 0x01;
static const int R2N_IRQ    = 0x02;
static const int IDX_IRQ    = 0x04;
static const int IMM_IRQ    = 0x08;

// Sync point types
enum SyncPointType { SCHED_FSM, SCHED_IDX_IRQ };

WD2793::WD2793(Scheduler& scheduler, DiskDrive& drive_, EmuTime::param time)
	: Schedulable(scheduler)
	, drive(drive_)
	, commandStart(time)
	, DRQTimer(time)
{
	// avoid (harmless) UMR in serialize()
	memset(dataBuffer, 0, sizeof(dataBuffer));
	dataCurrent = 0;
	dataAvailable = 0;

	reset(time);
}

void WD2793::reset(EmuTime::param time)
{
	removeSyncPoint(SCHED_FSM);
	removeSyncPoint(SCHED_IDX_IRQ);
	fsmState = FSM_NONE;

	statusReg = 0;
	trackReg = 0;
	dataReg = 0;
	directionIn = true;

	setDRQ(false, time);
	resetIRQ();
	immediateIRQ = false;

	formatting = false;
	transferring = false;

	// Execute Restore command
	sectorReg = 0x01;
	setCommandReg(0x03, time);
}

bool WD2793::getDTRQ(EmuTime::param time)
{
	if (((commandReg & 0xC0) == 0x80) && (statusReg & BUSY)) {
		// read/write sector cmd busy
		if (transferring) {
			int ticks = DRQTimer.getTicksTill(time);
			if (ticks >= 15) {
				DRQ = true;
			}
		}
	} else if (((commandReg & 0xF0) == 0xF0) && (statusReg & BUSY)) {
		// WRITE TRACK cmd busy
		switch (drive.indexPulseCount(commandStart, time)) {
		case 0: // no index pulse yet
			break;
		case 1: // first index pulse passed
			if (DRQTimer.getTicksTill(time) >= 16) {
				// '16' found by trial and error
				// TODO implement something based on RAWTRACK_SIZE
				DRQ = true;
			}
			break;
		default: // next indexpulse passed
			endWriteTrackCmd();
			break;
		}
	}
	//PRT_DEBUG("WD2793::getDTRQ() " << DRQ);
	return DRQ;
}

bool WD2793::peekDTRQ(EmuTime::param /*time*/)
{
	return DRQ; // TODO can be improved
}

bool WD2793::getIRQ(EmuTime::param /*time*/)
{
	//PRT_DEBUG("WD2793::getIRQ() " << INTRQ);
	return INTRQ | immediateIRQ;
}

bool WD2793::peekIRQ(EmuTime::param time)
{
	return getIRQ(time);
}

void WD2793::setIRQ()
{
	INTRQ = true;
}

void WD2793::resetIRQ()
{
	INTRQ = false;
}

void WD2793::setDRQ(bool drq, EmuTime::param time)
{
	DRQ = drq;
	DRQTimer.advance(time);
}

void WD2793::setCommandReg(byte value, EmuTime::param time)
{
	//PRT_DEBUG("WD2793::setCommandReg() 0x" << std::hex << (int)value);
	removeSyncPoint(SCHED_FSM);

	commandReg = value;
	resetIRQ();
	transferring = false;
	switch (commandReg & 0xF0) {
		case 0x00: // restore
		case 0x10: // seek
		case 0x20: // step
		case 0x30: // step (Update trackRegister)
		case 0x40: // step-in
		case 0x50: // step-in (Update trackRegister)
		case 0x60: // step-out
		case 0x70: // step-out (Update trackRegister)
			startType1Cmd(time);
			break;

		case 0x80: // read sector
		case 0x90: // read sector (multi)
		case 0xA0: // write sector
		case 0xB0: // write sector (multi)
			startType2Cmd(time);
			break;

		case 0xC0: // Read Address
		case 0xE0: // read track
		case 0xF0: // write track
			startType3Cmd(time);
			break;

		case 0xD0: // Force interrupt
			startType4Cmd(time);
			break;
	}
}

byte WD2793::getStatusReg(EmuTime::param time)
{
	if (((commandReg & 0x80) == 0) || ((commandReg & 0xF0) == 0xD0)) {
		// Type I or type IV command
		statusReg &= ~(INDEX | TRACK00 | HEAD_LOADED | WRITE_PROTECTED);
		if (drive.indexPulse(time)) {
			statusReg |=  INDEX;
		}
		if (drive.isTrack00()) {
			statusReg |=  TRACK00;
		}
		if (drive.headLoaded(time)) {
			statusReg |=  HEAD_LOADED;
		}
		if (drive.isWriteProtected()) {
			statusReg |=  WRITE_PROTECTED;
		}
	} else {
		// Not type I command so bit 1 should be DRQ
		if (getDTRQ(time)) {
			statusReg |=  S_DRQ;
		} else {
			statusReg &= ~S_DRQ;
		}
	}

	if (drive.isDiskInserted()) {
		statusReg &= ~NOT_READY;
	} else {
		statusReg |=  NOT_READY;
	}

	resetIRQ();
	//PRT_DEBUG("WD2793::getStatusReg() 0x" << std::hex << (int)statusReg);
	return statusReg;
}

byte WD2793::peekStatusReg(EmuTime::param time)
{
	return getStatusReg(time);
}

void WD2793::setTrackReg(byte value, EmuTime::param /*time*/)
{
	//PRT_DEBUG("WD2793::setTrackReg() 0x" << std::hex << (int)value);
	trackReg = value;
}

byte WD2793::getTrackReg(EmuTime::param /*time*/)
{
	return trackReg;
}

byte WD2793::peekTrackReg (EmuTime::param time)
{
	return getTrackReg(time);
}

void WD2793::setSectorReg(byte value, EmuTime::param /*time*/)
{
	//PRT_DEBUG("WD2793::setSectorReg() 0x" << std::hex << (int)value);
	sectorReg = value;
}

byte WD2793::getSectorReg(EmuTime::param /*time*/)
{
	return sectorReg;
}

byte WD2793::peekSectorReg(EmuTime::param time)
{
	return getSectorReg(time);
}

void WD2793::setDataReg(byte value, EmuTime::param time)
{
	//PRT_DEBUG("WD2793::setDataReg() 0x" << std::hex << (int)value);
	// TODO Is this also true in case of sector write?
	//      Not so according to ASM of brMSX
	dataReg = value;
	if (((commandReg & 0xE0) == 0xA0) && (statusReg & BUSY)) {
		// WRITE SECTOR
		dataBuffer[dataCurrent] = value;
		dataCurrent++;
		dataAvailable--;
		setDRQ(false, time);
		if (dataAvailable == 0) {
			transferring = false;
			PRT_DEBUG("WD2793: Now we call the backend to write a sector");
			try {
				dataCurrent = 0;
				byte onDiskTrack, onDiskSector, onDiskSide;
				int  onDiskSize;
				drive.write(sectorReg, dataBuffer,
				            onDiskTrack, onDiskSector,
				            onDiskSide, onDiskSize);
				dataAvailable = onDiskSize;
				if (onDiskTrack != trackReg) {
					// TODO we should wait for 6 index holes
					PRT_DEBUG("WD2793: Record not found");
					statusReg |= RECORD_NOT_FOUND;
					endCmd();
					return;
				}
				assert(onDiskSize == 512);
				// If we wait too long we should also write a
				// partialy filled sector ofcourse and set the
				// correct status bits!
				if (!(commandReg & M_FLAG)) {
					endCmd();
				} else {
					// TODO multi sector write
					endCmd();
				}
			} catch (MSXException&) {
				// Backend couldn't write data
				// TODO which status bit should be set?
				statusReg |= RECORD_NOT_FOUND;
				endCmd();
				return;
			}
		}
	} else if (((commandReg & 0xF0) == 0xF0) && (statusReg & BUSY)) {
		// WRITE TRACK
		if (!formatting) {
			return;
		}
		setDRQ(false, time);

		// indexmark related timing
		switch (drive.indexPulseCount(commandStart, time)) {
		case 0: // no index pulse yet
			break;
		case 1: // first index pulse passed
			assert(dataCurrent < Disk::RAWTRACK_SIZE);
			dataBuffer[dataCurrent++] = value;
			break;
		default: // next indexpulse passed
			endWriteTrackCmd();
			break;
		}
		/* followin switch stement belongs in the backend, since
		 * we do not know how the actual diskimage stores the
		 * data. It might simply drop all the extra CRC/header
		 * stuff and just use some of the switches to actually
		 * simply write a 512 bytes sector.
		 *
		 * However, timing should be done here :-\
		 *

		 switch (value) {
		 case 0xFE:
		 case 0xFD:
		 case 0xFC:
		 case 0xFB:
		 case 0xFA:
		 case 0xF9:
		 case 0xF8:
			PRT_DEBUG("CRC generator initializing");
			break;
		 case 0xF6:
			PRT_DEBUG("write C2 ?");
			break;
		 case 0xF5:
			PRT_DEBUG("CRC generator initializing in MFM, write A1?");
			break;
		 case 0xF7:
			PRT_DEBUG("two CRC characters");
			break;
		 default:
			// Normal write to track
			break;
		 }
		 // shouldn't be done here!!
		 statusReg &= ~0x03; // reset status on Busy and DRQ
		 */


		/*
		   if (indexmark) {
		   statusReg &= ~0x03; // reset status on Busy and DRQ
		   setIRQ();
		   DRQ = false;
		   }
		 */
	}
}

byte WD2793::getDataReg(EmuTime::param time)
{
	if (((commandReg & 0xE0) == 0x80) && (statusReg & BUSY)) {
		// READ SECTOR
		dataReg = dataBuffer[dataCurrent];
		dataCurrent++;
		dataAvailable--;
		setDRQ(false, time);
		if (dataAvailable == 0) {
			transferring = false;
			if (!(commandReg & M_FLAG)) {
				endCmd();
			} else {
				// TODO ceck in tech data (or on real machine)
				// if implementation multi sector read is
				// correct, since this is programmed by hart.
				sectorReg++;
				tryToReadSector();
			}
		}
	}
	return dataReg;
}

byte WD2793::peekDataReg(EmuTime::param /*time*/)
{
	if (((commandReg & 0xE0) == 0x80) && (statusReg & BUSY)) {
		// READ SECTOR
		return dataBuffer[dataCurrent];
	} else {
		return dataReg;
	}
}

void WD2793::tryToReadSector()
{
	try {
		byte onDiskTrack, onDiskSector, onDiskSide;
		int  onDiskSize;
		drive.read(sectorReg, dataBuffer,
		           onDiskTrack, onDiskSector, onDiskSide, onDiskSize);
		if (onDiskTrack != trackReg) {
			// TODO we should wait for 6 index holes
			statusReg |= RECORD_NOT_FOUND;
			endCmd();
			return;
		}
		assert(onDiskSize == 512);
		dataCurrent = 0;
		dataAvailable = onDiskSize;
		DRQ = false;
		transferring = true;
	} catch (MSXException& e) {
		PRT_DEBUG("WD2793: read sector failed: " << e.getMessage());
		(void)&e; // Prevent warning
		DRQ = false; // TODO data not ready (read error)
		statusReg = 0; // reset flags
	}
}


void WD2793::schedule(FSMState state, EmuTime::param time)
{
	assert(!pendingSyncPoint(SCHED_FSM));
	fsmState = state;
	setSyncPoint(time, SCHED_FSM);
}

void WD2793::executeUntil(EmuTime::param time, int userData)
{
	if (userData == SCHED_IDX_IRQ) {
		INTRQ = true;
		return;
	}

	assert(userData == SCHED_FSM);
	FSMState state = fsmState;
	fsmState = FSM_NONE;
	switch (state) {
		case FSM_SEEK:
			if ((commandReg & 0x80) == 0x00) {
				// Type I command
				seekNext(time);
			}
			break;
		case FSM_TYPE2_WAIT_LOAD:
			if ((commandReg & 0xC0) == 0x80)  {
				// Type II command
				type2WaitLoad(time);
			}
			break;
		case FSM_TYPE2_LOADED:
			if ((commandReg & 0xC0) == 0x80)  {
				// Type II command
				type2Loaded(time);
			}
			break;
		case FSM_TYPE2_ROTATED:
			if ((commandReg & 0xC0) == 0x80)  {
				// Type II command
				type2Rotated();
			}
			break;
		case FSM_TYPE3_WAIT_LOAD:
			if (((commandReg & 0xC0) == 0xC0) &&
			    ((commandReg & 0xF0) != 0xD0)) {
				// Type III command
				type3WaitLoad(time);
			}
			break;
		case FSM_TYPE3_LOADED:
			if (((commandReg & 0xC0) == 0xC0) &&
			    ((commandReg & 0xF0) != 0xD0)) {
				// Type III command
				type3Loaded(time);
			}
			break;
		default:
			UNREACHABLE;
	}
}

void WD2793::startType1Cmd(EmuTime::param time)
{
	statusReg &= ~(SEEK_ERROR | CRC_ERROR);
	statusReg |= BUSY;
	setDRQ(false, time);

	drive.setHeadLoaded((commandReg & H_FLAG) != 0, time);

	switch (commandReg & 0xF0) {
		case 0x00: // restore
			trackReg = 0xFF;
			dataReg  = 0x00;
			seek(time);
			break;

		case 0x10: // seek
			seek(time);
			break;

		case 0x20: // step
		case 0x30: // step (Update trackRegister)
			step(time);
			break;

		case 0x40: // step-in
		case 0x50: // step-in (Update trackRegister)
			directionIn = true;
			step(time);
			break;

		case 0x60: // step-out
		case 0x70: // step-out (Update trackRegister)
			directionIn = false;
			step(time);
			break;
	}
}

void WD2793::seek(EmuTime::param time)
{
	if (trackReg == dataReg) {
		endType1Cmd();
	} else {
		directionIn = (dataReg > trackReg);
		step(time);
	}
}

void WD2793::step(EmuTime::param time)
{
	const int timePerStep[4] = {
		// in ms, in case a 1MHz clock is used (as in MSX)
		6, 12, 20, 30
	};

	if ((commandReg & T_FLAG) || ((commandReg & 0xE0) == 0x00)) {
		// Restore or seek  or  T_FLAG
		if (directionIn) {
			trackReg++;
		} else {
			trackReg--;
		}
	}
	if (!directionIn && drive.isTrack00()) {
		trackReg = 0;
		endType1Cmd();
	} else {
		drive.step(directionIn, time);
		Clock<1000> next(time); // ms
		next += timePerStep[commandReg & STEP_SPEED];
		schedule(FSM_SEEK, next.getTime());
	}
}

void WD2793::seekNext(EmuTime::param time)
{
	if ((commandReg & 0xE0) == 0x00) {
		// Restore or seek
		seek(time);
	} else {
		endType1Cmd();
	}
}

void WD2793::endType1Cmd()
{
	if (commandReg & V_FLAG) {
		// verify sequence
		// TODO verify sequence
	}
	endCmd();
}


void WD2793::startType2Cmd(EmuTime::param time)
{
	statusReg &= ~(LOST_DATA   | RECORD_NOT_FOUND |
	               RECORD_TYPE | WRITE_PROTECTED);
	statusReg |= BUSY;
	setDRQ(false, time);

	if (!drive.isDiskInserted()) {
		endCmd();
	} else {
		// WD2795/WD2797 would now set SSO output
		drive.setHeadLoaded(true, time);

		if (commandReg & E_FLAG) {
			Clock<1000> next(time); // ms
			next += 30; // when 1MHz clock
			schedule(FSM_TYPE2_WAIT_LOAD, next.getTime());
		} else {
			type2WaitLoad(time);
		}
	}
}

void WD2793::type2WaitLoad(EmuTime::param time)
{
	// TODO wait till head loaded, I arbitrarily took 1ms delay
	Clock<1000> next(time);
	next += 1;
	schedule(FSM_TYPE2_LOADED, next.getTime());
}

void WD2793::type2Loaded(EmuTime::param time)
{
	if (((commandReg & 0xE0) == 0xA0) && (drive.isWriteProtected())) {
		// write command and write protected
		PRT_DEBUG("WD2793: write protected");
		statusReg |= WRITE_PROTECTED;
		endCmd();
	} else {
		EmuTime next = drive.getTimeTillSector(sectorReg, time);
		schedule(FSM_TYPE2_ROTATED, next);
	}
}

void WD2793::type2Rotated()
{
	switch (commandReg & 0xF0) {
		case 0x80: // read sector
		case 0x90: // read sector (multi)
			tryToReadSector();
			break;

		case 0xA0: // write sector
		case 0xB0: // write sector (multi)
			dataCurrent = 0;
			dataAvailable = 512; // TODO should come from sector header
			DRQ = true; // data ready to be written
			transferring = true;
			break;
	}
}

void WD2793::startType3Cmd(EmuTime::param time)
{
	//PRT_DEBUG("WD2793 start type 3 command");
	statusReg &= ~(LOST_DATA | RECORD_NOT_FOUND | RECORD_TYPE);
	statusReg |= BUSY;
	setDRQ(false, time);
	commandStart = time; // done again later

	if (!drive.isDiskInserted()) {
		endCmd();
	} else {
		drive.setHeadLoaded(true, time);
		// WD2795/WD2797 would now set SSO output

		if (commandReg & E_FLAG) {
			Clock<1000> next(time); // ms
			next += 30; // when 1MHz clock
			schedule(FSM_TYPE3_WAIT_LOAD, next.getTime());
		} else {
			type3WaitLoad(time);
		}
	}
}

void WD2793::type3WaitLoad(EmuTime::param time)
{
	// TODO wait till head loaded, I arbitrarily took 1ms delay
	Clock<1000> next(time);
	next += 1;
	schedule(FSM_TYPE3_LOADED, next.getTime());
}

void WD2793::type3Loaded(EmuTime::param time)
{

	// TODO TG43 update

	commandStart = time;
	switch (commandReg & 0xF0) {
	case 0xC0: // read Address
		readAddressCmd();
		break;
	case 0xE0: // read track
		readTrackCmd();
		break;
	case 0xF0: // write track
		writeTrackCmd(time);
		break;
	}
}

void WD2793::readAddressCmd()
{
	PRT_DEBUG("WD2793 command: read address  NOT YET IMPLEMENTED");
	endCmd();
}

void WD2793::readTrackCmd()
{
	PRT_DEBUG("WD2793 command: read track   NOT YET IMPLEMENTED");
	endCmd();
}

void WD2793::writeTrackCmd(EmuTime::param time)
{
	PRT_DEBUG("WD2793 command: write track");

	if (drive.isWriteProtected()) {
		// write track command and write protected
		PRT_DEBUG("WD2793: write protected");
		statusReg |= WRITE_PROTECTED;
		endCmd();
	} else {
		// TODO wait for indexPulse
		formatting = true;
		dataCurrent = 0;
		memset(dataBuffer, 0, Disk::RAWTRACK_SIZE);
		setDRQ(true, time);
	}
}

void WD2793::endWriteTrackCmd()
{
	try {
		drive.writeTrackData(dataBuffer);
	} catch (MSXException&) {
		// Ignore. Should rarely happen, because
		// write-protected is already checked at the
		// beginning of write-track command (maybe
		// when disk is swapped during format)
	}
	dataAvailable = 0; // return correct DTR
	dataCurrent = 0;
	DRQ = false;
	formatting = false;
	endCmd();
}

void WD2793::startType4Cmd(EmuTime::param time)
{
	// Force interrupt
	PRT_DEBUG("WD2793 command: Force interrupt");

	byte flags = commandReg & 0x0F;
	if (flags & (N2R_IRQ | R2N_IRQ)) {
		// all flags not yet supported
		PRT_DEBUG("WD2793 type 4 cmd, unimplemented bits " << int(flags));
	}

	if (flags == 0x00) {
		immediateIRQ = false;
	}
	if ((flags & IDX_IRQ) && drive.isDiskInserted()) {
		setSyncPoint(drive.getTimeTillIndexPulse(time), SCHED_IDX_IRQ);
	} else {
		removeSyncPoint(SCHED_IDX_IRQ);
	}
	if (flags & IMM_IRQ) {
		immediateIRQ = true;
	}

	setDRQ(false, time);
	statusReg &= ~BUSY; // reset status on Busy
}

void WD2793::endCmd()
{
	setIRQ();
	statusReg &= ~BUSY;
}


static enum_string<WD2793::FSMState> fsmStateInfo[] = {
	{ "NONE",            WD2793::FSM_NONE },
	{ "SEEK",            WD2793::FSM_SEEK },
	{ "TYPE2_WAIT_LOAD", WD2793::FSM_TYPE2_WAIT_LOAD },
	{ "TYPE2_LOADED",    WD2793::FSM_TYPE2_LOADED },
	{ "TYPE2_ROTATED",   WD2793::FSM_TYPE2_ROTATED },
	{ "TYPE3_WAIT_LOAD", WD2793::FSM_TYPE3_WAIT_LOAD },
	{ "TYPE3_LOADED",    WD2793::FSM_TYPE3_LOADED },
	{ "IDX_IRQ",         WD2793::FSM_IDX_IRQ }
};
SERIALIZE_ENUM(WD2793::FSMState, fsmStateInfo);

template<typename Archive>
void WD2793::serialize(Archive& ar, unsigned /*version*/)
{
	ar.template serializeBase<Schedulable>(*this);

	ar.serialize("commandStart", commandStart);
	ar.serialize("DRQTimer", DRQTimer);

	ar.serialize("fsmState", fsmState);
	ar.serialize("statusReg", statusReg);
	ar.serialize("commandReg", commandReg);
	ar.serialize("sectorReg", sectorReg);
	ar.serialize("trackReg", trackReg);
	ar.serialize("dataReg", dataReg);

	ar.serialize("directionIn", directionIn);
	ar.serialize("INTRQ", INTRQ);
	ar.serialize("immediateIRQ", immediateIRQ);
	ar.serialize("DRQ", DRQ);
	ar.serialize("transferring", transferring);
	ar.serialize("formatting", formatting);

	ar.serialize_blob("dataBuffer", dataBuffer, sizeof(dataBuffer));
	ar.serialize("dataCurrent", dataCurrent);
	ar.serialize("dataAvailable", dataAvailable);
}
INSTANTIATE_SERIALIZE_METHODS(WD2793);

} // namespace openmsx

