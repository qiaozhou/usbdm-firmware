/*
 * SWD.c
 *
 *  Created on: 04/08/2012
 *      Author: podonoghue
 */
#include "Common.h"
#include "Configure.h"
#include "Commands.h"
#include "SWD.h"
#include "Commands.h"
#include "BDM.h"
#include "BDM_CF.h"
#include "CmdProcessing.h"
#include "BDMCommon.h"
#include "SPI.h"
#include "TargetDefines.h"

//#if (HW_CAPABILITY&CAP_JTAG_HW)
//#include "JTAGSequence.h"
//#endif
  
#if HW_CAPABILITY & CAP_SWD_HW

//!< SPI Masks - Mask to enable SPI as master Tx
#define SPIxC1_M_ON_TX  (SPIxC1_SPE_MASK|SPIxC1_MSTR_MASK|SPIxC1_CPOL_MASK|SPIxC1_CPHA_MASK|SPIxC1_SSOE_MASK|SPIxC1_LSBFE_MASK)
#define SPIxC1_M_ON_RX  (SPIxC1_SPE_MASK|SPIxC1_MSTR_MASK|SPIxC1_CPOL_MASK)                   //!< SPI Masks - Mask to enable SPI as master Tx
#define SPIxC2_M_8      (0)                                                                   //!< SPI Masks - 8-bit mode
#define SPIxC2_M_16     (SPIxC2_SPIMODE_MASK)                                                 //!< SPI Masks - 8-bit mode

// Can't use enumerations in assembly code
#define BDM_RC_OKx                 (0)
#define BDM_RC_NO_CONNECTIONx      (5)
#define BDM_RC_ACK_TIMEOUTx        (30)
#define BDM_RC_ARM_PARITY_ERRORx   (51)
#define BDM_RC_ARM_FAULT_ERRORx    (52)

#define SWD_READ_IDCODE 0xA5 // (Park,Stop,Parity,A[32],R/W,AP/DP,Start) = 10100101

// Masks for SWD_WR_DP_ABORT
#define SWD_DP_ABORT_CLEAR_ERRORS_B3 0x1E

// Masks for SWD_RD_DP_STATUS
#define SWD_RD_DP_STATUS_ANYERROR_B3 0xB2

#pragma MESSAGE DISABLE C1404 // Disable warnings about missing return value
#pragma MESSAGE DISABLE C5703 // Disable warnings about unused parameter

//! Sets the SWD interface to an idle state
//! RESET=3-state, SWCLK=High, SWDIO=3-state (SPI off)
//!
void swd_interfaceIdle(void) {
#ifdef RESET_3STATE
   RESET_3STATE();
#endif   
   SWD_3STATE();
   SWCLK_HIGH();
   SPIC1 = SPIxC1_OFF;
}

//! Initialise the SWD interface and sets it to an idle state
//! RESET=3-state, SWCLK=High, SWDIO=3-state
//!
//! @note This includes once-off initialisation such as PUPs etc 
void swd_init(void) {
   // 4 pins SWD_OUT, SWD_OUT_EN, SWCLK_OUT, SWCLK_OUT_EN
   // Individually controlled PUPs
   SWD_OUT_PER      = 1;     // Prevent float when disabled
   SWD_OUT_EN_PER   = 1;     // Prevent drive when disabled
   SWCLK_OUT_PER    = 1;     // Prevent float when disabled
   SWCLK_OUT_EN_PER = 1;     // Prevent drive when disabled
   
   SWD_IN_DDR       = 0;  // Make input
   SWD_IN_PER       = 1;  // Shouldn't be req. as external PUP for speed
   
#ifdef RESET_IN_DDR
   RESET_IN_DDR     = 0;     // Make input
#endif
#ifdef RESET_IN_PER
   RESET_IN_PER     = 1;     // Needed for input level translation to 5V
#endif
#ifdef RESET_OUT_PER
   RESET_OUT_PER    = 1;     // Holds RESET_OUT inactive when unused
#endif
   (void)spi_setSpeed(0);
   swd_interfaceIdle();
}

