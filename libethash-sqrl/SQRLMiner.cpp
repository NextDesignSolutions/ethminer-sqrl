/*
This file is part of ethminer.

ethminer is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

ethminer is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with ethminer.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 SQRLMiner mines to SQRL FPGAs
*/


#pragma GCC diagnostic ignored "-Wunused-function"

#if defined(__linux__)
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* we need sched_setaffinity() */
#endif
#include <error.h>
#include <sched.h>
#include <unistd.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <libethcore/Farm.h>
#include <ethash/ethash.hpp>

#include "SQRLMiner.h"


/* Sanity check for defined OS */
#if defined(__APPLE__) || defined(__MACOSX)
/* MACOSX */
#include <mach/mach.h>
#elif defined(__linux__)
/* linux */
#elif defined(_WIN32)
/* windows */
#include <windows.h>
#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Mswsock.lib")
#else
#error "Invalid OS configuration"
#endif


#include "SQRLAXI.h"

using namespace std;
using namespace dev;
using namespace eth;

static uint64_t eswap64(uint64_t in) {

  return (
           (((in >>  0ULL) & 0xFF) << 56) |
           (((in >>  8ULL) & 0xFF) << 48) |
           (((in >> 16ULL) & 0xFF) << 40) |
           (((in >> 24ULL) & 0xFF) << 32) |
           (((in >> 32ULL) & 0xFF) << 24) |
           (((in >> 40ULL) & 0xFF) << 16) |
           (((in >> 48ULL) & 0xFF) <<  8) |
           ((in >> 56ULL) & 0xFF)
         );
}
static uint32_t eswap32(uint32_t in) {
  return (
           (((in >> 0) & 0xFF) << 24) |
           (((in >> 8) & 0xFF) << 16) |
           (((in >>16) & 0xFF) <<  8) |
           ((in >>24) & 0xFF)
         );
}

/* ################## OS-specific functions ################## */

/*
 * returns physically available memory (no swap)
 */
static size_t getTotalPhysAvailableMemory()
{
  return 8*1024*1024*1024ULL;
}

/*
 * return numbers of available CPUs
 */
unsigned SQRLMiner::getNumDevices(SQSettings _settings)
{
  return _settings.hosts.size(); // Hosts are manually assigned 
}


/* ######################## CPU Miner ######################## */




SQRLMiner::SQRLMiner(unsigned _index, SQSettings _settings, DeviceDescriptor& _device, TelemetryType* telemetry)
  : Miner("sqrl-", _index), m_settings(_settings)
{
    m_deviceDescriptor = _device;
    m_tuner = new AutoTuner(this, telemetry);
}


SQRLMiner::~SQRLMiner()
{
    DEV_BUILD_LOG_PROGRAMFLOW(sqrllog, "sq-" << m_index << " SQRLMiner::~SQRLMiner() begin");
    stopWorking();
    kick_miner();
    DEV_BUILD_LOG_PROGRAMFLOW(sqrllog, "sq-" << m_index << " SQRLMiner::~SQRLMiner() end");

    // Close socket
    if (m_axi != 0) {
      sqrllog << "Disconnecting " << m_deviceDescriptor.name;
      SQRLAXIDestroy(&m_axi);
    }
    if (m_tuner != NULL)
        delete m_tuner;
}

// Full formula (VID being a voltage ID from 0 - 255, inclusive):
// Original r1 calculation:
// double r1 = 1.0 / (1.0 / 8.87 + 1.0 / 8.87);								// R101 || R29

// Optimized r1 calculation:
// double r1 = 4.435														// (optimized)

// double r2 = 20.0;														// R30
// double rSeries = 10.0;													// R81
// double rRheoMax = 50.0;													// +/- 20%

// Original r2Adj calculation:
// double r2Adj = 1.0 / ((1.0 / r2) + (1.0 / (rSeries + (rRheoMax / 256.0 * (double)(VID)))));

// Modified r2Adj calculation with constants substituted in:
// double r2Adj = 1.0 / ((1.0 / 20.0) + (1.0 / (10.0 + (50.0 / 256.0 * (double)(VID)))));

// Optimized r2Adj calculation
// double r2Adj = 20 - (2048 / (x + 153.6))

// Original voltage calculation:
// double voltage = 0.6 * (1.0 + (r1 / r2Adj);

// Modified voltage calculation with constants substituted in:
// double voltage = 0.6 * (1.0 + (4.435 / r2Adj))

// Optimized voltage calculation:
// double voltage = 0.6 + (2.661 / r2Adj)

// Total optimized voltage calculation:
// double voltage = 0.6 + (2.661 / (20 - (2048 / (VID + 153.6))))

// Populate a voltage table of 255 entries, containing
// the voltage (in volts) for every possible VID, which
// ranges from 0 - 255 (inclusive.)
void SQRLMiner::InitVoltageTbl()
{
	for(uint8_t VID = 0x00; VID < 0xFF; ++VID)
	{	  
		SQRLMiner::VoltageTbl[VID] = 0.6 + (2.661 / (20 - (2048 / (((double)VID + 153.6)))));
	}

	// Fill last table entry with the correct voltage at VID 0xFF
	SQRLMiner::VoltageTbl[0xFF] = 0.6 + (2.661 / (20 - (2048 / (255.0 + 153.6))));
}

// Returns VID which will yield the voltage closest to the value requested.
// Uses a binary search pattern after the usual sanity checks.
uint8_t SQRLMiner::FindClosestVIDToVoltage(double ReqVoltage)
{
	uint8_t idx = 0x80;
		
	// Normal idiot checks - including ensuring the requested voltage
	// is both above the minimum, yet below the maximum - if not,
	// clamp the voltage value to the possible range.
	if(ReqVoltage <= SQRLMiner::VoltageTbl[0xFF])
		return(0xFF);
	else if(ReqVoltage >= SQRLMiner::VoltageTbl[0x00])
		return(0x00);
	
	for(int half = 0x40; half > 0x00; half >>= 1)
	{
		// Binary search the table
		if(ReqVoltage < SQRLMiner::VoltageTbl[idx]) idx += half;
		else if(ReqVoltage > SQRLMiner::VoltageTbl[idx]) idx -= half;
		else if(ReqVoltage == SQRLMiner::VoltageTbl[idx]) return(idx);	
	}
	
	return(idx);
}

double SQRLMiner::LookupVID(uint8_t VID)
{
	return(SQRLMiner::VoltageTbl[VID]);
}

