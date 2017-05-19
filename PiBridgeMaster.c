//+=============================================================================================
//|
//!             \file PiBridgeSlave.c
//!             Statemachine for PiBridge master modules
//|
//+---------------------------------------------------------------------------------------------
//|
//|             File-ID:                $Id: PiBridgeMaster.c 11244 2016-12-07 09:11:48Z mduckeck $
//|             Location:               $URL: http://srv-kunbus03.de.pilz.local/feldbus/software/trunk/platform/ModGateCom/sw/PiBridgeMaster.c $
//|
//+=============================================================================================

#include <project.h>

#include <common_define.h>
#include <linux/module.h>	// included for all kernel modules
#include <linux/kernel.h>	// included for KERN_INFO
#include <linux/slab.h>		// included for KERN_INFO
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/jiffies.h>
#include <linux/thermal.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#include <kbUtilities.h>
#include <ModGateRS485.h>
#include <PiBridgeMaster.h>
#include <RevPiDevice.h>
#include <piControlMain.h>
#include <piControl.h>
#include <piIOComm.h>
#include <piDIOComm.h>
#include <piAIOComm.h>

static volatile TBOOL bEntering_s = bTRUE;
static EPiBridgeMasterStatus eRunStatus_s = enPiBridgeMasterStatus_Init;
static enPiBridgeState eBridgeStateLast_s = piBridgeStop;

static INT32U i32uFWUAddress, i32uFWUSerialNum, i32uFWUFlashAddr, i32uFWUlength, i8uFWUScanned;
static INT32S i32sRetVal;
static char *pcFWUdata;

#define VCMSG_ID_ARM_CLOCK 0x000000003		/* Clock/Voltage ID's */

static int bcm2835_cpufreq_clock_property(u32 tag, u32 id, u32 *val)
{
	struct rpi_firmware *fw = rpi_firmware_get(NULL);
	struct {
		u32 id;
		u32 val;
	} packet;
	int ret;

	packet.id = id;
	packet.val = *val;
	ret = rpi_firmware_property(fw, tag, &packet, sizeof(packet));
	if (ret)
		return ret;

	*val = packet.val;

	return 0;
}

static uint32_t bcm2835_cpufreq_get_clock(void)
{
	u32 rate;
	int ret;

	ret = bcm2835_cpufreq_clock_property(RPI_FIRMWARE_GET_CLOCK_RATE, VCMSG_ID_ARM_CLOCK, &rate);
	if (ret) {
		pr_err("Failed to get clock (%d)\n", ret);
		return 0;
	}

	rate /= 1000 * 1000; //convert to MHz

	return rate;
}

void PiBridgeMaster_Stop(void)
{
	rt_mutex_lock(&piDev_g.lockBridgeState);
	piDev_g.eBridgeState = piBridgeStop;
	rt_mutex_unlock(&piDev_g.lockBridgeState);
}

void PiBridgeMaster_Continue(void)
{
	// this function can only be called, if the driver was running before
	rt_mutex_lock(&piDev_g.lockBridgeState);
	piDev_g.eBridgeState = piBridgeRun;
	eRunStatus_s = enPiBridgeMasterStatus_Continue;	// make no initialization
	bEntering_s = bFALSE;
	rt_mutex_unlock(&piDev_g.lockBridgeState);
}

void PiBridgeMaster_Reset(void)
{
	rt_mutex_lock(&piDev_g.lockBridgeState);
	piDev_g.eBridgeState = piBridgeInit;
	eRunStatus_s = enPiBridgeMasterStatus_Init;
	RevPiScan.i8uStatus = 0;

	bEntering_s = bTRUE;

	RevPiDevice_init();
	rt_mutex_unlock(&piDev_g.lockBridgeState);
}