//!  Turns off the SWD interface
//!
//!  Depending upon settings, may leave target power on.
//!
void swd_off( void ) {
#if ((HW_CAPABILITY & CAP_FLASH) != 0)
   (void)bdmSetVpp(BDM_TARGET_VPP_OFF);
#endif
   if (!bdm_option.leaveTargetPowered) {
      VDD_OFF();
   }
   swd_interfaceIdle();
   SWCLK_DISABLE();
   SWD_DISABLE();
   RESET_DISABLE();
}

#define SWD_DATA1_CLK1 SWCLK_OUT_MASK|SWD_OUT_EN_MASK
#define SWD_DATA0_CLK1 SWCLK_OUT_MASK|0

//! Transmits 1 clock with SWDIO 3-state
//!
void swd_turnAround() {
   asm {               
      // 1 clock turn-around
      mov    #SWD_OUT_MASK,DATA_PORT        // SWDIO=3-state,SWCLK=0
      ldx    bitDelay                       // Low time delay
      dbnzx  *-0                            // [4n fppp]
      bset   SWCLK_OUT_BIT,DATA_PORT        // SWCLK=1
      ldx    bitDelay                       // High time delay
      dbnzx  *-0                            // [4n fppp]
   }
}

//! Transmits 8-bits of idle (SDIO=0)
//!
void swd_txIdle8(void) {
   asm {               
	  mov    #SPIxC2_M_8,SPIxC2             // Initialise SPI (8 bit)
	  mov    #SPIxC1_M_ON_TX,SPIC1          // Enable SPI
	  clr    DATA_PORT                      // Enable SWD drive
	  cmp    SPIxS                          // Dummy status read
	  clr    SPIxD                          // Tx data (=0)
   L1: 
	  brclr  SPIS_SPRF_BIT,SPIxS,L1         // Wait until Tx/Rx complete
	  lda    SPIxD                          // Discard rx data
	  mov    #SWCLK_OUT_MASK|SWD_OUT_EN_MASK,DATA_PORT // Setup for SWCLK=1, SWD=3-state
	  clr    SPIC1                          // Disable SPI
   }
}

#if (SWD_IN_BIT != 0)
#error "SWD_IN must be bit #0"
#endif
#pragma MESSAGE DISABLE C5703 // Disable warnings about unused parameter