SQRLAXIResult SQRLMiner::StopHashcore(bool soft)
{
    // Stop the hashcore, optionally using a gradual
    // intensity ramp-down to minimize voltage spikes
    // - UART speed is 1 Mbps axi is 100 MHz
    // - UART message is 16 bytes, 160 wire bits
    // - Each wire bit is 1 microsecond, minimum 160us per step
    // - PMIC response time is > 40us - we can fire these as
    // - fast as we want 
    if (soft) {
      uint32_t dbg;
      SQRLAXIResult err = SQRLAXIRead(m_axi, &dbg, 0x5080);
      if (err == SQRLAXIResultOK) {
        int inn = (dbg >> 24) & 0xFF; 
	int step = ceil((double)inn / 8.0); 
        while(inn > 0) {
          dbg = (dbg & 0x00FFFFFF) | (inn << 24);
	  //printf("Dropping intensity to %i\n", inn);
  	  SQRLAXIWrite(m_axi, dbg, 0x5080, false);
	  inn -= step;
        } 
	if (inn != 0) {
  	  SQRLAXIWrite(m_axi, dbg & 0x00FFFFFF, 0x5080, false);
	}

      } else {
        sqrllog << EthRed << "Error gracefully reseting core, using hard-reset";
      }
      return SQRLAXIWrite(m_axi, 0x0, 0x506c, false);
    } else {
      return SQRLAXIWrite(m_axi, 0x0, 0x506c, false);
    }
}