int PiBridgeMaster_Adjust(void)
{
	int i, j, done;
	int result = 0, found;
	uint8_t *state;

	if (piDev_g.devs == NULL || piDev_g.ent == NULL) {
		// config file could not be read, do nothing
		return -1;
	}

	state = kcalloc(piDev_g.devs->i16uNumDevices, sizeof(uint8_t), GFP_KERNEL);

	// Schleife über alle Module die automatisch erkannt wurden
	for (j = 0; j < RevPiScan.i8uDeviceCount; j++) {
		// Suche diese Module in der Konfigurationsdatei
		for (i = 0, found = 0, done = 0; found == 0 && i < piDev_g.devs->i16uNumDevices && !done; i++) {
			// Grundvoraussetzung ist, dass die Adresse gleich ist.
			if (RevPiScan.dev[j].i8uAddress == piDev_g.devs->dev[i].i8uAddress) {
				// Außerdem muss ModuleType, InputLength und OutputLength gleich sein.
				if (RevPiScan.dev[j].sId.i16uModulType != piDev_g.devs->dev[i].i16uModuleType) {
					pr_info("## address %d: incorrect module type %d != %d\n",
						  RevPiScan.dev[j].i8uAddress, RevPiScan.dev[j].sId.i16uModulType,
						  piDev_g.devs->dev[i].i16uModuleType);
					result = PICONTROL_CONFIG_ERROR_WRONG_MODULE_TYPE;
					RevPiScan.i8uStatus |= PICONTROL_STATUS_SIZE_MISMATCH;
					done = 1;
					break;
				}
				if (RevPiScan.dev[j].sId.i16uFBS_InputLength != piDev_g.devs->dev[i].i16uInputLength) {
					pr_info("## address %d: incorrect input length %d != %d\n",
						  RevPiScan.dev[j].i8uAddress, RevPiScan.dev[j].sId.i16uFBS_InputLength,
						  piDev_g.devs->dev[i].i16uInputLength);
					result = PICONTROL_CONFIG_ERROR_WRONG_INPUT_LENGTH;
					RevPiScan.i8uStatus |= PICONTROL_STATUS_SIZE_MISMATCH;
					done = 1;
					break;
				}
				if (RevPiScan.dev[j].sId.i16uFBS_OutputLength != piDev_g.devs->dev[i].i16uOutputLength) {
					pr_info("## address %d: incorrect output length %d != %d\n",
						  RevPiScan.dev[j].i8uAddress,
						  RevPiScan.dev[j].sId.i16uFBS_OutputLength,
						  piDev_g.devs->dev[i].i16uOutputLength);
					result = PICONTROL_CONFIG_ERROR_WRONG_OUTPUT_LENGTH;
					RevPiScan.i8uStatus |= PICONTROL_STATUS_SIZE_MISMATCH;
					done = 1;
					break;
				}
				// we found the device in the configuration file
				// -> adjust offsets
				pr_info_master("Adjust: base %d in %d out %d conf %d\n",
					  piDev_g.devs->dev[i].i16uBaseOffset,
					  piDev_g.devs->dev[i].i16uInputOffset,
					  piDev_g.devs->dev[i].i16uOutputOffset, piDev_g.devs->dev[i].i16uConfigOffset);

				RevPiScan.dev[j].i16uInputOffset = piDev_g.devs->dev[i].i16uInputOffset;
				RevPiScan.dev[j].i16uOutputOffset = piDev_g.devs->dev[i].i16uOutputOffset;
				RevPiScan.dev[j].i16uConfigOffset = piDev_g.devs->dev[i].i16uConfigOffset;
				RevPiScan.dev[j].i16uConfigLength = piDev_g.devs->dev[i].i16uConfigLength;
				if (j == 0) {
					RevPiScan.pCoreData =
					    (SRevPiCoreImage *) & piDev_g.ai8uPI[RevPiScan.dev[0].i16uInputOffset];
				}

				state[i] = 1;	// dieser Konfigeintrag wurde übernommen
				found = 1;	// innere For-Schrleife verlassen
			}
		}
		if (found == 0) {
			// Falls ein autom. erkanntes Modul in der Konfiguration nicht vorkommt, wird es deakiviert
			RevPiScan.dev[j].i8uActive = 0;
			RevPiScan.i8uStatus |= PICONTROL_STATUS_EXTRA_MODULE;
		}
	}

	// nun wird die Liste der automatisch erkannten Module um die ergänzt, die nur in der Konfiguration vorkommen.
	for (i = 0; i < piDev_g.devs->i16uNumDevices; i++) {
		if (state[i] == 0) {
			RevPiScan.i8uStatus |= PICONTROL_STATUS_MISSING_MODULE;

			j = RevPiScan.i8uDeviceCount;
			if (piDev_g.devs->dev[i].i16uModuleType >= PICONTROL_SW_OFFSET)
			{
				// if a module is already defined as software module in the RAP file,
				// it is handled by user space software and therefore always active
				RevPiScan.dev[j].i8uActive = 1;
				RevPiScan.dev[j].sId.i16uModulType = piDev_g.devs->dev[i].i16uModuleType;
			}
			else
			{
				RevPiScan.dev[j].i8uActive = 0;
				RevPiScan.dev[j].sId.i16uModulType = piDev_g.devs->dev[i].i16uModuleType | PICONTROL_NOT_CONNECTED;
			}
			RevPiScan.dev[j].i8uAddress = piDev_g.devs->dev[i].i8uAddress;
			RevPiScan.dev[j].i8uScan = 0;
			RevPiScan.dev[j].i16uInputOffset = piDev_g.devs->dev[i].i16uInputOffset;
			RevPiScan.dev[j].i16uOutputOffset = piDev_g.devs->dev[i].i16uOutputOffset;
			RevPiScan.dev[j].i16uConfigOffset = piDev_g.devs->dev[i].i16uConfigOffset;
			RevPiScan.dev[j].i16uConfigLength = piDev_g.devs->dev[i].i16uConfigLength;
			RevPiScan.dev[j].sId.i32uSerialnumber = piDev_g.devs->dev[i].i32uSerialnumber;
			RevPiScan.dev[j].sId.i16uHW_Revision = piDev_g.devs->dev[i].i16uHW_Revision;
			RevPiScan.dev[j].sId.i16uSW_Major = piDev_g.devs->dev[i].i16uSW_Major;
			RevPiScan.dev[j].sId.i16uSW_Minor = piDev_g.devs->dev[i].i16uSW_Minor;
			RevPiScan.dev[j].sId.i32uSVN_Revision = piDev_g.devs->dev[i].i32uSVN_Revision;
			RevPiScan.dev[j].sId.i16uFBS_InputLength = piDev_g.devs->dev[i].i16uInputLength;
			RevPiScan.dev[j].sId.i16uFBS_OutputLength = piDev_g.devs->dev[i].i16uOutputLength;
			RevPiScan.dev[j].sId.i16uFeatureDescriptor = 0;	// not used
			RevPiScan.i8uDeviceCount++;
		}
	}

	kfree(state);
	return result;
}