//! SWD command phase 
//! Writes 8-bit command and receives 3-bit response
//! It will retry on WAIT response a limited number of times
//!
//! @param command - 8-bit command to write to SWD (including parity!)
//!
//! @return \n
//!    == \ref BDM_RC_OK              => Success        \n
//!    == \ref BDM_RC_ARM_FAULT_ERROR => FAULT response from target \n
//!    == \ref BDM_RC_ACK_TIMEOUT     => Excessive number of WAIT responses from target \n
//!    == \ref BDM_RC_NO_CONNECTION   => Unexpected/no response from target
//!
//! @note A turn-around clock period will be added on error responses
//!
U8 swd_sendCommandWithWait(U8 command) {
   asm {
      mov    #20,rxTiming1                  // Set up retry count
      
   retry:
      mov    #SPIxC2_M_8,SPIxC2             // Initialise SPI (8 bit)
      mov    #SPIxC1_M_ON_TX,SPIC1          // Enable SPI
      clr    DATA_PORT                      // Enable SWD drive
      cmp    SPIxS                          // Dummy status read
      sta    SPIxD                          // Tx data
   L1: 
      brclr  SPIS_SPRF_BIT,SPIxS,L1         // Wait until Tx/Rx complete
      lda    SPIxD                          // Discard rx data
      SWD_3STATE_ASM                        // SWD=3-state
      clr    SPIC1                          // Disable SPI

      // 1 clock turn-around
      bclr   SWCLK_OUT_BIT,DATA_PORT        // SWCLK=0
      ldx    bitDelay                       // Low time delay
      dbnzx  *-0                            // [4n fppp]
      bset   SWCLK_OUT_BIT,DATA_PORT        // SWCLK=1
      ldx    bitDelay                       // High time delay
      dbnzx  *-0                            // [4n fppp]
      
      // 1st bit
      clra                                  // Clear initial data value
      bclr   SWCLK_OUT_BIT,DATA_PORT        // SWCLK=0
      ldx    bitDelay                       // Low time delay
      dbnzx  *-0                            // [4n fppp]
      ldx    DATA_PORT                      // Capture data before rising edge
      rorx
      rora
      bset   SWCLK_OUT_BIT,DATA_PORT        // SWCLK=1
      ldx    bitDelay                       // High time delay
      dbnzx  *-0                            // [4n fppp]

      // 2nd bit
      bclr   SWCLK_OUT_BIT,DATA_PORT        // SWCLK=0
      ldx    bitDelay                       // Low time delay
      dbnzx  *-0                            // [4n fppp]
      ldx    DATA_PORT                      // Capture data before rising edge
      rorx
      rora
      bset   SWCLK_OUT_BIT,DATA_PORT        // SWCLK=1
      ldx    bitDelay                       // High time delay
      dbnzx  *-0                            // [4n fppp]

      // 3rd bit
      bclr   SWCLK_OUT_BIT,DATA_PORT        // SWCLK=0
      ldx    bitDelay                       // Low time delay
      dbnzx  *-0                            // [4n fppp]
      ldx    DATA_PORT
      rorx
      rora      
      bset   SWCLK_OUT_BIT,DATA_PORT        // SWCLK=1
      ldx    bitDelay                       // High time delay
      dbnzx  *-0                            // [4n fppp]
      
      tax
      lda    #BDM_RC_OKx
      cbeqx  #SWD_ACK_OK,done
      
      // Do turn-around clock on any error
      bclr   SWCLK_OUT_BIT,DATA_PORT        // SWCLK=0
      lda    bitDelay                       // Low time delay
      dbnza  *-0                            // [4n fppp]
      bset   SWCLK_OUT_BIT,DATA_PORT        // SWCLK=1
      lda    bitDelay                       // High time delay
      dbnza  *-0                            // [4n fppp]
      
      lda    #BDM_RC_ACK_TIMEOUTx
      cpx    #SWD_ACK_WAIT
      bne    identifyError
      dbnz   rxTiming1,retry
      bra    done
      
   identifyError:
      lda    #BDM_RC_ARM_FAULT_ERRORx
      cbeqx  #SWD_ACK_FAULT,done
      lda    #BDM_RC_NO_CONNECTIONx
      
   done:
   }
}

#pragma MESSAGE DEFAULT C5703 // Restore warnings about unused parameter