bool SQRLMiner::initDevice()
{
    DEV_BUILD_LOG_PROGRAMFLOW(sqrllog, "sq-" << m_index << " SQRLMiner::initDevice begin");

    sqrllog << "Using FPGA: " << m_deviceDescriptor.name
           << " Memory : " << dev::getFormattedMemory((double)m_deviceDescriptor.totalMemory);
    m_hwmoninfo.deviceType = HwMonitorInfoType::SQRL;

    SQRLAXIResult err;
    SQRLAXIRef axi = SQRLAXICreate(SQRLAXIConnectionTCP, (char *)m_deviceDescriptor.sqHost.c_str(), m_deviceDescriptor.sqPort);
    if (axi != NULL) {
      SQRLAXISetTimeout(axi, m_settings.axiTimeoutMs);
      // Only affects interrupts from the multi-client bridge
      // used for dual-mining
      SQRLAXIEnableInterruptsWithMask(axi, 0x1);
      sqrllog << m_deviceDescriptor.name << " Connected";
      m_axi = axi;

     

      // Critical Data
      uint32_t dnaLo,dnaMid,dnaHi;
      err = SQRLAXIRead(m_axi, &dnaLo, 0x1000);
      if (err != 0) {
        sqrllog << "Error reading dna";
	dnaLo = 0;
      } 
      err = SQRLAXIRead(m_axi, &dnaMid, 0x1008);
      if (err != 0) {
        sqrllog << "Error reading dna";
	dnaMid = 0;
      } 
      err = SQRLAXIRead(m_axi, &dnaHi, 0x7000);
      if (err != 0) {
        sqrllog << "Error reading dna";
	dnaHi = 0;
      } 
      std::stringstream s;
      s << setfill('0') << setw(8) << std::hex << dnaLo << std::hex << dnaMid << std::hex << dnaHi;
      sqrllog << "DNA: " << s.str();
      m_settingID += s.str() + "_";

      uint32_t device, bitstream;
      err = SQRLAXIRead(m_axi, &device, 0x0);
      if (err != 0) {
        sqrllog << "Error reading device type";
	device = 0x756e6b6e;// 'unkn';
      } 
      err = SQRLAXIRead(m_axi, &bitstream, 0x8);
      if (err != 0) {
        sqrllog << "Error reading bitstream version";
	bitstream = 0;
      } 
      s.str("");
      s.clear();
      s << (char)(device >> 24) << (char)((device >> 16)&0xff) << (char)((device >> 8)&0xff) << (char)((device >> 0)&0xff);
      sqrllog << "FPGA: " << s.str();
      s.str("");
      s.clear();
      s << setfill('0') << setw(8) << std::hex << bitstream;
      sqrllog << "Bitstream: " << s.str();
      m_settingID += s.str() + "_";

      InitVoltageTbl();

      m_settingID += format2decimal(m_settings.fkVCCINT);
      m_settingID += format2decimal(m_settings.jcVCCINT);
   
      setVoltage(m_settings.fkVCCINT, m_settings.jcVCCINT);
     

      // Initialize clk
      sqrllog << "Stock Clock: " << setClock(-2);
      if ( m_deviceDescriptor.targetClk != 0) {
        sqrllog << "Target Clock: " << m_deviceDescriptor.targetClk; 
	// Target Clock set after Dag Generation
	m_lastClk = m_deviceDescriptor.targetClk;
      } else {
        m_lastClk = getClock();
      }

      sqrllog << "TuneID=" << m_settingID;
      if (boost::filesystem::exists(m_settings.tuneFile) && m_settings.autoTune > 0)
      {
          bool tuneFound = m_tuner->readSavedTunes(m_settings.tuneFile, m_settingID);

          if (tuneFound)
            m_settings.autoTune = 0; //if tune file exists, apply the tune and disable auto-tuning
      }
      


      // Print the settings
      sqrllog << "WorkDelay: " << m_settings.workDelay;
      sqrllog << "Patience: " << m_settings.patience;
      sqrllog << "IntensityN: " << m_settings.intensityN;
      sqrllog << "IntensityD: " << m_settings.intensityD;
      sqrllog << "SkipStallDetect: " << m_settings.skipStallDetection;

      
    } else {
      sqrllog << m_deviceDescriptor.name << " Failed to Connect";
      m_axi = NULL;
    }

    DEV_BUILD_LOG_PROGRAMFLOW(sqrllog, "sq-" << m_index << " SQRLMiner::initDevice end");
    return (m_axi != 0);
}

    void SQRLMiner::setVoltage(unsigned fkVCCINT, unsigned jcVCCINT)
{
    unsigned upperVoltLimit = 920;
    unsigned lowerVoltLimit = 500;

    if (fkVCCINT != 0)
    {
        if (fkVCCINT <= lowerVoltLimit || fkVCCINT > upperVoltLimit)
            sqrllog << EthRed << "Asking to set fkVCCINT out of bounds! [" << lowerVoltLimit << "-"
                    << upperVoltLimit << "]";

        else  // Set voltage if asked
        {
            uint32_t tmv;
            uint8_t tWiper = FindClosestVIDToVoltage(((double)fkVCCINT / 1000.0));
            tmv = (uint32_t)(LookupVID(tWiper) * 1000.0);

            sqrllog << "Found wiper code " << to_string(tWiper) << " for voltage " << to_string(tmv)
                    << "mV.\n";

            sqrllog << "Instructing FK VRM, if present, to target " << fkVCCINT << "mv";
            sqrllog << "Closest Viable Voltage " << tmv << "mv";
            SQRLAXIWrite(m_axi, 0xA, 0x9040, false);
            SQRLAXIWrite(m_axi, 0x158, 0x9108, false);
            SQRLAXIWrite(m_axi, 0x00, 0x9108, false);
            SQRLAXIWrite(m_axi, 0x200 | tWiper, 0x9108, false);
            SQRLAXIWrite(m_axi, 0x1, 0x9100, false);
        }
    }
    if (jcVCCINT != 0)
    {
        if (jcVCCINT <= lowerVoltLimit || jcVCCINT > upperVoltLimit)
            sqrllog << EthRed << "Asking to set jcVCCINT out of bounds! [" << lowerVoltLimit << "-"
                    << upperVoltLimit << "]";

        else  // Set voltage if asked
        {
            sqrllog << "Applying JCM PMIC Hot Fix";
            SQRLAXIWrite(m_axi, 0xA, 0xA040, false); // Soft Reset IIC 	
            SQRLAXIWrite(m_axi, 0x100|(0x4d<<1), 0xA108, false); // Transmit FIFO byte 1 (Write(startbit), Addr, Acadia) 	
            SQRLAXIWrite(m_axi, 0xD0, 0xA108, false); // Transmit FIFO byte 2 (SingleShotPage+Cmd)
            SQRLAXIWrite(m_axi, 0x04, 0xA108, false); // Transmit FIFO byte 3 (Write)
            SQRLAXIWrite(m_axi, 0x22, 0xA108, false); // Transmit FIFO byte 4 (AddrLo (CMD)	
            SQRLAXIWrite(m_axi, 0x08, 0xA108, false); // Transmit FIFO byte 2, VCCBRAM loop PID 
            SQRLAXIWrite(m_axi, 0x1C , 0xA108, false); // Transmit FIFO byte 3 // new param lo
            SQRLAXIWrite(m_axi, 0x200 | 0x5C, 0xA108, false); // Transmit FIFO byte 4 // new param hi (With Stop)
            SQRLAXIWrite(m_axi, 0x100|(0x4d<<1), 0xA108, false); // Transmit FIFO byte 1 (Write(startbit), Addr, Acadia) 	
            SQRLAXIWrite(m_axi, 0xD0, 0xA108, false); // Transmit FIFO byte 2 (SingleShotPage+Cmd)
            SQRLAXIWrite(m_axi, 0x04, 0xA108, false); // Transmit FIFO byte 3 (Write)
            SQRLAXIWrite(m_axi, 0x24, 0xA108, false); // Transmit FIFO byte 4 (AddrLo (CMD)	
            SQRLAXIWrite(m_axi, 0x08, 0xA108, false); // Transmit FIFO byte 2, VCCBRAM loop PID 
            SQRLAXIWrite(m_axi, 0x22 , 0xA108, false); // Transmit FIFO byte 3 // new param lo
            SQRLAXIWrite(m_axi, 0x200 | 0x2C, 0xA108, false); // Transmit FIFO byte 4 // new param hi (With Stop)
            SQRLAXIWrite(m_axi, 0x1, 0xA100, false); // Send IIC transaction 	
#ifdef _WIN32
            Sleep(1000);
#else
            usleep(1000000);
#endif
            SQRLAXIWrite(m_axi, 0xA, 0xA040, false);  // Soft Reset IIC
            SQRLAXIWrite(m_axi, 0x100 | (0x4d << 1), 0xA108,
                false);  // Transmit FIFO byte 1 (Write(startbit), Addr, Acadia)
            SQRLAXIWrite(m_axi, 0xD0, 0xA108, false);  // Transmit FIFO byte 2
                                                       // (SingleShotPage+Cmd)
            SQRLAXIWrite(m_axi, 0x04, 0xA108, false);  // Transmit FIFO byte 3 (Write)
            SQRLAXIWrite(m_axi, 0xAA, 0xA108, false);  // Transmit FIFO byte 4 (AddrLo (CMD)
            SQRLAXIWrite(m_axi, 0x0A, 0xA108, false);  // Transmit FIFO byte 2, VCCBRAM_OV_FAULT
            SQRLAXIWrite(m_axi, 0xf3, 0xA108, false);  // Transmit FIFO byte 3 // vEnc[0]
            SQRLAXIWrite(m_axi, 0x200 | 0xe0, 0xA108, false);  // Transmit FIFO byte 4 //
                                                               // vEnc[1] (With Stop)
            SQRLAXIWrite(m_axi, 0x1, 0xA100, false);           // Send IIC transaction
#ifdef _WIN32
            Sleep(1000);
#else
            usleep(1000000);
#endif
            SQRLAXIWrite(m_axi, 0xA, 0xA040, false);  // Soft Reset IIC
            SQRLAXIWrite(m_axi, 0x100 | (0x4d << 1), 0xA108,
                false);  // Transmit FIFO byte 1 (Write(startbit), Addr, Acadia)
            SQRLAXIWrite(m_axi, 0xD0, 0xA108, false);  // Transmit FIFO byte 2
                                                       // (SingleShotPage+Cmd)
            SQRLAXIWrite(m_axi, 0x04, 0xA108, false);  // Transmit FIFO byte 3 (Write)
            SQRLAXIWrite(m_axi, 0xAA, 0xA108, false);  // Transmit FIFO byte 4 (AddrLo (CMD)
            SQRLAXIWrite(m_axi, 0x06, 0xA108, false);  // Transmit FIFO byte 2, VCCINT OV_FAULT
            SQRLAXIWrite(m_axi, 0xf3, 0xA108, false);  // Transmit FIFO byte 3 // vEnc[0]
            SQRLAXIWrite(m_axi, 0x200 | 0xe0, 0xA108, false);  // Transmit FIFO byte 4 //
                                                               // vEnc[1] (With Stop)
            SQRLAXIWrite(m_axi, 0x1, 0xA100, false);           // Send IIC transaction

            sqrllog << "Asking JCM VRM, if present, to target " << jcVCCINT << "mv";

#ifdef _WIN32
            Sleep(1000);
#else
            usleep(1000000);
#endif
            uint16_t vEnc = (uint16_t)(((double)jcVCCINT / 1000.0) * 256.0);
            SQRLAXIWrite(m_axi, 0xA, 0xA040, false);  // Soft Reset IIC
            SQRLAXIWrite(m_axi, 0x100 | (0x4d << 1), 0xA108,
                false);  // Transmit FIFO byte 1 (Write(startbit), Addr, Acadia)
            SQRLAXIWrite(m_axi, 0xD0, 0xA108, false);         // Transmit FIFO byte 2
                                                              // (SingleShotPage+Cmd)
            SQRLAXIWrite(m_axi, 0x04, 0xA108, false);         // Transmit FIFO byte 3 (Write)
            SQRLAXIWrite(m_axi, (0x21 << 1), 0xA108, false);  // Transmit FIFO byte 4 (AddrLo
                                                              // (CMD)
            SQRLAXIWrite(m_axi, 0x06, 0xA108, false);         // Transmit FIFO byte 2, VOUT CMD
            SQRLAXIWrite(m_axi, 0x0 | (vEnc & 0xFF), 0xA108, false);  // Transmit FIFO byte 3 //
                                                                      // vEnc[0]
            SQRLAXIWrite(m_axi, 0x200 | ((vEnc >> 8) & 0xFF), 0xA108,
                false);                               // Transmit FIFO byte 4 // vEnc[1] (With Stop)
            SQRLAXIWrite(m_axi, 0x1, 0xA100, false);  // Send IIC transaction
        }
    }
}

    /*
 * A new epoch was receifed with last work package (called from Miner::initEpoch())
 *
 * If we get here it means epoch has changed so it's not necessary
 * to check again dag sizes. They're changed for sure
 * We've all related infos in m_epochContext (.dagSize, .dagNumItems, .lightSize, .lightNumItems)
 */