void PiBridgeMaster_setDefaults(void)
{
	int i;

	if (piDev_g.ent == NULL)
		return;

	memset(piDev_g.ai8uPIDefault, 0, KB_PI_LEN);

	for (i=0; i<piDev_g.ent->i16uNumEntries; i++)
	{
		if (piDev_g.ent->ent[i].i32uDefault != 0)
		{
			pr_info_master2("addr %2d  type %2x  len %3d  offset %3d+%d  default %x\n",
				piDev_g.ent->ent[i].i8uAddress,
				piDev_g.ent->ent[i].i8uType,
				piDev_g.ent->ent[i].i16uBitLength,
				piDev_g.ent->ent[i].i16uOffset,
				piDev_g.ent->ent[i].i8uBitPos,
				piDev_g.ent->ent[i].i32uDefault);

			if (piDev_g.ent->ent[i].i16uBitLength == 1)
			{
				INT8U i8uValue, i8uMask, addr, bit;

				addr = piDev_g.ent->ent[i].i16uOffset;
				bit = piDev_g.ent->ent[i].i8uBitPos;

				addr += bit / 8;
				bit %= 8;

				i8uValue = piDev_g.ai8uPIDefault[addr];

				i8uMask = (1 << bit);
				if (piDev_g.ent->ent[i].i32uDefault != 0)
					i8uValue |= i8uMask;
				else
					i8uValue &= ~i8uMask;
				piDev_g.ai8uPIDefault[addr] = i8uValue;
			}
			else if (piDev_g.ent->ent[i].i16uBitLength == 8)
			{
				piDev_g.ai8uPIDefault[piDev_g.ent->ent[i].i16uOffset] = (INT8U)piDev_g.ent->ent[i].i32uDefault;
			}
			else if (piDev_g.ent->ent[i].i16uBitLength == 16 && piDev_g.ent->ent[i].i16uOffset < KB_PI_LEN-1)
			{
				INT16U *pi16uPtr = (INT16U *)&piDev_g.ai8uPIDefault[piDev_g.ent->ent[i].i16uOffset];

				*pi16uPtr = (INT16U)piDev_g.ent->ent[i].i32uDefault;
			}
			else if (piDev_g.ent->ent[i].i16uBitLength == 32 && piDev_g.ent->ent[i].i16uOffset < KB_PI_LEN-3)
			{
				INT32U *pi32uPtr = (INT32U *)&piDev_g.ai8uPIDefault[piDev_g.ent->ent[i].i16uOffset];

				*pi32uPtr = (INT32U)piDev_g.ent->ent[i].i32uDefault;
			}
		}
	}
}