#pragma MESSAGE DISABLE C5703 // Disable warnings about unused parameter
//! Transmits 32-bit value
//!
//! Sequence as follows:
//!   - 1-clock turn-around 
//!   - 32-bit data value
//!   - 1-bit parity
//!   - 8-bit idle
//!
//! @param data - ptr to 32-bit data to Tx
//!
static void swd_tx32(const U8 *data) {
   asm {
	  // 1 clock turn-around
	  bclr   SWCLK_OUT_BIT,DATA_PORT        // SWCLK=0
	  lda    bitDelay                       // Low time delay
	  dbnza  *-0                            // [4n fppp]
      bset   SWCLK_OUT_BIT,DATA_PORT        // SWCLK=1

	  mov    #SPIxC2_M_16,SPIxC2            // Initialise SPI (16 bit)
      mov    #SPIxC1_M_ON_TX,SPIxC1         // Enable SPI
      clr    DATA_PORT                      // Enable SWD drive

   	  cmp    SPIxS                          // Dummy status read
   	  
   	  lda    3,x                            // Start Tx 1st & 2nd bytes
   	  sta    SPIxDL                         
   	  lda    2,x
   	  sta    SPIxDH                         
   	  lda    0,x                            // Do byte-wide parity
   	  eor    1,x
   	  eor    2,x
   	  eor    3,x
   	  ldhx   0,x                            // Get 3rd & 4th bytes
  L1:
   	  brclr  SPIS_SPRF_BIT,SPIxS,L1         // Wait until previous Tx/Rx complete
  	  sthx   SPIxD16                        // Tx 3rd & 4th bytes
   	  ldhx   SPIxD16                        // Discard read data

   	  // Calculate nibble parity
   	  psha             // [2]
      nsa              // [1]
      eor    1,sp      // [4]
      ais    #1        // [2]
      
      // Calculate final parity
      tax              // [1]
      clra             // [1]
      rorx             // [1]
      adc    #0        // [2]
      rorx             // [1]
      adc    #0        // [2]
      rorx             // [1]
      adc    #0        // [2]
      rorx             // [1]
      adc    #0        // [2]
      //parity in A.0

  L2:
	  brclr  SPIS_SPRF_BIT,SPIxS,L2         // Wait until previous Tx/Rx complete
	  ldhx   SPIxD16                        // Discard read data
      
#if SWD_OUT_BIT >= 1                        // move to SWD_OUT position
      lsla
#endif
#if SWD_OUT_BIT == 2
      lsla
#else
#error Fix this code
#endif
      and    #SWD_OUT_MASK                  // SWD=p, SWCLK=0
      sta    DATA_PORT                      // Set up - no effect yet
      ldx    bitDelay                       // Low time delay
      clr    SPIC1                          // Disable SPI,  
      dbnzx  *-0                            // [4n fppp]
      bset   SWCLK_OUT_BIT,DATA_PORT        // SWD=p, SWCLK=1

      // Start Tx of 8-bit idle
	  mov    #SPIxC2_M_8,SPIxC2             // Initialise SPI (8 bit)
	  mov    #SPIxC1_M_ON_TX,SPIC1          // Enable SPI
	  clr    DATA_PORT                      // Enable SWD drive
	  cmp    SPIxS                          // Dummy status read
	  clr    SPIxD                          // Tx data (=0)

	  // Wait for Idle Tx to complete
   L3: 
	  brclr  SPIS_SPRF_BIT,SPIxS,L3         // Wait until Tx/Rx complete
	  cmp    SPIxD                          // Discard rx data
	  mov    #SWCLK_OUT_MASK|SWD_OUT_EN_MASK,DATA_PORT // Setup for SWCLK=1, SWD=3-state
	  clr    SPIC1                          // Disable SPI
      rts 
   }
}
#pragma MESSAGE DEFAULT C5703 // Restore warnings about unused parameter

