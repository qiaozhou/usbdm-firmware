/*
 * CmdProcessingSWD.c
 *
 *  Created on: 11/08/2012
 *      Author: podonoghuE
 */
#include <string.h>
#include "Common.h"
#include "Configure.h"
#include "Commands.h"
#include "TargetDefines.h"
#include "BDM.h"
#include "BDMMacros.h"
#include "BDMCommon.h"
#include "CmdProcessing.h"
#include "CmdProcessingSWD.h"
#include "SWD.h"

// DP_SELECT register value to access AHB_AP Bank #0 for memory read/write
static const U8 SWD_AHB_AP_BANK0[4] = {AHB_AP_NUM,  0,  0,  0};

// Initial value of AHB_SP_CSW register
static       U8 ahb_ap_csw_defaultValue_B0 = 0;

// Maps size (1,2,4 byes) to CSW control value (size+increment)
U8 cswValues[] = {
		0,
		0x40|AHB_AP_CSW_SIZE_BYTE|AHB_AP_CSW_INC_SINGLE,
		0x40|AHB_AP_CSW_SIZE_HALFWORD|AHB_AP_CSW_INC_SINGLE,
		0,
		0x40|AHB_AP_CSW_SIZE_WORD|AHB_AP_CSW_INC_SINGLE,
};

//! SWD - Try to connect to the target
//!
//! This will do the following:
//! - Switch the interface to SWD mode
//! - Read IDCODE
//! - Clear any sticky errors
//!
//! @return
//!    == \ref BDM_RC_OK => success        \n
//!    != \ref BDM_RC_OK => error
//!
U8 f_CMD_SWD_CONNECT(void) {
   U8 rc;
	
   ahb_ap_csw_defaultValue_B0 = 0;
   
   rc = swd_connect();
   
   (void)swd_clearStickyError();
   return rc;
}

//!  Write SWD DP register;
//!
//! @note
//!  commandBuffer\n
//!   - [2..3]  =>  2-bit register number [MSB ignored]
//!   - [4..7]  =>  32-bit register value
//!
//! @return
//!  == \ref BDM_RC_OK => success
//!
U8 f_CMD_SWD_WRITE_DREG(void) {
   static const U8 writeDP[] = {SWD_WR_DP_ABORT,  SWD_WR_DP_CONTROL, SWD_WR_DP_SELECT, 0};
   U8 rc = swd_writeReg(writeDP[commandBuffer[3]&0x03], commandBuffer+4);
   return rc;
}

//! Read SWD DP register;
//!
//! @note
//!  commandBuffer\n
//!   - [2..3]  =>  2-bit register number [MSB ignored]
//!
//! @return
//!  == \ref BDM_RC_OK => success         \n
//!                                       \n
//!  commandBuffer                        \n
//!   - [1..4]  =>  32-bit register value
//!
U8 f_CMD_SWD_READ_DREG(void) {
   static const U8 readDP[]  = {SWD_RD_DP_IDCODE, SWD_RD_DP_STATUS,  SWD_RD_DP_RESEND, SWD_RD_DP_RDBUFF};
   U8 rc = swd_readReg(readDP[commandBuffer[3]&0x03], commandBuffer+1);
   if (rc != BDM_RC_OK) {
      return rc;
   }
   returnSize = 5;
   return rc;
}

//! Write to AP register (sets AP_SELECT & APACC)
//!
//! @note
//!  commandBuffer\n
//!   - [2..3]  =>  16-bit register number = A  \n
//!    A[15:8]  => DP-AP-SELECT[31:24] (AP Select) \n
//!    A[7:4]   => DP-AP-SELECT[7:4]   (Bank select within AP) \n
//!    A[3:2]   => APACC[3:2]          (Register select within bank)
//!
//!   - [4..7]  =>  32-bit register value
//!
//! @return
//!  == \ref BDM_RC_OK => success
//!
U8 f_CMD_SWD_WRITE_CREG(void) {
   U8 rc;

   // Write to AP register
   rc = swd_writeAPReg(commandBuffer+2, commandBuffer+4);
   return rc;   
}