bool SQRLMiner::initEpoch_internal()
{
    // TODO - Update and recalc DAG
    // Do DAG Stuff!
    // m_epochContext.lightSize
    // m_epochContext.dagSize
    // m_epochContext.lightCache
   
    m_dagging = true;   
    // Always drop to stock clock immediately on start, before we stop or change cores
    setClock(-2);

    axiMutex.lock();
    sqrllog << "Changing to Epoch " << m_epochContext.epochNumber; 
    // Stop the mining core if it is active, and stop DAGGEN if active
    StopHashcore(true);
    // Ensure DAGGEN is powered on
    SQRLAXIWrite(m_axi, 0xFFFFFFFF, 0xB000, true);
    // Stop DAGGEN
    SQRLAXIWrite(m_axi, 0x2, 0x4000, true);

    uint8_t err = 0;

    // Compute and set mining parameters always (DAG may be generated, but core may have been reset)
    uint32_t nItems = m_epochContext.dagSize/128;
    err = SQRLAXIWrite(m_axi, nItems, 0x5040, true);
    if (err != 0) sqrllog << "Failed setting ethcore nItems";

    // Compute the reciprical, adjusted to ETH optimized modulo
    double reciprical = 1.0/(double)nItems * 0x1000000000000000ULL;
    uint32_t intR = (uint64_t)reciprical >> 4ULL;
    err = SQRLAXIWrite(m_axi, intR, 0x5088, true);
    if (err != 0) sqrllog << "Failed setting ethcore rnItems!";

    // Check for the existing DAG
    uint32_t dagStatusWord = 0;
    err = SQRLAXIRead(m_axi, &dagStatusWord, 0x40B8);
    if (err != 0) {
      sqrllog << "Failed checking current HW DAG version";
      dagStatusWord = 0;
    }
    if ((dagStatusWord >> 31) && !m_settings.forceDAG) {
      sqrllog << "Current HW DAG is for Epoch " << (dagStatusWord & 0xFFFF);
      if ( (dagStatusWord & 0xFFFF) == (uint32_t)m_epochContext.epochNumber) {
        sqrllog << "No DAG Generation is needed";
	// Power off DAGGEN
	SQRLAXIWrite(m_axi, 0x0, 0xB000, true);
	m_dagging = false;
	axiMutex.unlock();
	setClock(m_lastClk);

    m_tuner->startTune(m_lastClk);

	return true;
      }
    }

    // Ensure DAGGEN reset if we have to regenerate
    SQRLAXIWrite(m_axi, 0xFFFFFFFD, 0xB000, true);
    SQRLAXIWrite(m_axi, 0xFFFFFFFF, 0xB000, true);

    // Reset clock to defaults
    double curClk = getClock();
    if (curClk < m_lastClk) {
      sqrllog << "Resetting clock to Bitstream Default for Dag Generation";
      //m_lastClk = getClock();
      setClock(-2);
    } else {
      setClock(m_lastClk);
    }

    // Newer-bitstreams support on-module cache generation
    const bool makeCacheOnChip = true;
    uint32_t num_parent_nodes = m_epochContext.lightSize/64;
    if (makeCacheOnChip) {
      sqrllog << "Generating LightCache...";
      auto startCache = std::chrono::steady_clock::now(); 
      SQRLAXIWrite(m_axi, 0x2, 0x40BC, true);
      SQRLAXIWrite(m_axi, num_parent_nodes, 0x4008, true);
      // Set seedhash (reverse byte order)
      uint8_t revSeed[32];
      uint8_t * newSeed = (uint8_t *)&m_epochContext.seed;
      for(int s=0; s < 32; s++) revSeed[s] = newSeed[31-s];
      //for(int s=0;s<32;s++) printf("%02hhx", revSeed[s]);
      //  printf("\n");
      SQRLAXIWriteBulk(m_axi, revSeed, 32, 0x40c0, 1/*EndianFlip*/);
      SQRLAXIWrite(m_axi, 0x1, 0x40BC, true);
      uint32_t cstatus = 0;
      while ((cstatus&2) != 0x2) {
	axiMutex.unlock();
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
	axiMutex.lock();
        err = SQRLAXIRead(m_axi, &cstatus, 0x40BC);
        if((err != 0) && m_settings.dieOnError) {
          exit(1);
        }
      }
      auto cacheTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startCache);
      sqrllog << "Final LightCache Generation Status: " << cstatus;
      sqrllog << "LightCache Generation took " << cacheTime.count() << " ms.";
    } else {
      sqrllog << "Uploading new Light Cache...(This may take some time)";
      auto uploadStart = std::chrono::steady_clock::now(); 
      uint8_t uploadFailed = 0;
      uint32_t cacheSize = m_epochContext.lightSize;
      uint8_t * cache = (uint8_t *)m_epochContext.lightCache;
      uint32_t chunkSize = 65536;
      uint32_t steps=0;
      for(uint32_t pos=0x00; pos < cacheSize; pos+=chunkSize) {
          if (SQRLAXICDMAWriteBytes(m_axi,cache+pos, (cacheSize-pos)>chunkSize?chunkSize:(cacheSize-pos), pos) != 0) {
            sqrllog << "Upload packet error, retrying...";
            if (SQRLAXICDMAWriteBytes(m_axi,cache+pos, (cacheSize-pos)>chunkSize?chunkSize:(cacheSize-pos), pos) != 0) {
              uploadFailed = 1;
              break;
            }
          }
	  if (steps++ % 100 == 0)
            sqrllog << "Cache upload " << (double)(pos+chunkSize)/(double)m_epochContext.lightSize * 100.0 << "%"; 
      }
      if (uploadFailed) {
        sqrllog <<  "Cache upload failed";
      } else {
        auto uploadTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - uploadStart);
        sqrllog << dev::getFormattedMemory((double)m_epochContext.lightSize)
            << " of cache uploaded in "
            << uploadTime.count() << " ms.";
      }
      if (uploadFailed) {
        m_dagging = false;
	axiMutex.unlock();
        return false;
      }
    }
    sqrllog << "Preparing new DAG Generator Parameters...";
    sqrllog << "NUM_PARENT_NODES = " << num_parent_nodes;
    uint32_t num_mixers=m_settings.dagMixers; // This is fixed at bitstream gen time, added only for convience
    sqrllog << "NUM_MIXERS = "<< num_mixers;
    uint32_t mixer_size = m_epochContext.dagSize/64/num_mixers;
    uint32_t leftover = (m_epochContext.dagSize/64 - mixer_size*num_mixers);
    sqrllog << "DAG_ITEMS_PER_MIXER = " << mixer_size;
    sqrllog << "DAG_ITEMS_LEFTOVER = " << leftover;

    SQRLAXIWrite(m_axi, num_parent_nodes, 0x4008, true);
    uint32_t dagPos=0;
    for(uint32_t i=0; i < num_mixers; i++) {
      uint32_t mixer_start  = dagPos;
      SQRLAXIWrite(m_axi, mixer_start, 0x400c + 8*i, true);
      uint32_t mixer_end = dagPos+mixer_size;
      if (i == 0) mixer_end += leftover;
      SQRLAXIWrite(m_axi, mixer_end, 0x4010 + 8*i, true);
      dagPos = mixer_end;
    }

    // Finally, kick off DAG generation
    sqrllog << "Generating DAG...";
    auto startInit = std::chrono::steady_clock::now(); 
    SQRLAXIWrite(m_axi, 0x1, 0x4000, true);
    uint32_t status;
    err = SQRLAXIRead(m_axi, &status, 0x4000);
    if (err != 0) {
      sqrllog << "Error checking DAG status";
    } 
    uint8_t cnt = 0;
    if (!m_settings.skipDAG) {
      while ((status&2) != 0x2) {
        axiMutex.unlock();
#ifdef _WIN32
        Sleep(1000);
#else
        usleep(1000000);
#endif
        axiMutex.lock();
        err = SQRLAXIRead(m_axi, &status, 0x4000);
        if((err != 0) && m_settings.dieOnError) {
          exit(1);
        }
        cnt++;
        if (cnt % 5 == 0) {
	  uint32_t dagProgress = 0;
	  SQRLAXIRead(m_axi, &dagProgress, 0x4008);
	  double progress = (double)(mixer_size+leftover);
  	  progress = (double)dagProgress / progress;
          sqrllog << EthPurple << "DAG " << std::fixed << std::setprecision(2) << (progress * 100.0) << "%" << EthReset; 
        }
      }
    } else {
      sqrllog << "DEV - Skipping DAG, expect failed hashes";
    }
    sqrllog << "Final DAG Generation Status: " << status;
    auto dagTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startInit);
        sqrllog << dev::getFormattedMemory((double)m_epochContext.dagSize)
              << " of DAG data generated in "
              << dagTime.count() << " ms."; 

    sqrllog << "Duplicating DAG Items for performance...";
    auto startSwizzle = std::chrono::steady_clock::now(); 
    for(uint64_t i=0; i < 256; i++) {
      uint64_t src = 0x100000000ULL | (i << 24);
      uint64_t dst = 0x0ULL | (((i&0x0f) << 4) | ((i&0xF0) >> 4)) << 24;
      //printf("Swizzling chunk from %016lx to %016lx\n", src, dst);
      err = SQRLAXICDMACopyBytes(m_axi, src, dst, 0x1000000ULL);

      if (err != 0) {
        sqrllog << "Failed to swizzle DAG!";
        break;
      } else {
        //printf("Swizzled DAG successfully!\n");
      }
    }
    if (err == 0) {
      //printf("Copying Swizzled DAG back to stack 1...\n");
      err = SQRLAXICDMACopyBytes(m_axi, 0x0ULL, 0x100000000ULL, 4ULL*1024ULL*1024ULL*1024ULL);
      if (err != 0) {
        sqrllog << "Failed to copy DAG!";
      } 
    }
    auto swizzleTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startSwizzle);
    sqrllog << "DAG Duplication took " << swizzleTime.count() << " ms.";

    // Preserve the status to avoid the work in the future
    SQRLAXIWrite(m_axi, (1 << 31) | (uint32_t)m_epochContext.epochNumber, 0x40B8, true);	
    m_dagging = false;

    sqrllog << "Putting DAG Generator in low power mode...";
    SQRLAXIWrite(m_axi, 0x0, 0xB000, true);

    if (m_lastClk != 0) {
      sqrllog << "Restoring clock to target of " << (int)m_lastClk;
      setClock(m_lastClk);
    }

    axiMutex.unlock();

    m_tuner->startTune(m_lastClk);

    return true;
}