#pragma MESSAGE DISABLE C5703 // Disable warnings about unused parameter
//! Receives a 32-bit value with parity at end (33 total)
//!
//! Sequence as follows:
//!   - 32-bit data value
//!   - 1-bit parity
//!   - 8-bit idle
//!
//! @param data - ptr to buffer for Rx data
//!
//! @return BDM_RC_OK \n
//!         BDM_RC_ARM_PARITY_ERROR
//!
static U8 swd_rx32(U8 *data) {
#define SPIS_SPTEF_BIT (5)
#define SPIS_SPRF_BIT  (7)

   asm {
	  SWD_3STATE_ASM                        // SWD=3-state
	  mov    #SPIxC2_M_16,SPIxC2            // Initialise SPI (16 bit)
      mov    #SPIxC1_M_ON_TX,SPIxC1         // Enable SPI
   	  cmp    SPIxS                          // Dummy status read
   	  
   	  sthx   SPIxD16                        // Tx dummy/Rx                         
  L1:
	  brclr  SPIS_SPRF_BIT,SPIxS,L1         // Wait until Rx complete
	  lda    SPIxDH                         // Save data
	  sta    2,x
	  lda    SPIxDL
	  sta    3,x
   	  sthx   SPIxD16                        // Tx dummy/Rx                         
  L2:
	  brclr  SPIS_SPRF_BIT,SPIxS,L2         // Wait until Rx complete
	  lda    SPIxDH                         // Save data
	  sta    0,x
	  lda    SPIxDL
	  sta    1,x
	  
      bclr   SWCLK_OUT_BIT,DATA_PORT        // Setup for SWCLK=0
      clr    SPIC1                          // Disable SPI (SWCLK=0)

      // Parity bit
      lda    bitDelay                       // Low time delay
      dbnza  *-0                            // [4n fppp]
      lda    DATA_PORT                      // Capture data before rising clock edge
      bset   SWCLK_OUT_BIT,DATA_PORT        // SWCLK=1
      and    #SWD_IN_MASK                   // Convert parity to byte width
   	  // Single Parity bit remains - position is unimportant

      // Start Tx of 8-bit idle
	  mov    #SPIxC2_M_8,SPIxC2             // Initialise SPI (8 bit)
	  mov    #SPIxC1_M_ON_TX,SPIC1          // Enable SPI
	  clr    DATA_PORT                      // Enable SWD drive
	  cmp    SPIxS                          // Dummy status read
	  clr    SPIxD                          // Tx data (=0)

	  // Do parity calculation
	  eor    0,x                            // Do byte-wide parity on data & parity bit
   	  eor    1,x
   	  eor    2,x
   	  eor    3,x

   	  ldx    bitDelay                       // High time delay
	  dbnzx  *-0                            // [4n fppp]
	  
	  // Calculate nibble parity
   	  psha             // [2]
      nsa              // [1]
      eor    1,sp      // [4]
      ais    #1        // [2]
      
      // Calculate final parity
      tax              // [1]
      clra             // [1]
      rorx             // [1]
      adc    #0        // [2]
      rorx             // [1]
      adc    #0        // [2]
      rorx             // [1]
      adc    #0        // [2]
      rorx             // [1]
      adc    #0        // [2]
      and    #1
      //parity in A.0 - should be 0
      beq    okExit
      lda    #BDM_RC_ARM_PARITY_ERRORx
   okExit:

	  // Wait for Idle Tx to complete
   L3: 
	  brclr  SPIS_SPRF_BIT,SPIxS,L3         // Wait until Tx/Rx complete
	  cmp    SPIxD                          // Discard rx data
	  mov    #SWCLK_OUT_MASK|SWD_OUT_EN_MASK,DATA_PORT // Setup for SWCLK=1, SWD=3-state
	  clr    SPIC1                          // Disable SPI
      rts 
   }
}
#pragma MESSAGE DEFAULT C5703 // Restore warnings about unused parameter