//! Read from AP register (sets AP_SELECT & APACC)
//!
//! @note
//!  commandBuffer\n
//!   - [2..3]  =>  16-bit register number = A  \n
//!    A[15:8]  => DP-AP-SELECT[31:24] (AP Select) \n
//!    A[7:4]   => DP-AP-SELECT[7:4]   (Bank select within AP) \n
//!    A[3:2]   => APACC[3:2]          (Register select within bank)
//!
//!   - [1..4]  =>  32-bit register value
//!
//! @return
//!  == \ref BDM_RC_OK => success
//!
U8 f_CMD_SWD_READ_CREG(void) {
   U8 rc;
   
   // Read from AP register
   rc = swd_readAPReg(commandBuffer+2, commandBuffer+1);
   if (rc == BDM_RC_OK) {
      returnSize = 5;
   }
   return rc;
}

//! Write ARM-SWD Memory
//!
//! @param address 32-bit memory address
//! @param data    32-bit data value
//!
//! @return
//!  == \ref BDM_RC_OK => success         \n
//!  != \ref BDM_RC_OK => various errors
//!                                       
U8 swd_writeMemoryWord(const U8 *address, const U8 *data) {
	U8  rc;
	U8  temp[4];
   /* Steps
	*  - Set up to access AHB-AP register bank 0 (CSW,TAR,DRW)
	*  - Write AP-CSW value (auto-increment etc)
	*  - Write AP-TAR value (target memory address)
	*  - Write value to DRW (data value to target memory)
	*/ 
   // Select AHB-AP memory bank - subsequent AHB-AP register accesses are all in the same bank 
   rc = swd_writeReg(SWD_WR_DP_SELECT, SWD_AHB_AP_BANK0);
   if (rc != BDM_RC_OK) {
	  return rc;
   }
   // Write CSW (word access etc)
   temp[0] = ahb_ap_csw_defaultValue_B0;
   temp[1] = 0;
   temp[2] = 0;
   temp[3] = 0x40|AHB_AP_CSW_SIZE_WORD;
   rc = swd_writeReg(SWD_WR_AHB_CSW, temp);
   if (rc != BDM_RC_OK) {
      return rc;	   
   }
   // Write TAR (target address)
   rc = swd_writeReg(SWD_WR_AHB_TAR, address);
   if (rc != BDM_RC_OK) {
	  return rc;	   
   }
   // Write data value
   rc = swd_writeReg(SWD_WR_AHB_DRW, data);
   if (rc != BDM_RC_OK) {
	  return rc;
   }
   return rc;   
}

//! Read ARM-SWD Memory
//!
//! @param address 32-bit memory address
//! @param data    32-bit data value
//!
//! @return
//!  == \ref BDM_RC_OK => success         \n
//!  != \ref BDM_RC_OK => various errors
//!
U8 swd_readMemoryWord(const U8 *address, U8 *data) {
U8  rc;
U8  temp[4];

   /* Steps
    *  - Set up to DP_SELECT to access AHB-AP register bank 0 (CSW,TAR,DRW)
    *  - Write AP-CSW value (auto-increment etc)
    *  - Write AP-TAR value (starting target memory address)
    *  - Initiate read by reading from DRW (dummy value)
    *  - Read data value from DP-READBUFF
    */ 
   // Select AHB-AP memory bank - subsequent AHB-AP register accesses are all in the same bank 
   rc = swd_writeReg(SWD_WR_DP_SELECT, SWD_AHB_AP_BANK0);
   if (rc != BDM_RC_OK) {
      return rc;
   }
   // Write memory access control to CSW
   temp[0] = ahb_ap_csw_defaultValue_B0;
   temp[1] = 0;
   temp[2] = 0;
   temp[3] = 0x40|AHB_AP_CSW_SIZE_WORD;
   rc = swd_writeReg(SWD_WR_AHB_CSW, temp);
   if (rc != BDM_RC_OK) {
      return rc;	   
   }
   // Write TAR (target address)
   rc = swd_writeReg(SWD_WR_AHB_TAR, address);
   if (rc != BDM_RC_OK) {
      return rc;	   
   }
   // Initial read of DRW (dummy data)
   rc = swd_readReg(SWD_RD_AHB_DRW, temp);
   if (rc != BDM_RC_OK) {
      return rc;	   
   }
   // Read memory data
   rc = swd_readReg(SWD_RD_DP_RDBUFF, data);
   return rc;  
}