/*
   Miner should stop working on the current block
   This happens if a
     * new work arrived                       or
     * miner should stop (eg exit ethminer)   or
     * miner should pause
*/
void SQRLMiner::kick_miner()
{
    m_new_work.store(true, std::memory_order_relaxed);
    // Just put the core in reset
    if (!m_dagging) {
      // This can happen on odd thread
      // Stop mining if we are mining
      //StopHashcore(true); - happens in search exit
      // Immediately wake from any interrupts
      SQRLAXIKickInterrupts(m_axi);
    }
    m_new_work_signal.notify_one();
}


void SQRLMiner::search(const dev::eth::WorkPackage& w)
{
    // Left for reference
    //const auto& context = ethash::get_global_epoch_context(w.epoch);
    //const auto header = ethash::hash256_from_bytes(w.header.data());
    //const auto boundary = ethash::hash256_from_bytes(w.boundary.data());
    auto nonce = w.startNonce;

    

    m_new_work.store(false, std::memory_order_relaxed);

    // Re-init parameters 
    axiMutex.lock();
    uint8_t err = 0;
    err = SQRLAXIWriteBulk(m_axi, (uint8_t *)w.header.data(), 32, 0x5000, 1); 
    if (err != 0) sqrllog << "Failed setting ethcore header";
    auto falseTarget = h256("0x0000001fffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    if (w.boundary > falseTarget) falseTarget = w.boundary;
    err = SQRLAXIWriteBulk(m_axi, (uint8_t*)falseTarget.data(), 32, 0x5020, 1);
    if (err != 0) sqrllog << "Failed setting ethcore target";
    uint32_t nonceStartHigh = nonce >> 32;
    uint32_t nonceStartLow = nonce & 0xFFFFFFFF;
    err = SQRLAXIWrite(m_axi, nonceStartHigh, 0x5068, false);
    if (err != 0) sqrllog << "Failed setting ethcore nonceStartHigh";
    err = SQRLAXIWrite(m_axi, nonceStartLow, 0x5064, false);
    if (err != 0) sqrllog << "Failed setting ethcore nonceStartLow";

    uint32_t flags = 0;
    auto intensSet = m_tuner->getIntensitySettings();

    if (intensSet.isSet()) //if some other settings are available as part of tuning
    {
        m_settings.patience = intensSet.patience;
        m_settings.intensityD = intensSet.intensityD;
        m_settings.intensityN = intensSet.intensityN;
    }

    if (m_settings.patience != 0)
    {
        flags |= (1 << 6) | ((m_settings.patience & 0xff) << 8); 
    }
    if (m_settings.intensityN != 0)
    {
        flags |= (1 << 0) | ((m_settings.intensityN & 0xFF) << 24);
        flags |= (((m_settings.intensityD & 0x3F) * 8 - 1) << 16);
    }
    err = SQRLAXIWrite(m_axi, flags, 0x5080, false);
    if (err != 0) {
      sqrllog << "Failed setting ethcore debugFlags";
      if(m_settings.dieOnError) {
        exit(1);
      }
    }
 
    // Esnure hashcore loads new, reset work
    // Redundant, was stopped on exit or last time
    //err = StopHashcore(true);
    //if (err != 0) {
    //  sqrllog << "Error stopping hashcore";
    //} 
    
    // Bit 0 = enable nonces via interrupt instead of polling
    SQRLAXIWrite(m_axi, 0x00010001, 0x506c, false);
    if (err != 0) {
      sqrllog << "Error starting hashcore";
    }

    uint32_t lastSCnt = 0;
    uint64_t lastTChecks = 0;
    while (true)
    {
        if (m_new_work.load(std::memory_order_relaxed))  // new work arrived ?
        {
            m_new_work.store(false, std::memory_order_relaxed);
            break;
        }

        if (shouldStop())
            break;

	//   auto r = ethash::search(context, header, boundary, nonce, blocksize);
	axiMutex.unlock();

	bool nonceValid[4] = {false,false,false,false};
	uint64_t nonce[4] = {0,0,0,0};

	if (0/*Legacy Mode*/) {
	  // LEGACY - polling based
#ifdef _WIN32
	  Sleep(m_settings.workDelay/1000); // Give a momment for solutions
#else
	  usleep(m_settings.workDelay); // Give a momment for solutions
#endif
	  axiMutex.lock();

	  uint32_t value = 0;
	  uint32_t nonceLo,nonceHi;
	  err = SQRLAXIRead(m_axi, &value, 0x506c);
          if (err != 0) {
            sqrllog << "Failed checking nonceFlags";
	    value = 0;
	  }
    	  if ((value >> 15) & 0x1) {
            nonceValid[0] = true;
	    SQRLAXIRead(m_axi, &nonceHi, 0x5000+19*4);
	    SQRLAXIRead(m_axi, &nonceLo, 0x5000+28*4);
	    nonce[0] = ((((uint64_t)nonceHi) << 32ULL) | (uint64_t)nonceLo);
 	  } else nonceValid[0] = false;
	  if ((value >> 14) & 0x1) {
            nonceValid[1] = true;
	    SQRLAXIRead(m_axi, &nonceHi, 0x5000+20*4);
	    SQRLAXIRead(m_axi, &nonceLo, 0x5000+29*4);
	    nonce[1] = ((((uint64_t)nonceHi) << 32ULL) | (uint64_t)nonceLo);
	  } else nonceValid[1] = false;
	  if ((value >> 13) & 0x1) {
            nonceValid[2] = true;
	    SQRLAXIRead(m_axi, &nonceHi, 0x5000+21*4);
	    SQRLAXIRead(m_axi, &nonceLo, 0x5000+30*4);
	    nonce[2] = ((((uint64_t)nonceHi) << 32ULL) | (uint64_t)nonceLo);
	  } else nonceValid[2] = false;
	  if ((value >> 12) & 0x1) {
            nonceValid[3] = true;
	    SQRLAXIRead(m_axi, &nonceHi, 0x5000+22*4);
	    SQRLAXIRead(m_axi, &nonceLo, 0x5000+31*4);
	    nonce[3] = ((((uint64_t)nonceHi) << 32ULL) | (uint64_t)nonceLo);
	  } else nonceValid[3] = false;
	  // Clear nonces if needed
	  if (nonceValid[0] || nonceValid[1] || nonceValid[2] || nonceValid[3]) {
	    SQRLAXIWrite(m_axi, 0x00010000, 0x506c, false);
 	  }
        } else {
          // Modern, interrupt
	  uint64_t interruptNonce;
          SQRLAXIResult axiRes = SQRLAXIWaitForInterrupt(m_axi, (1<<0), &interruptNonce,m_settings.workDelay/1000);  	
	  if (axiRes == SQRLAXIResultOK) {
            nonceValid[0] = true;
	    nonce[0] = interruptNonce;  
	  } else if (axiRes == SQRLAXIResultTimedOut) {
            // Normal
	    nonceValid[0] = false;
	  } else {
	    sqrllog << EthRed << "FPGA Interrupt Error";
	    if(m_settings.dieOnError) {
              exit(1);
	    }
  	  }
	  axiMutex.lock();
	}

        // Get stall check parameters
	uint32_t sCnt;
	uint32_t tChkLo, tChkHi;
	if (!m_settings.skipStallDetection) {
          err = SQRLAXIRead(m_axi, &sCnt, 0x5084);
	  if (err != 0) {
            sqrllog << "Error checking for hashcore stall";
	    sCnt = 0;
	  } 
	}
	err = SQRLAXIRead(m_axi, &tChkLo, 0x5048);
	if (err != 0) {
          sqrllog << "Error reading target check counter";
	  tChkLo = 0;
	} 
	err = SQRLAXIRead(m_axi, &tChkHi, 0x5044);
        if (err != 0) {
          sqrllog << "Error reading target check counter";
	  tChkHi = 0;
	} 
	uint64_t tChks = ((uint64_t)tChkHi << 32) + tChkLo;

	uint64_t newTChks = 0;
	if (!((tChkLo == 0) && (tChkHi == 0))) {
	  if (tChks < lastTChecks) {
            tChkHi++; // Cheap rollover detection
	    tChks = ((uint64_t)tChkHi << 32) + tChkLo;
	  }
	  newTChks = tChks - lastTChecks;
	}
	lastTChecks = tChks; 

	uint8_t shouldReset = 0;
	if (!m_settings.skipStallDetection && (sCnt == lastSCnt)) {
          // Reset the core, re-init nonceStart 
	  shouldReset = 1;
	}
	lastSCnt = sCnt;

	for (int i=0; i < 4; i++) {
          if (nonceValid[i]) {
            auto sol = Solution{nonce[i], h256(0), w, std::chrono::steady_clock::now(), m_index};
 
            sqrllog << EthWhite << "Job: " << w.header.abridged()
                 << " Sol: " << toHex(sol.nonce, HexPrefix::Add) << EthReset;
            Farm::f().submitProof(sol);
	  }
	}
   
        // Update the hash rate
        updateHashRate(1, newTChks);

        //Auto tune and temperature check
        m_tuner->tune(newTChks);
       
        //For hashrate averages
        processHashrateAverages(newTChks);


	if (shouldReset) break; // Let core reset
    }
    // Ensure core is in reset
    StopHashcore(true);
    axiMutex.unlock();

}
void SQRLMiner::processHashrateAverages(uint64_t newTcks)
{
    m_hashCounter += newTcks;


    auto elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - (std::chrono::steady_clock::time_point)m_avgHashTimer)
                              .count();

    if (elapsedSeconds > 60)
    {
        double avg1min = (m_hashCounter / 60) / pow(10, 6);
        float errorRate = m_tuner->getHardwareErrorRate() * 100;

        if (avg1min > 10 && avg1min < 100)  // check for flukes
        {
            m_10minHashAvg.push_back(avg1min);
            m_60minHashAvg.push_back(avg1min);
        }
        if (m_10minHashAvg.size() > 10)
            m_10minHashAvg.erase(m_10minHashAvg.begin());  // pop front

        if (m_60minHashAvg.size() > 60)
            m_60minHashAvg.erase(m_60minHashAvg.begin());  // pop front

        double avg10min = average(m_10minHashAvg);
        double avg60min = average(m_60minHashAvg);

        m_avgValues[0] = avg1min;
        m_avgValues[1] = avg10min;
        m_avgValues[2] = avg60min;
        m_avgValues[3] = errorRate;

        m_avgHashTimer = std::chrono::steady_clock::now();
        m_hashCounter = 0;
    }
}
double SQRLMiner::average(std::vector<double> const& v)
{
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

double SQRLMiner::getClock() {
  return setClock(-1);
}

double SQRLMiner::setClock(double targetClk) {
  uint32_t valueVCO;
  SQRLAXIResult err = SQRLAXIRead(m_axi, &valueVCO, 0x8200);
  if (err != 0) {
    sqrllog << "Error checking current VCO - Aborting clock change";
    return 0.0;
  }
  // You can force VCO values here - be aware it also affects APB bus clock
  //valueVCO &= 0xFF0000FF;
  //valueVCO |= 0x007d0700;
  double mult = (double)((valueVCO>>8) &0xFF);
  double frac = 0;
  if ((valueVCO >> 16) & 0x2F) {
     frac = ((double)((valueVCO >> 16) & 0x3FF)) / 1000;
  }
  double gdiv = (valueVCO & 0xF);
  double vco = 200.0 * (mult+frac);
  vco /= gdiv;

  uint32_t valueClk0;
  err = SQRLAXIRead(m_axi, &valueClk0, 0x8208);
  if (err != SQRLAXIResultOK) { 
    sqrllog << "Error checking current clock - Aborting clock change";
    return 0.0;
  }

  double clk0div = (double)(valueClk0 & 0xF);
  double clk0FracDiv = ((double)((valueClk0 >> 8) & 0x3FF))/1000;
  clk0div += clk0FracDiv;

  double currentClk = vco / clk0div;

  // Changing?
  uint32_t nItems,rnItems;
  uint32_t daggenPwrState;
  if (targetClk != -1.0) {
    // Make sure we backup mining parameters - clock unlock can reset these
    err = SQRLAXIRead(m_axi, &nItems, 0x5040);
    if (err != 0) {
      sqrllog << "Fatal error preserving settings for clock change";
      nItems = 0;
    } 
    SQRLAXIRead(m_axi, &rnItems, 0x5088);
    if (err != 0) {
      sqrllog << "Fatal error preserving settings for clock change";
      rnItems = 0;
    } 
    // Ensure DAGGEN is powered on
    SQRLAXIRead(m_axi, &daggenPwrState, 0xB000);   
    if (err != 0) {
      sqrllog << "Fatal error preserving settings for clock change";
      daggenPwrState = 0;
    } 
    SQRLAXIWrite(m_axi, 0xFFFFFFFF, 0xB000, true);   
  }
  if (targetClk > 0) {
    double desiredDiv = vco/(targetClk+1); // Handles rounding when user tries to set a "UI" clock
    // Adjust to be multiple of 0.125 (round up == closed without going over
    desiredDiv = ((double)((int)(desiredDiv * 8 + 0.99))) / 8.0;
    if (desiredDiv < 2.0) {
      // Over max clock
      sqrllog << "CoreClk would exceed limit"; 
    } else {
      uint32_t newDiv = ((uint8_t)desiredDiv) | ((uint16_t)((desiredDiv-floor(desiredDiv))*1000.0) << 8);
      SQRLAXIWrite(m_axi, valueVCO, 0x8200, true);
      SQRLAXIWrite(m_axi, newDiv, 0x8208, true);
      SQRLAXIWrite(m_axi, 0x7, 0x825c, true);
      SQRLAXIWrite(m_axi, 0x3, 0x825c, true);
      currentClk = vco/desiredDiv;
      sqrllog << "Setting CoreClk to " << (int)currentClk;
      m_lastClk = (int)currentClk;
    }
  } else if (targetClk < -1.0) {
    sqrllog << "Resetting CoreClk to Stock";
    // Reset to factory defaults
    SQRLAXIWrite(m_axi, 0x5, 0x825c, true);
    SQRLAXIWrite(m_axi, 0x1, 0x825c, true);
#ifdef _WIN32
    Sleep(10);
#else
    usleep(10000);
#endif
    SQRLAXIWrite(m_axi, 0xA, 0x8000, true);
  }
  if (targetClk != -1.0) {
    // Wait for locked
    uint32_t waitCnt=1000;
    while(waitCnt--) {
      uint32_t locked;
      SQRLAXIRead(m_axi, &locked, 0x8004);
      if (locked&1) break;
    }
    if (waitCnt == 0) {
      sqrllog << "Timed out waiting for clock change to re-lock";
    } 

    // Make sure we restore the mining parameters 
    SQRLAXIWrite(m_axi, nItems, 0x5040, true);
    SQRLAXIWrite(m_axi, rnItems, 0x5088, true);
    SQRLAXIWrite(m_axi, daggenPwrState, 0xB000, true);
  }
  return currentClk;
}

void SQRLMiner::getTelemetry(unsigned int *tempC, unsigned int *fanprct, unsigned int *powerW) {
  // Temp Conversion: 
  // ((double)raw * 507.6 / 65536.0) - 279.43;
  // Volt Conversion
  // ((double)raw * 3.0 / 65536.0);

  // Read general SYSMON temp 
  axiMutex.lock();
  uint32_t raw;
  if (SQRLAXIResultOK == SQRLAXIRead(m_axi, &raw, 0x3400)) {
    (*tempC) = ((double)raw * 507.6 / 65536.0) - 279.43;
  } else {
    (*tempC) = 0;
  }
  (*fanprct) = getClock(); 
  if (SQRLAXIResultOK == SQRLAXIRead(m_axi, &raw, 0x3404)) {
    (*powerW) = ((double)raw * 3.0 / 65536.0) * 1000.0;
  } else {
    (*powerW) = 0;
  }

  // Read the HBM stack control values
  // Force "calibrated" if comms fail (Avoid cascaded errors)
  raw = 0x3;
  SQRLAXIRead(m_axi, &raw, 0x7008);
  axiMutex.unlock();
  // Left CAL, Right CL, Left CAT, Left 7 bit, Right CAT (Meow), Right 7bit 
  bool leftCalibrated = ((raw >> 0) & 1)?true:false;
  bool rightCalibrated = ((raw >> 1) & 1)?true:false;
  bool leftCatastrophic = ((raw >> 2) & 1)?true:false;
  bool rightCatastrophic = ((raw >> 10) & 1)?true:false;
  uint8_t leftTemp = (raw >> 3) & 0x7f;
  uint8_t rightTemp = (raw >> 11) & 0x7f;
  ostringstream s;
  if (m_settings.showHBMStats || leftTemp > 70 || rightTemp > 70 || leftCatastrophic || rightCatastrophic) {
    s <<  EthOrange << " HBM " 
	    << (leftCalibrated?"":"LCAL: 0 ")
	    << (rightCalibrated?"":"RCAL: 0 ")
	    << (leftCatastrophic?"LCATTRIP: ":"")
	    << (rightCatastrophic?"RCATTRIP: ":"")
	    << (int)leftTemp << "C " 
    	    << (int)rightTemp << "C";
  }
  
  float voltage = (*powerW) / 1000.0; 
  int temp = (*tempC);

  m_FPGAtemps[0] = temp;
  m_FPGAtemps[1] = leftTemp;
  m_FPGAtemps[2] = rightTemp;

  uint8_t tunerStage = m_tuner->getTuningStage(); 
  if (tunerStage > 0)  // still tuning
      s << EthRed << " Tuning... S" << (int)tunerStage;
  
  //Average hashrate block
  sqrllog << EthTeal << "sqrl-" << m_index << EthLime
          << " Avg 1m:" << format2decimal(m_avgValues[0])
          << " 10m:" << format2decimal(m_avgValues[1]) << " 60m:" << format2decimal(m_avgValues[2])
          << "Mhs" << EthPurple << " Err=" << format2decimal(m_avgValues[3])
          << "% [P=" << m_settings.patience << " N=" << m_settings.intensityN
          << " D=" << m_settings.intensityD << "] " << EthWhite << m_lastClk << "MHz "
          << format2decimal(voltage) << "V " << temp << "C " << s.str();
  

  if (leftCatastrophic | rightCatastrophic | !leftCalibrated | !rightCalibrated) {
    // Power down all cores
    StopHashcore(true);
    // Power down daggen
    SQRLAXIWrite(m_axi, 0x0, 0xB000, true);
    // Forces a stall
    if (leftCatastrophic | rightCatastrophic) {
      sqrllog << EthRed << "HBM STACK CATASTROPHIC TEMP - Powered Off, Refusing Work";
    } else {
      sqrllog << EthRed << "HBM Calibration Failed - Refusing Work";
    }
    m_dagging = true;
    kick_miner();
  }
} 

/*
 * The main work loop of a Worker thread
 */
void SQRLMiner::workLoop()
{
    DEV_BUILD_LOG_PROGRAMFLOW(sqrllog, "sq-" << m_index << " SQRLMiner::workLoop() begin");

    WorkPackage current;
    current.header = h256();

    if (!initDevice())
        return;

    while (!shouldStop())
    {
        // Wait for work or 3 seconds (whichever the first)
        const WorkPackage w = work();
        if (!w)
        {
            boost::system_time const timeout =
                boost::get_system_time() + boost::posix_time::seconds(3);
            boost::mutex::scoped_lock l(x_work);
            m_new_work_signal.timed_wait(l, timeout);
            continue;
        }

        if (w.algo == "ethash")
        {
            // Epoch change ?
            if (current.epoch != w.epoch)
            {
                if (!initEpoch())
                    break;  // This will simply exit the thread

                // As DAG generation takes a while we need to
                // ensure we're on latest job, not on the one
                // which triggered the epoch change
                current = w;
                continue;
            }

            // Persist most recent job.
            // Job's differences should be handled at higher level
            current = w;

            // Start searching
            search(w);
        }
        else
        {
            throw std::runtime_error("Algo : " + w.algo + " not yet implemented");
        }
    }

    DEV_BUILD_LOG_PROGRAMFLOW(sqrllog, "sq-" << m_index << " SQRLMiner::workLoop() end");
}


void SQRLMiner::enumDevices(std::map<string, DeviceDescriptor>& _DevicesCollection, SQSettings _settings)
{
    unsigned numDevices = getNumDevices(_settings);
    if (numDevices == 1)  // 127.0.0.1:2000-20XX
    {
        string s = _settings.hosts[0];
        if ((s.find("-") != std::string::npos) && (s.find(":") != std::string::npos) && (s.find(":") < s.find("-")))
        {
            vector<string> strs;
            boost::split(strs, s, boost::is_any_of(":"));

            string ip = strs[0];
            string portRange = strs[1];

            vector<string> ports;
            boost::split(ports, portRange, boost::is_any_of("-"));

            int startPort = std::stoi(ports[0]);
            int endPort = std::stoi(ports[1]);
            _settings.hosts.clear();

            for (int i = startPort; i <= endPort; i++)
            {
                string newIpPort = ip + ":" + std::to_string(i);
                _settings.hosts.push_back(newIpPort);
            }

            numDevices = getNumDevices(_settings);
        }
    }

    for (unsigned i = 0; i < numDevices; i++)
    {
        string uniqueId;
        ostringstream s;
        DeviceDescriptor deviceDescriptor;

        s << "sqrl-" << i;
        uniqueId = s.str();
        if (_DevicesCollection.find(uniqueId) != _DevicesCollection.end())
            deviceDescriptor = _DevicesCollection[uniqueId];
        else
            deviceDescriptor = DeviceDescriptor();

        std::vector<std::string> words;
        boost::split(words, _settings.hosts[i], boost::is_any_of(":"), boost::token_compress_on);

	deviceDescriptor.sqHost = words[0];
	deviceDescriptor.sqPort = (words.size() > 1)?stoi(words[1]):2000;

        s.str("");
        s.clear();
        s << "SQRL TCP-FPGA (" << deviceDescriptor.sqHost << ":" << deviceDescriptor.sqPort << ")" ;
        deviceDescriptor.name = s.str();
        deviceDescriptor.uniqueId = uniqueId;
        deviceDescriptor.type = DeviceTypeEnum::Fpga;
        deviceDescriptor.totalMemory = getTotalPhysAvailableMemory();
	deviceDescriptor.targetClk = _settings.targetClk;

        _DevicesCollection[uniqueId] = deviceDescriptor;
    }
}