#pragma MESSAGE DISABLE C5703 // Disable warnings about unused parameter
//! Switches interface to SWD
//!
//! Sequence as follows:
//!  - 64-bit sequence of 1's
//!  - 8-bit magic number 0xE79E
//!  - 64-bit sequence of 1's
//!
//! @note Interface is reset even if already in SWD mode so IDCODE must be read
//!       to enable interface
//!
static void swd_JTAGtoSWD(void) {
   asm {
	  mov    #SPIxC2_M_16,SPIxC2            // Initialise SPI (16 bit)
      mov    #SPIxC1_M_ON_TX,SPIxC1         // Enable SPI
      clr    DATA_PORT                      // Enable SWD drive
   	  cmp    SPIxS                          // Dummy status read
      
   	  bsr    txOnes							// Send 64 clocks
      ldhx   #0xE79E                        // Send magic #
      sthx   SPIxD16                        
  L5:
      brclr  SPIS_SPTEF_BIT,SPIxS,L5        // Wait until Tx buffer empty
   	  bsr    txOnes                         // Send 64 clocks
  L6:
      brclr  SPIS_SPTEF_BIT,SPIxS,L6                   // Wait until Tx buffer empty
      ldhx   SPIxD16                                   // Discard last data
	  mov    #SWCLK_OUT_MASK|SWD_OUT_EN_MASK,DATA_PORT // Setup for SWCLK=1, SWD=3-state
  L7:
	  brclr  SPIS_SPRF_BIT,SPIxS,L7                    // Wait until Tx complete
      ldhx   SPIxD16                                   // Discard rx data
      clr    SPIC1                                     // Disable SPI (SWCLK=1)
      rts
      
  txOnes:
   	  ldhx   #0xFFFF                        // Tx 64 bits with '1'
   	  sthx   SPIxD16                         
  L1:
      brclr  SPIS_SPTEF_BIT,SPIxS,L1        // Wait until Tx buffer empty
      sthx   SPIxD16                         
  L2:
      brclr  SPIS_SPTEF_BIT,SPIxS,L2        // Wait until Tx buffer empty
      sthx   SPIxD16                         
  L3:
      brclr  SPIS_SPTEF_BIT,SPIxS,L3        // Wait until Tx buffer empty
      sthx   SPIxD16                         
  L4:
      brclr  SPIS_SPTEF_BIT,SPIxS,L4        // Wait until Tx buffer empty
      rts 
   }
}
#pragma MESSAGE DEFAULT C5703 // Restore warnings about unused parameter

//! SWD - Try to connect to the target
//!
//! This will do the following:
//! - Switch the interface to SWD mode
//! - Read IDCODE
//!
//! @return \n
//!    == \ref BDM_RC_OK              => Success        \n
//!    == \ref BDM_RC_NO_CONNECTION   => Unexpected/no response from target
//!
U8 swd_connect(void) {
   U8 buff[4];	
   
   swd_JTAGtoSWD();
   swd_txIdle8();
	  
   // Target must respond to read IDCODE immediately
   return swd_readReg(SWD_READ_IDCODE, buff);
}

//! Read SWD register
//!
//! @param command - SWD command byte to select register etc.
//! @param data    - buffer for 32-bit value read
//!
//! @return \n
//!    == \ref BDM_RC_OK               => Success        \n
//!    == \ref BDM_RC_ARM_FAULT_ERROR  => FAULT response from target \n
//!    == \ref BDM_RC_ACK_TIMEOUT      => Excessive number of WAIT responses from target \n
//!    == \ref BDM_RC_NO_CONNECTION    => Unexpected/no response from target \n
//!    == \ref BDM_RC_ARM_PARITY_ERROR => Parity error on data read
//!
U8 swd_readReg(U8 command, U8 *data) {
   U8 rc = swd_sendCommandWithWait(command);
   if (rc != BDM_RC_OK) {
      return rc;
   }
   return swd_rx32(data);
}

//! Write SWD register
//!
//! @param command - SWD command byte to select register etc.
//! @param data    - buffer containing 32-bit value to write
//!
//! @return \n
//!    == \ref BDM_RC_OK               => Success        \n
//!    == \ref BDM_RC_ARM_FAULT_ERROR  => FAULT response from target \n
//!    == \ref BDM_RC_ACK_TIMEOUT      => Excessive number of WAIT responses from target \n
//!    == \ref BDM_RC_NO_CONNECTION    => Unexpected/no response from target
//!
U8 swd_writeReg(U8 command, const U8 *data) {
   U8 rc = swd_sendCommandWithWait(command);
   if (rc != BDM_RC_OK) {
      return rc;
   }
   swd_tx32(data);
   return rc;
}