//! Write ARM-SWD Memory
//!
//! @note
//!  commandBuffer\n
//!   - [2]     =>  size of data elements
//!   - [3]     =>  # of bytes
//!   - [4..7]  =>  Memory address [MSB ignored]
//!   - [8..N]  =>  Data to write
//!
//! @return \n
//!  == \ref BDM_RC_OK => success         \n
//!  != \ref BDM_RC_OK => various errors
//!                                       
U8 f_CMD_SWD_WRITE_MEM(void) {
	U8  elementSize = commandBuffer[2];  // Size of the data writes
	U8  count       = commandBuffer[3];  // # of bytes
	U8  addrLSB     = commandBuffer[7];  // Address in target memory
	U8  *data_ptr   = commandBuffer+8;   // Where the data is
	U8  rc;
	U8  temp[4];

   /* Steps
	*  - Set up to access AHB-AP register bank 0 (CSW,TAR,DRW)
	*  - Write AP-CSW value (auto-increment etc)
	*  - Write AP-TAR value (target memory address)
	*  - Loop
	*    - Pack data
	*    - Write value to DRW (data value to target memory)
	*/ 
   // Select AHB-AP memory bank - subsequent AHB-AP register accesses are all in the same bank 
   rc = swd_writeReg(SWD_WR_DP_SELECT, SWD_AHB_AP_BANK0);
   if (rc != BDM_RC_OK) {
	  return rc;
   }
   if (ahb_ap_csw_defaultValue_B0 == 0) {
      // Read initial AHB-AP.csw register value as device dependent
      // Do posted read - dummy data returned
      rc = swd_readReg(SWD_RD_AHB_CSW, temp);
      if (rc != BDM_RC_OK) {
         return rc;	   
      }
      // Get actual data
      rc = swd_readReg(SWD_RD_DP_RDBUFF, temp);
      if (rc != BDM_RC_OK) {
         return rc;	   
      }
      ahb_ap_csw_defaultValue_B0 = temp[0]; 
    }
   // Write CSW (auto-increment etc)
   temp[0] = ahb_ap_csw_defaultValue_B0;
   temp[1] = 0;
   temp[2] = 0;
   temp[3] = cswValues[elementSize];
   rc = swd_writeReg(SWD_WR_AHB_CSW, temp);
   if (rc != BDM_RC_OK) {
      return rc;	   
   }
   // Write TAR (target address)
   rc = swd_writeReg(SWD_WR_AHB_TAR, commandBuffer+4);
   if (rc != BDM_RC_OK) {
	  return rc;	   
   }
   switch (elementSize) {
   case MS_Byte:
	  while (count > 0) {
		 switch (addrLSB&0x3) {
		 case 0: temp[3] = *data_ptr++; break;
		 case 1: temp[2] = *data_ptr++; break;
		 case 2: temp[1] = *data_ptr++; break;
		 case 3: temp[0] = *data_ptr++; break;
		 }
		 rc = swd_writeReg(SWD_WR_AHB_DRW, temp);
		 if (rc != BDM_RC_OK) {
		    return rc;	   
		 }
		 addrLSB++;
		 count--;
	  }
      break;
   case MS_Word:
      count >>= 1;
      while (count > 0) {
         switch (addrLSB&0x2) {
         case 0:  temp[3] = *data_ptr++;
    	          temp[2] = *data_ptr++; break;
         case 2:  temp[1] = *data_ptr++;
    	          temp[0] = *data_ptr++; break;
         }
    	 rc = swd_writeReg(SWD_WR_AHB_DRW, temp);
         if (rc != BDM_RC_OK) {
      	    return rc;	   
         }
         addrLSB  += 2;
         count--;
      }
	  break;
   case MS_Long:
      count >>= 2;
      while (count > 0) {
     	 temp[3] = *data_ptr++;
    	 temp[2] = *data_ptr++;
    	 temp[1] = *data_ptr++;
    	 temp[0] = *data_ptr++;
    	 rc = swd_writeReg(SWD_WR_AHB_DRW, temp);
         if (rc != BDM_RC_OK) {
      	    return rc;	   
         }
//         addrLSB  += 4;
         count--;
      }
 	  break;
   }
   return rc;   
}