int PiBridgeMaster_Run(void)
{
	static kbUT_Timer tTimeoutTimer_s;
	static int error_cnt;
	INT8U led;
	static INT8U last_led;
	int ret = 0;
	int i;

	rt_mutex_lock(&piDev_g.lockBridgeState);
	if (piDev_g.eBridgeState != piBridgeStop) {
		switch (eRunStatus_s) {
		case enPiBridgeMasterStatus_Init:	// Do some initializations and go to next state
			if (bEntering_s) {
				pr_info_master("Enter Init State\n");
				bEntering_s = bFALSE;
				// configure PiBridge Sniff lines as input
				piIoComm_writeSniff1A(enGpioValue_Low, enGpioMode_Input);
				piIoComm_writeSniff1B(enGpioValue_Low, enGpioMode_Input);
				piIoComm_writeSniff2A(enGpioValue_Low, enGpioMode_Input);
				piIoComm_writeSniff2B(enGpioValue_Low, enGpioMode_Input);
			}
			eRunStatus_s = enPiBridgeMasterStatus_MasterIsPresentSignalling1;
			bEntering_s = bTRUE;
			break;
			// *****************************************************************************************

		case enPiBridgeMasterStatus_MasterIsPresentSignalling1:
			if (bEntering_s) {
				pr_info_master("Enter PresentSignalling1 State\n");

				bEntering_s = bFALSE;
				piIoComm_writeSniff2A(enGpioValue_High, enGpioMode_Output);
				piIoComm_writeSniff2B(enGpioValue_High, enGpioMode_Output);

				usleep_range(9000, 9000);

				piIoComm_writeSniff2A(enGpioValue_Low, enGpioMode_Input);
				piIoComm_writeSniff2B(enGpioValue_Low, enGpioMode_Input);
				kbUT_TimerStart(&tTimeoutTimer_s, 30);
			}
			if (kbUT_TimerExpired(&tTimeoutTimer_s)) {
				eRunStatus_s = enPiBridgeMasterStatus_InitialSlaveDetectionRight;
				bEntering_s = bTRUE;
			}
			break;
			// *****************************************************************************************

		case enPiBridgeMasterStatus_InitialSlaveDetectionRight:
			if (bEntering_s) {
				pr_info_master("Enter InitialSlaveDetectionRight State\n");
				bEntering_s = bFALSE;
				piIoComm_readSniff1A();
				piIoComm_readSniff1B();
			}
			if (piIoComm_readSniff2B() == enGpioValue_High) {
				eRunStatus_s = enPiBridgeMasterStatus_ConfigRightStart;
				bEntering_s = bTRUE;
			} else {
				eRunStatus_s = enPiBridgeMasterStatus_InitialSlaveDetectionLeft;
				bEntering_s = bTRUE;
			}
			break;
			// *****************************************************************************************

		case enPiBridgeMasterStatus_ConfigRightStart:
			if (bEntering_s) {
				pr_info_master("Enter ConfigRightStart State\n");
				bEntering_s = bFALSE;
				piIoComm_writeSniff1B(enGpioValue_Low, enGpioMode_Output);
				kbUT_TimerStart(&tTimeoutTimer_s, 10);
			}
			if (kbUT_TimerExpired(&tTimeoutTimer_s)) {
				eRunStatus_s = enPiBridgeMasterStatus_ConfigDialogueRight;
				bEntering_s = bTRUE;
			}
			break;
			// *****************************************************************************************

		case enPiBridgeMasterStatus_ConfigDialogueRight:
			if (bEntering_s) {
				pr_info_master("Enter ConfigDialogueRight State\n");
				error_cnt = 0;
				bEntering_s = bFALSE;
			}
			// Write configuration data to the currently selected slave
			if (RevPiDevice_writeNextConfigurationRight() == bFALSE) {
				error_cnt++;
				if (error_cnt > 5) {
					// no more slaves on the right side, configure left slaves
					eRunStatus_s = enPiBridgeMasterStatus_InitialSlaveDetectionLeft;
					bEntering_s = bTRUE;
				}
			} else {
				eRunStatus_s = enPiBridgeMasterStatus_SlaveDetectionRight;
				bEntering_s = bTRUE;
			}
			break;
			// *****************************************************************************************

		case enPiBridgeMasterStatus_SlaveDetectionRight:
			if (bEntering_s) {
				pr_info_master("Enter SlaveDetectionRight State\n");
				bEntering_s = bFALSE;
				kbUT_TimerStart(&tTimeoutTimer_s, 10);
			}
			if (kbUT_TimerExpired(&tTimeoutTimer_s)) {
				if (piIoComm_readSniff2B() == enGpioValue_High) {
					// configure next right slave
					eRunStatus_s = enPiBridgeMasterStatus_ConfigDialogueRight;
					bEntering_s = bTRUE;
				} else {
					// no more slaves on the right side, configure left slaves
					eRunStatus_s = enPiBridgeMasterStatus_InitialSlaveDetectionLeft;
					bEntering_s = bTRUE;
				}
			}
			break;
			// *****************************************************************************************

		case enPiBridgeMasterStatus_InitialSlaveDetectionLeft:
			if (bEntering_s) {
				pr_info_master("Enter InitialSlaveDetectionLeft State\n");
				bEntering_s = bFALSE;
				piIoComm_writeSniff1B(enGpioValue_Low, enGpioMode_Input);
			}
			if (piIoComm_readSniff2A() == enGpioValue_High) {
				// configure first left slave
				eRunStatus_s = enPiBridgeMasterStatus_ConfigLeftStart;
				bEntering_s = bTRUE;
			} else {
				// no slave on the left side
				eRunStatus_s = enPiBridgeMasterStatus_EndOfConfig;
				bEntering_s = bTRUE;
			}
			break;
			// *****************************************************************************************

		case enPiBridgeMasterStatus_ConfigLeftStart:
			if (bEntering_s) {
				pr_info_master("Enter ConfigLeftStart State\n");
				bEntering_s = bFALSE;
				piIoComm_writeSniff1A(enGpioValue_Low, enGpioMode_Output);
				kbUT_TimerStart(&tTimeoutTimer_s, 10);
			}
			if (kbUT_TimerExpired(&tTimeoutTimer_s)) {
				eRunStatus_s = enPiBridgeMasterStatus_ConfigDialogueLeft;
				bEntering_s = bTRUE;
			}
			break;
			// *****************************************************************************************

		case enPiBridgeMasterStatus_ConfigDialogueLeft:
			if (bEntering_s) {
				pr_info_master("Enter ConfigDialogueLeft State\n");
				error_cnt = 0;
				bEntering_s = bFALSE;
			}
			// Write configuration data to the currently selected slave
			if (RevPiDevice_writeNextConfigurationLeft() == bFALSE) {
				error_cnt++;
				if (error_cnt > 5) {
					// no more slaves on the right side, configure left slaves
					eRunStatus_s = enPiBridgeMasterStatus_EndOfConfig;
					bEntering_s = bTRUE;
				}
			} else {
				eRunStatus_s = enPiBridgeMasterStatus_SlaveDetectionLeft;
				bEntering_s = bTRUE;
			}
			break;
			// *****************************************************************************************

		case enPiBridgeMasterStatus_SlaveDetectionLeft:
			if (bEntering_s) {
				pr_info_master("Enter SlaveDetectionLeft State\n");
				bEntering_s = bFALSE;
				kbUT_TimerStart(&tTimeoutTimer_s, 10);
			}
			if (kbUT_TimerExpired(&tTimeoutTimer_s)) {
				if (piIoComm_readSniff2A() == enGpioValue_High) {
					// configure next left slave
					eRunStatus_s = enPiBridgeMasterStatus_ConfigDialogueLeft;
					bEntering_s = bTRUE;
				} else {
					// no more slaves on the left
					eRunStatus_s = enPiBridgeMasterStatus_EndOfConfig;
					bEntering_s = bTRUE;
				}
			}
			break;
			// *****************************************************************************************

		case enPiBridgeMasterStatus_Continue:
			msleep(100);	// wait a while
			pr_info("start data exchange\n");
			RevPiDevice_startDataexchange();
			msleep(110);	// wait a while

			// send config messages
			for (i = 0; i < RevPiScan.i8uDeviceCount; i++) {
				if (RevPiScan.dev[i].i8uActive) {
					switch (RevPiScan.dev[i].sId.i16uModulType) {
					case KUNBUS_FW_DESCR_TYP_PI_DIO_14:
					case KUNBUS_FW_DESCR_TYP_PI_DI_16:
					case KUNBUS_FW_DESCR_TYP_PI_DO_16:
						ret = piDIOComm_Init(i);
						pr_info("piDIOComm_Init done %d\n", ret);
						if (ret != 0)
						{
							// init failed -> deactive module
							pr_err("piDIOComm_Init module %d failed %d\n", RevPiScan.dev[i].i8uAddress, ret);
							RevPiScan.dev[i].i8uActive = 0;
						}
						break;
					case KUNBUS_FW_DESCR_TYP_PI_AIO:
						ret = piAIOComm_Init(i);
						pr_info("piAIOComm_Init done %d\n", ret);
						if (ret != 0)
						{
							// init failed -> deactive module
							pr_err("piAIOComm_Init module %d failed %d\n", RevPiScan.dev[i].i8uAddress, ret);
							RevPiScan.dev[i].i8uActive = 0;
						}
						break;
					}
				}
			}

			eRunStatus_s = enPiBridgeMasterStatus_EndOfConfig;
			bEntering_s = bFALSE;
			break;

		case enPiBridgeMasterStatus_EndOfConfig:
			if (bEntering_s) {
#ifdef DEBUG_MASTER_STATE
				pr_info_master("Enter EndOfConfig State\n\n");
				for (i = 0; i < RevPiScan.i8uDeviceCount; i++) {
					pr_info_master("Device %2d: Addr %d Type %x  Act %d  In %d Out %d\n",
						  i,
						  RevPiScan.dev[i].i8uAddress,
						  RevPiScan.dev[i].sId.i16uModulType,
						  RevPiScan.dev[i].i8uActive,
						  RevPiScan.dev[i].sId.i16uFBS_InputLength,
						  RevPiScan.dev[i].sId.i16uFBS_OutputLength);
					pr_info_master("           input offset  %5d  len %3d\n",
						  RevPiScan.dev[i].i16uInputOffset,
						  RevPiScan.dev[i].sId.i16uFBS_InputLength);
					pr_info_master("           output offset %5d  len %3d\n",
						  RevPiScan.dev[i].i16uOutputOffset,
						  RevPiScan.dev[i].sId.i16uFBS_OutputLength);
					pr_info_master("           serial number %d\n",
						  RevPiScan.dev[i].sId.i32uSerialnumber);
				}

				pr_info_master("\n");
#endif

				piIoComm_writeSniff1A(enGpioValue_Low, enGpioMode_Input);

				PiBridgeMaster_Adjust();

#ifdef DEBUG_MASTER_STATE
				pr_info_master("After Adjustment\n");
				for (i = 0; i < RevPiScan.i8uDeviceCount; i++) {
					pr_info_master("Device %2d: Addr %d Type %x  Act %d  In %d Out %d\n",
						  i,
						  RevPiScan.dev[i].i8uAddress,
						  RevPiScan.dev[i].sId.i16uModulType,
						  RevPiScan.dev[i].i8uActive,
						  RevPiScan.dev[i].sId.i16uFBS_InputLength,
						  RevPiScan.dev[i].sId.i16uFBS_OutputLength);
					pr_info_master("           input offset  %5d  len %3d\n",
						  RevPiScan.dev[i].i16uInputOffset,
						  RevPiScan.dev[i].sId.i16uFBS_InputLength);
					pr_info_master("           output offset %5d  len %3d\n",
						  RevPiScan.dev[i].i16uOutputOffset,
						  RevPiScan.dev[i].sId.i16uFBS_OutputLength);
				}
				pr_info_master("\n");
#endif
				PiBridgeMaster_setDefaults();

				rt_mutex_lock(&piDev_g.lockPI);
				memcpy(piDev_g.ai8uPI, piDev_g.ai8uPIDefault, KB_PI_LEN);
				rt_mutex_unlock(&piDev_g.lockPI);

				msleep(100);	// wait a while
				pr_info("start data exchange\n");
				RevPiDevice_startDataexchange();
				msleep(110);	// wait a while

				// send config messages
				for (i = 0; i < RevPiScan.i8uDeviceCount; i++) {
					if (RevPiScan.dev[i].i8uActive) {
						switch (RevPiScan.dev[i].sId.i16uModulType) {
						case KUNBUS_FW_DESCR_TYP_PI_DIO_14:
						case KUNBUS_FW_DESCR_TYP_PI_DI_16:
						case KUNBUS_FW_DESCR_TYP_PI_DO_16:
							ret = piDIOComm_Init(i);
							pr_info("piDIOComm_Init done %d\n", ret);
							if (ret != 0)
							{
								// init failed -> deactive module
								pr_err("piDIOComm_Init module %d failed %d\n", RevPiScan.dev[i].i8uAddress, ret);
								RevPiScan.dev[i].i8uActive = 0;
							}
							break;
						case KUNBUS_FW_DESCR_TYP_PI_AIO:
							ret = piAIOComm_Init(i);
							pr_info("piAIOComm_Init done %d\n", ret);
							if (ret != 0)
							{
								// init failed -> deactive module
								pr_err("piAIOComm_Init module %d failed %d\n", RevPiScan.dev[i].i8uAddress, ret);
								RevPiScan.dev[i].i8uActive = 0;
							}
							break;
						}
					}
				}
				bEntering_s = bFALSE;
				ret = 0;
			}

			if (RevPiDevice_run()) {
				// an error occured, check error limits
				if (RevPiScan.pCoreData != NULL) {
					if (RevPiScan.pCoreData->i16uRS485ErrorLimit2 > 0
					    && RevPiScan.pCoreData->i16uRS485ErrorLimit2 < RevPiScan.i16uErrorCnt) {
						pr_err("too many communication errors -> set BridgeState to stopped\n");
						piDev_g.eBridgeState = piBridgeStop;
					} else if (RevPiScan.pCoreData->i16uRS485ErrorLimit1 > 0
						   && RevPiScan.pCoreData->i16uRS485ErrorLimit1 <
						   RevPiScan.i16uErrorCnt) {
						// bad communication with inputs -> set inputs to default values
						pr_err("too many communication errors -> set inputs to default\n");
					}
				}
			} else {
				ret = 1;
			}
			if (RevPiScan.pCoreData != NULL) {
				RevPiScan.pCoreData->i16uRS485ErrorCnt = RevPiScan.i16uErrorCnt;
			}
			break;
			// *****************************************************************************************

		default:
			break;

		}

		if (ret && piDev_g.eBridgeState != piBridgeRun) {
			pr_info("set BridgeState to running\n");
			piDev_g.eBridgeState = piBridgeRun;
		}
	} else			// piDev_g.eBridgeState == piBridgeStop
	{
		if (eRunStatus_s == enPiBridgeMasterStatus_EndOfConfig) {
			pr_info("stop data exchange\n");
			ret = piIoComm_gotoGateProtocol();
			pr_info("piIoComm_gotoGateProtocol returned %d\n", ret);
			eRunStatus_s = enPiBridgeMasterStatus_Init;
		} else if (eRunStatus_s == enPiBridgeMasterStatus_FWUMode) {
			if (bEntering_s) {
				if (i8uFWUScanned == 0)
				{
					// old mGates always use 2
					i32uFWUAddress = 2;
				}

				i32sRetVal = piIoComm_gotoFWUMode(i32uFWUAddress);
				pr_info("piIoComm_gotoFWUMode returned %d\n", i32sRetVal);

				if (i32uFWUAddress == RevPiScan.i8uAddressRight-1)
					i32uFWUAddress = 2;	// address must be 2 in the following calls
				else
					i32uFWUAddress = 1;	// address must be 1 in the following calls
				pr_info("using address %d\n", i32uFWUAddress);

				ret = 0;	// do not return errors here
				bEntering_s = bFALSE;
			}
		} else if (eRunStatus_s == enPiBridgeMasterStatus_ProgramSerialNum) {
			if (bEntering_s) {
				i32sRetVal = piIoComm_fwuSetSerNum(i32uFWUAddress, i32uFWUSerialNum);
				pr_info("piIoComm_fwuSetSerNum returned %d\n", i32sRetVal);

				ret = 0;	// do not return errors here
				bEntering_s = bFALSE;
			}
		} else if (eRunStatus_s == enPiBridgeMasterStatus_FWUFlashErase) {
			if (bEntering_s) {
				i32sRetVal = piIoComm_fwuFlashErase(i32uFWUAddress);
				pr_info("piIoComm_fwuFlashErase returned %d\n", i32sRetVal);

				ret = 0;	// do not return errors here
				bEntering_s = bFALSE;
			}
		} else if (eRunStatus_s == enPiBridgeMasterStatus_FWUFlashWrite) {
			if (bEntering_s) {
				INT32U i32uOffset = 0, len;
				i32sRetVal = 0;

				while (i32sRetVal == 0 && i32uFWUlength > 0)
				{
					if (i32uFWUlength > MAX_FWU_DATA_SIZE)
						len = MAX_FWU_DATA_SIZE;
					else
						len = i32uFWUlength;
					i32sRetVal = piIoComm_fwuFlashWrite(i32uFWUAddress, i32uFWUFlashAddr+i32uOffset, pcFWUdata+i32uOffset, len);
					pr_info("piIoComm_fwuFlashWrite(0x%08x, %x) returned %d\n", i32uFWUFlashAddr+i32uOffset, len, i32sRetVal);
					i32uOffset += len;
					i32uFWUlength -= len;
				}

				ret = 0;	// do not return errors here
				bEntering_s = bFALSE;
			}
		} else if (eRunStatus_s == enPiBridgeMasterStatus_FWUReset) {
			if (bEntering_s) {
				i32sRetVal = piIoComm_fwuReset(i32uFWUAddress);
				pr_info("piIoComm_fwuReset returned %d\n", i32sRetVal);

				ret = 0;	// do not return errors here
				bEntering_s = bFALSE;
			}
		}
	}

	rt_mutex_unlock(&piDev_g.lockBridgeState);
	if (RevPiScan.pCoreData != NULL) {
	}

	if (eBridgeStateLast_s != piDev_g.eBridgeState) {
		if (piDev_g.eBridgeState == piBridgeRun) {
			RevPiScan.i8uStatus |= PICONTROL_STATUS_RUNNING;
			gpio_set_value_cansleep(GPIO_LED_PWRRED, 0);
		} else {
			RevPiScan.i8uStatus &= ~PICONTROL_STATUS_RUNNING;
			gpio_set_value_cansleep(GPIO_LED_PWRRED, 1);
		}
		eBridgeStateLast_s = piDev_g.eBridgeState;
	}
	// set LED and status
	if (RevPiScan.pCoreData != NULL) {
		static unsigned long last_update;

		RevPiScan.pCoreData->i8uStatus = RevPiScan.i8uStatus;

		led = RevPiScan.pCoreData->i8uLED;
		if (led != last_led) {
			gpio_set_value_cansleep(GPIO_LED_AGRN, (led & PICONTROL_LED_A1_GREEN) ? 1 : 0);
			gpio_set_value_cansleep(GPIO_LED_ARED, (led & PICONTROL_LED_A1_RED) ? 1 : 0);
			gpio_set_value_cansleep(GPIO_LED_BGRN, (led & PICONTROL_LED_A2_GREEN) ? 1 : 0);
			gpio_set_value_cansleep(GPIO_LED_BRED, (led & PICONTROL_LED_A2_RED) ? 1 : 0);

			last_led = led;
		}

		// update every 1 sec
		if ((jiffies - last_update) > msecs_to_jiffies(1000)) {
			if (piDev_g.thermal_zone != NULL) {
				int temp, ret;

				ret = thermal_zone_get_temp(piDev_g.thermal_zone, &temp);
				if (ret) {
					pr_err("could not read cpu temperature");
				} else {
					RevPiScan.pCoreData->i8uCPUTemperature = temp / 1000;
				}
			}

			RevPiScan.pCoreData->i8uCPUFrequency = bcm2835_cpufreq_get_clock() / 10;

			last_update = jiffies;
		}
	}

	return ret;
}