//! Write AP register
//!
//! @param 16-bit address \n
//!    A[15:8]  => DP-AP-SELECT[31:24] (AP # Select) \n
//!    A[7:4]   => DP-AP-SELECT[7:4]   (Bank select within AP) \n
//!    A[3:2]   => APACC[3:2]          (Register select within bank)
//! @param buff \n
//!   - [1..4]  =>  32-bit register value
//!
//! @return
//!  == \ref BDM_RC_OK => success
//!
U8 swd_writeAPReg(const U8 *address, const U8 *buff) {
   static const U8 writeAP[] = {SWD_WR_AP_REG0,   SWD_WR_AP_REG1,    SWD_WR_AP_REG2,   SWD_WR_AP_REG3};
   U8 rc;
   U8 regNo = writeAP[(address[1]&0xC)>>2];
   U8 selectData[4];
   selectData[0] = address[0];
   selectData[1] = 0;
   selectData[2] = 0;
   selectData[3] = address[1]&0xF0;
   
   // Set up SELECT register for AP access
   rc = swd_writeReg(SWD_WR_DP_SELECT, selectData);
   if (rc != BDM_RC_OK) {
      return rc;
   }
   // Initiate write to AP register
   rc = swd_writeReg(regNo, buff);
   if (rc != BDM_RC_OK) {
      return rc;
   }
   // Read from READBUFF register to allow stall/status response
   rc = swd_readReg(SWD_RD_DP_RDBUFF, selectData);
   return rc;
}

//! Read AP register
//!
//! @param 16-bit address \n
//!    A[15:8]  => DP-AP-SELECT[31:24] (AP # Select) \n
//!    A[7:4]   => DP-AP-SELECT[7:4]   (Bank select within AP) \n
//!    A[3:2]   => APACC[3:2]          (Register select within bank)
//! @param buff \n
//!   - [1..4]  =>  32-bit register value
//!
//! @return
//!  == \ref BDM_RC_OK => success
//!
U8 swd_readAPReg(const U8 *address, U8 *buff) {
   static const U8 readAP[]  = {SWD_RD_AP_REG0,   SWD_RD_AP_REG1,    SWD_RD_AP_REG2,   SWD_RD_AP_REG3};
   U8 rc;
   U8 regNo = readAP[(address[1]&0xC)>>2];
   U8 selectData[4];
   selectData[0] = address[0];
   selectData[1] = 0;
   selectData[2] = 0;
   selectData[3] = address[1]&0xF0;

   // Set up SELECT register for AP access
   rc = swd_writeReg(SWD_WR_DP_SELECT, selectData);
   if (rc != BDM_RC_OK) {
	  return rc;
   }
   // Initiate read from AP register (dummy data)
   rc = swd_readReg(regNo, buff);
   if (rc != BDM_RC_OK) {
	  return rc;	   
   }
   // Read from READBUFF register
   rc = swd_readReg(SWD_RD_DP_RDBUFF, buff);
   return rc;
}

//! ARM-SWD - check  &clear stick bits
//!
//! @return error code
//!
U8 swd_clearStickyError(void) {
   static const U8 swdClearErrors[4] = {0,0,0,SWD_DP_ABORT_CLEAR_ERRORS_B3};
   return swd_writeReg(SWD_WR_DP_ABORT, swdClearErrors);

//   U8 statusValue[4];
//   U8 rc;
//   rc = swd_readReg(SWD_RD_AHB_CSW, statusValue);
//   if (rc != BDM_RC_OK) {
//      return rc;
//   }
//   if ((statusValue[3] & SWD_RD_DP_STATUS_ANYERROR_B3) != 0) {
//      // Clear sticky flags
//      return swd_writeReg(SWD_WR_DP_ABORT, swdClearErrors);
//   }
//   return swd_writeReg(SWD_WR_DP_ABORT, swdClearErrors);
//   return BDM_RC_OK;
}

U8 swd_test(void) {

   return swd_connect();
}
#endif // HW_CAPABILITY && CAP_SWD_HW