//! Read ARM-SWD Memory
//!
//! @note
//!  commandBuffer\n
//!   - [2]     =>  size of data elements
//!   - [3]     =>  # of bytes
//!   - [4..7]  =>  Memory address
//!
//! @return
//!  == \ref BDM_RC_OK => success         \n
//!  != \ref BDM_RC_OK => various errors  \n
//!                                       \n
//!  commandBuffer                        \n
//!   - [1..N]  =>  Data read
//!
U8 f_CMD_SWD_READ_MEM(void) {
U8  elementSize = commandBuffer[2];          // Size of the data writes
U8  count       = commandBuffer[3];          // # of data bytes
U8  addrLSB     = commandBuffer[7];          // LSB of Address in target memory
U8 *data_ptr    = commandBuffer+1;           // Where in buffer to write the data
U8  rc;
U8  temp[4];

   /* Steps
    *  - Set up to DP_SELECT to access AHB-AP register bank 0 (CSW,TAR,DRW)
    *  - Write AP-CSW value (auto-increment etc)
    *  - Write AP-TAR value (starting target memory address)
    *  - Loop
    *    - Read value from DRW (data value from target memory)
    *      Note: 1st value read from DRW is discarded
    *      Note: Last value is read from DP-READBUFF
    *    - Copy to buffer adjusting byte order
    */ 
   if (count>MAX_COMMAND_SIZE-1) {
      return BDM_RC_ILLEGAL_PARAMS;  // requested block+status is too long to fit into the buffer
   }
   // Select AHB-AP memory bank - subsequent AHB-AP register accesses are all in the same bank 
   rc = swd_writeReg(SWD_WR_DP_SELECT, SWD_AHB_AP_BANK0);
   if (rc != BDM_RC_OK) {
      return rc;
   }
   if (ahb_ap_csw_defaultValue_B0 == 0) {
      // Read initial AHB-AP.csw register value as device dependent
      // Do posted read - dummy data returned
   	  rc = swd_readReg(SWD_RD_AHB_CSW, temp);
      if (rc != BDM_RC_OK) {
   	     return rc;	   
      }
      // Get actual data
      rc = swd_readReg(SWD_RD_DP_RDBUFF, temp);
      if (rc != BDM_RC_OK) {
   	     return rc;	   
      }
      ahb_ap_csw_defaultValue_B0 = temp[0]; 
   }
   // Write CSW (auto-increment etc)
   temp[0] = ahb_ap_csw_defaultValue_B0;
   temp[1] = 0;
   temp[2] = 0;
   temp[3] = cswValues[elementSize];
   rc = swd_writeReg(SWD_WR_AHB_CSW, temp);
   if (rc != BDM_RC_OK) {
      return rc;	   
   }
   // Write TAR (target address)
   rc = swd_writeReg(SWD_WR_AHB_TAR, commandBuffer+4);
   if (rc != BDM_RC_OK) {
      return rc;	   
   }
   // Return size including status byte
   returnSize = count+1;

   // Initial read of DRW (dummy data)
   rc = swd_readReg(SWD_RD_AHB_DRW, temp);
   if (rc != BDM_RC_OK) {
      return rc;	   
   }
   switch (elementSize) {
   case MS_Byte:
      do {
         count--;
         if (count == 0) {
            // Read data from RDBUFF for final read
            rc = swd_readReg(SWD_RD_DP_RDBUFF, temp);
         }
         else {
            // Start next read and collect data from last read
            rc = swd_readReg(SWD_RD_AHB_DRW, temp);	 
         }
         if (rc != BDM_RC_OK) {
            return rc;	   
         }
         // Save data
         switch (addrLSB&0x3) {
         case 0: *data_ptr++ = temp[3];  break;
         case 1: *data_ptr++ = temp[2];  break;
         case 2: *data_ptr++ = temp[1];  break;
         case 3: *data_ptr++ = temp[0];  break;
         }
         addrLSB++;
      } while (count > 0);
      break;
   case MS_Word:
      count >>= 1;
	  do {
         count--;
		 if (count == 0) {
			// Read data from RDBUFF for final read
			rc = swd_readReg(SWD_RD_DP_RDBUFF, temp);
		 }
		 else {
			// Start next read and collect data from last read
			rc = swd_readReg(SWD_RD_AHB_DRW, temp);	 
		 }
		 if (rc != BDM_RC_OK) {
			return rc;	   
		 }
         // Save data
		 switch (addrLSB&0x2) {
		 case 0: *data_ptr++ = temp[3];
		         *data_ptr++ = temp[2];  break;
		 case 2: *data_ptr++ = temp[1];
		         *data_ptr++ = temp[0];  break;
		 }
		 addrLSB+=2;
	  } while (count > 0);
	  break;
   case MS_Long:
	  count >>= 2;
	  do {
		 count--;
		 if (count == 0) {
			// Read data from RDBUFF for final read
			rc = swd_readReg(SWD_RD_DP_RDBUFF, temp);
		 }
		 else {
			// Start next read and collect data from last read
			rc = swd_readReg(SWD_RD_AHB_DRW, temp);	 
		 }
		 if (rc != BDM_RC_OK) {
			return rc;	   
		 }
         // Save data
		 *data_ptr++ = temp[3];
		 *data_ptr++ = temp[2];
		 *data_ptr++ = temp[1];
		 *data_ptr++ = temp[0];
//		 addrLSB+=4;
	  } while (count > 0);
 	  break;
   }
   return rc;  
}