//-------------------------------------------------------------------------------------------------------------------------
// the following functions are called from the ioctl funtion which is executed in the application task
// they block using msleep until their task is completed, which is signalled by reseting the flags bEntering_s to bFALSE.

INT32S PiBridgeMaster_FWUModeEnter(INT32U address, INT8U i8uScanned)
{
	if (piDev_g.eBridgeState == piBridgeStop) {
		i32uFWUAddress = address;
		i8uFWUScanned = i8uScanned;
		eRunStatus_s = enPiBridgeMasterStatus_FWUMode;
		bEntering_s = bTRUE;
		do {
			msleep(10);
		} while (bEntering_s);
		return i32sRetVal;
	}
	return -1;
}

INT32S PiBridgeMaster_FWUsetSerNum(INT32U serNum)
{
	if (piDev_g.eBridgeState == piBridgeStop) {
		i32uFWUSerialNum = serNum;
		eRunStatus_s = enPiBridgeMasterStatus_ProgramSerialNum;
		bEntering_s = bTRUE;
		do {
			msleep(10);
		} while (bEntering_s);
		return i32sRetVal;
	}
	return -1;
}


INT32S PiBridgeMaster_FWUflashErase(void)
{
	if (piDev_g.eBridgeState == piBridgeStop) {
		eRunStatus_s = enPiBridgeMasterStatus_FWUFlashErase;
		bEntering_s = bTRUE;
		do {
			msleep(10);
		} while (bEntering_s);
		return i32sRetVal;
	}
	return -1;
}

INT32S PiBridgeMaster_FWUflashWrite(INT32U flashAddr, char *data, INT32U length)
{
	if (piDev_g.eBridgeState == piBridgeStop) {
		i32uFWUFlashAddr = flashAddr;
		pcFWUdata = data;
		i32uFWUlength = length;
		eRunStatus_s = enPiBridgeMasterStatus_FWUFlashWrite;
		bEntering_s = bTRUE;
		do {
			msleep(10);
		} while (bEntering_s);
		return i32sRetVal;
	}
	return -1;
}

INT32S PiBridgeMaster_FWUReset(void)
{
	if (piDev_g.eBridgeState == piBridgeStop) {
		eRunStatus_s = enPiBridgeMasterStatus_FWUReset;
		bEntering_s = bTRUE;
		do {
			msleep(10);
		} while (bEntering_s);
		return i32sRetVal;
	}
	return -1;
}