// Memory addresses of debug/core registers
const U8 DHCSR[] = {0xE0, 0x00, 0xED, 0xF0}; // RW Debug Halting Control and Status Register
const U8 DCSR[]  = {0xE0, 0x00, 0xED, 0xF4}; // WO Debug Core Selector Register
const U8 DCDR[]  = {0xE0, 0x00, 0xED, 0xF8}; // RW Debug Core Data Register

#define DCSR_WRITE_B1         (1<<(16-16))
#define DCSR_READ_B1          (0<<(16-16))
#define DCSR_REGMASK_B0       (0x7F)

#define DHCSR_DBGKEY_B0       (0xA0<<(24-24))
#define DHCSR_DBGKEY_B1       (0x5F<<(16-16))
#define DHCSR_S_RESET_ST_B0   (1<<(25-24)
#define DHCSR_S_RETIRE_ST_B0  (1<<(24-24))
#define DHCSR_S_LOCKUP_B1     (1<<(19-16))
#define DHCSR_S_SLEEP_B1      (1<<(18-16))
#define DHCSR_S_HALT_B1       (1<<(17-16))
#define DHCSR_S_REGRDY_B1     (1<<(16-16))
#define DHCSR_C_SNAPSTALL_B3  (1<<5)
#define DHCSR_C_MASKINTS_B3   (1<<3)
#define DHCSR_C_STEP_B3       (1<<2)
#define DHCSR_C_HALT_B3       (1<<1)
#define DHCSR_C_DEBUGEN_B3    (1<<0)

//! Initiates core register operation (read/write) and
//! waits for completion
//!
//! @param DCSRData - value to write to DCSRD register to control operation
//!
//! @note DCSRD is used as scratch buffer so must be ram
//!
static U8 swd_coreRegisterOperation(U8 *DCSRData) {
   U8 retryCount = 40;
   U8 rc;
   
   rc = swd_writeMemoryWord(DCSR, DCSRData);
   if (rc != BDM_RC_OK) {
	  return rc;
   }
   do {
	  if (retryCount-- == 0) {
		 return BDM_RC_ARM_ACCESS_ERROR;
	  }
	  // Check complete (use DCSRData as scratch)
	  rc = swd_readMemoryWord(DHCSR, DCSRData);
	  if (rc != BDM_RC_OK) {
		 return rc;
	  }
   } while ((DCSRData[1] & DHCSR_S_REGRDY_B1) == 0);
   return BDM_RC_OK;
}

//! Read ARM-SWD core register
//!
//! @note
//!  commandBuffer\n
//!   - [2..3]  =>  16-bit register number [MSB ignored]
//!
//! @return
//!  == \ref BDM_RC_OK => success         \n
//!                                       \n
//!  commandBuffer                        \n
//!   - [1..4]  =>  32-bit register value
//!
U8 f_CMD_SWD_READ_REG(void) {
   U8 rc;
   
   // Use commandBuffer as scratch
   commandBuffer[4+0] = 0;
   commandBuffer[4+1] = DCSR_READ_B1;
   commandBuffer[4+2] = 0;
   commandBuffer[4+3] = commandBuffer[3];
   // Execute register transfer 
   rc = swd_coreRegisterOperation(commandBuffer+4);
   if (rc != BDM_RC_OK) {
	  return rc;
   }
   returnSize = 5;
   // Read data value from DCDR holding register
   return swd_readMemoryWord(DCDR, commandBuffer+1);
}

//! Write ARM-SWD core register
//!
//! @note
//!  commandBuffer\n
//!   - [2..3]  =>  16-bit register number [MSB ignored]
//!   - [4..7]  =>  32-bit register value
//!
//! @return
//!  == \ref BDM_RC_OK => success
//!
U8 f_CMD_SWD_WRITE_REG(void) {
   U8 rc;
   
   // Write data value to DCDR holding register
   rc = swd_writeMemoryWord(DCDR,commandBuffer+4);
   if (rc != BDM_RC_OK) {
	  return rc;
   }
   // Use commandBuffer as scratch
   commandBuffer[4+0] = 0;
   commandBuffer[4+1] = DCSR_WRITE_B1;
   commandBuffer[4+2] = 0;
   commandBuffer[4+3] = commandBuffer[3];
   // Execute register transfer 
   return swd_coreRegisterOperation(commandBuffer+4);
}

//! ARM-SWD -  Step over 1 instruction
//!
//! @return
//!    == \ref BDM_RC_OK => success       \n
//!    != \ref BDM_RC_OK => error         \n
//!
U8 f_CMD_SWD_TARGET_STEP(void) {
   U8 debugStepValue[4];
   U8 rc;
   
   rc = swd_readMemoryWord(DHCSR, debugStepValue);
   if (rc != BDM_RC_OK) {
      return rc;
   }
   debugStepValue[0]  = DHCSR_DBGKEY_B0;
   debugStepValue[1]  = DHCSR_DBGKEY_B1;
   debugStepValue[2]  = 0;
   debugStepValue[3] &= DHCSR_C_MASKINTS_B3; // Preserve DHCSR_C_MASKINTS value
   debugStepValue[3] |= DHCSR_C_STEP_B3|DHCSR_C_DEBUGEN_B3;   
   return swd_writeMemoryWord(DHCSR, debugStepValue);
}

//! ARM-SWD -  Start code execution
//!
//! @return
//!    == \ref BDM_RC_OK => success       \n
//!    != \ref BDM_RC_OK => error         \n
//!
U8 f_CMD_SWD_TARGET_GO(void) {
   static const U8 debugGoValue[] = {DHCSR_DBGKEY_B0, DHCSR_DBGKEY_B1, 0, DHCSR_C_DEBUGEN_B3};
   return swd_writeMemoryWord(DHCSR, debugGoValue);
}

// ARM-SWD -  Stop the target
//
//! @return
//!    == \ref BDM_RC_OK => success       \n
//!    != \ref BDM_RC_OK => error         \n
//!
U8 f_CMD_SWD_TARGET_HALT(void) {
   static const U8 debugOnValue[] = {DHCSR_DBGKEY_B0, DHCSR_DBGKEY_B1, 0, DHCSR_C_HALT_B3|DHCSR_C_DEBUGEN_B3};
   return swd_writeMemoryWord(DHCSR, debugOnValue);
}
