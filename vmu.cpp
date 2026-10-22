/*
    VeMUlator - A Dreamcast Visual Memory Unit emulator for libretro
    Copyright (C) 2018  Mahmoud Jaoune

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include <streams/file_stream.h>

#include "vmu.h"

/* Forward declarations */
extern "C" {
RFILE* rfopen(const char *path, const char *mode);
int64_t rfseek(RFILE* stream, int64_t offset, int origin);
int64_t rftell(RFILE* stream);
int rfgetc(RFILE* stream);
int rfclose(RFILE* stream);
}

VMU::VMU(uint16_t *_frameBuffer)
{
   //Initialize system
   ram          = new VE_VMS_RAM();
   rom          = new VE_VMS_ROM();
   flash        = new VE_VMS_FLASH(ram);
   intHandler   = new VE_VMS_INTERRUPTS();

   cpu          = new VE_VMS_CPU(ram, rom, flash, intHandler, true);
   audio        = new VE_VMS_AUDIO(cpu, ram);
   t0           = new VE_VMS_TIMER0(ram, intHandler, cpu, &prescaler);
   t1           = new VE_VMS_TIMER1(ram, intHandler, audio);
   baseTimer    = new VE_VMS_BASETIMER(ram, intHandler, cpu);
   video        = new VE_VMS_VIDEO(ram);
   serial       = new VE_VMS_SERIAL(ram, intHandler);
   frameBuffer  = _frameBuffer;


   //Initialize variables
   ccount       = 0;  //Cycle count
   cycle_count  = 0;
   time_reg     = 0;
   frame_skip   = 0;
   CPS          = 0; //Real cycles per second
   prescaler    = 0;
   pcount       = 0;
   oldPRR       = -1;

   OSC          = 0;
   OCR_old      = -1; //For performance, not to calculate clock each time, unless OCR is changed.
   threadReady  = false;
   inSleepState = false;
   BIOSExists   = false;
   linkConnectorBits = 0;
   enableSound  = true;
   useT1ELD     = false; //Some mini-game programmers (Especially homebrew creators) don't use it
   cycles_left  = 0;
}

VMU::~VMU()
{
   delete t0;
   delete t1;
   delete baseTimer;
   delete audio;
   delete serial;
   delete video;
   delete flash;
   delete cpu;
   delete intHandler;
   delete ram;
   delete rom;
}

int VMU::loadBIOS(const char *filePath)
{
   size_t i, fileSize;
   RFILE *bios = rfopen(filePath, "rb");

   if(!bios)
      return -1;

   rfseek(bios, 0, SEEK_END);
   fileSize = rftell(bios);
   rfseek(bios, 0, SEEK_SET);

   byte *BIOS_Data_Encrypted = new byte[0xF004];
   byte *BIOS_Data = new byte[0xF000];

   if(fileSize > 0xF004)
      return -2; //Unknown BIOS image type

   for(i = 0; i < fileSize; ++i)
      BIOS_Data_Encrypted[i] = rfgetc(bios);

   rfclose(bios);

   //Decrypt BIOS file if encrypted (First opcode is not JMPF)
   if(BIOS_Data_Encrypted[0] != 0x2A) 
   {
      //Remove first 4 bytes
      for (i = 0; i < 0xF000; ++i) 
         BIOS_Data[i] = BIOS_Data_Encrypted[i + 4];

      //XOR 0x37
      for (i = 0; i < 0xF000; ++i) 
         BIOS_Data[i] = (byte) ((BIOS_Data[i] ^ 0x37) & 0xFF);
   } 
   else 
   {
      //BIOS is not encrypted
      for(i = 0; i < 0xF000; ++i)
         BIOS_Data[i] = BIOS_Data_Encrypted[i];
   }

   //Check BIOS one last time (After decrypting)
   if(BIOS_Data[0] != 0x2A)
      return -1;

   BIOSExists = true;

   //This is loaded in (64KB) of ROM
   for(i = 0; i < 0xF000; ++i)
      rom->writeByte(i, BIOS_Data[i]);

   delete []BIOS_Data;
   delete []BIOS_Data_Encrypted;

   return 0;
}

void VMU::halt()
{
	cpu->state = 0;
}

void VMU::setDate()
{
	//Set time and date
	time_t rawTime;
	time(&rawTime);
	
	struct tm *currentTime = localtime(&rawTime);
	
	byte day   = currentTime->tm_mday & 0xFF;
	byte month = currentTime->tm_mon & 0xFF;
	byte year  = (currentTime->tm_year + 1900) & 0xFF;
	byte yearH = (year & 0xFF00) >> 8;
	byte yearL = year & 0xFF;
	byte hour  = currentTime->tm_hour & 0xFF;
	byte min   = currentTime->tm_min & 0xFF;
	byte sec   = currentTime->tm_sec & 0xFF;

	//BCD time
	ram->writeByte_RAW(0x10, int2BCD(year) & 0xFF);
	ram->writeByte_RAW(0x11, (int2BCD(year) & 0xFF00) >> 8);
	ram->writeByte_RAW(0x12, int2BCD(month));
	ram->writeByte_RAW(0x13, int2BCD(day));
	ram->writeByte_RAW(0x14, int2BCD(hour));
	ram->writeByte_RAW(0x15, int2BCD(min));
	ram->writeByte_RAW(0x16, int2BCD(sec));

	ram->writeByte_RAW(0x17, yearH);
	ram->writeByte_RAW(0x18, yearL);
	ram->writeByte_RAW(0x19, month);
	ram->writeByte_RAW(0x1A, day);
	ram->writeByte_RAW(0x1B, hour);
	ram->writeByte_RAW(0x1C, min);
	ram->writeByte_RAW(0x1D, sec);

	ram->writeByte_RAW(0x50, yearH / 4);
	ram->writeByte_RAW(0x51, yearL / 4);
}

//Sets system variables in RAM
void VMU::initBIOS()
{
	setDate();

	ram->writeByte_RAW(0x31, 0xFF);

	//No buttons clicked
	ram->writeByte_RAW(P3, 0xFF);
}

void VMU::startCPU()
{
	if(!BIOSExists)
	{
		//Enable HLE
		ram->writeByte_RAW(EXT, 1);
		cpu->EXTOld = 1;
		initializeHLE();
	} else initBIOS();

	cpu->state = 1;
}

void VMU::initializeHLE()
{
	//Initialize system variables
	setDate();
	ram->writeByte_RAW(0x31, 0xFF);
	ram->writeByte_RAW(0x6E, 0xFF);

	//Initialize SFR
	ram->writeByte_RAW(P3, 0xFF);
	ram->writeByte_RAW(SP, 0x7F);
	ram->writeByte_RAW(PSW, 0x02);
	ram->writeByte_RAW(IE, 0x80);
	ram->writeByte_RAW(MCR, 0x08);
	ram->writeByte_RAW(P7, 0x02);
	ram->writeByte_RAW(OCR, 0xA3);
	ram->writeByte_RAW(BTCR, 0x41);
}

void VMU::runCycle() 
{
	//Calculate cpu clock frequency (Only when OCR is changed)
	byte OCR_data = ram->readByte_RAW(OCR);
	if (OCR_data != OCR_old) 
	{
		int freqDiv = 12;
		if ((OCR_data & 128) != 0) freqDiv = 6;
		OSC = 0;    //Main clock by default is RC
		if ((OCR_data & 32) != 0) OSC = 1; //Quartz
		//double freq;
		if (OSC == 0) 
		{
			audio->setAudioFrequency(600000 / freqDiv);   //What the real frequency should be
			cpu->setFrequency(600000 / freqDiv);
		} 
		else    //RC
		{
			audio->setAudioFrequency(32768 / freqDiv);   //What the real frequency should be
			cpu->setFrequency(32768 / freqDiv);
		}
	}
	OCR_old = OCR_data;

	//Set T1LC and T1HC when bit 4 of T1CNT is 1
	byte T1CNT_data = ram->readByte_RAW(T1CNT);
	bool T1CUpdate = (T1CNT_data & 16) != 0;
	if(T1CUpdate)
	{
		ram->writeByte_RAW(T1LC, ram->T1LC_Temp);
		ram->writeByte_RAW(T1HC, ram->T1HC_Temp);

		audio->setT1C(ram->T1LC_Temp);
	}

	//Battery not low
	ram->writeByte_RAW(0x31, 0xFF);

	/* Bits 2 and 3 are the states of connector pins 13 and 6, which is how a
	   VMU notices another one has been clipped onto it. Software looks here
	   before it touches the serial port at all. */
	ram->writeByte_RAW(P7, (byte)(0x02 | linkConnectorBits));

	byte PCON_data = ram->readByte_RAW(PCON);

	//Execute
	if (cpu->state != 0) 
	{
		cpu->processInterrupts();
		if (PCON_data == 0) cycles_left = cpu->processInstruction(false);
	}
	--cycles_left;

	//Set prescaler
	byte PRR = ram->readByte_RAW(T0PRR);

	//This is important since Base Timer interrupts sometimes manipulates PRR, so we reset it so the modulo would be 0 in case number came.
	if (PRR != oldPRR) 
		pcount = PRR;
		
	if (pcount / 256 == 1) 
	{
		prescaler = 1;
		pcount = PRR;
	} 
	else 
	{
		prescaler = 0;
		pcount++;
	}
	oldPRR = PRR;


	//Run timers (t0 and t1)
	serial->runCycle();

	t0->runTimer();
	t1->runTimer();
	baseTimer->runTimer();

	//Set VMU date (I just randomly put it at 10000, that is, till the BIOS has fully initialized memory, so it wont manipulate date value)
	if (ccount == 10000 && BIOSExists) 
		setDate();
	else ccount++;

	cycle_count++;
}

void VMU::reset()
{
   delete t0;
   delete t1;
   delete baseTimer;
   delete audio;
   delete serial;
   delete video;
   delete flash;
   delete cpu;
   delete intHandler;
   delete ram;
   delete rom;

   //Re-initialize system
   ram          = new VE_VMS_RAM();
   rom          = new VE_VMS_ROM();
   flash        = new VE_VMS_FLASH(ram);
   intHandler   = new VE_VMS_INTERRUPTS();

   cpu          = new VE_VMS_CPU(ram, rom, flash, intHandler, true);

   audio        = new VE_VMS_AUDIO(cpu, ram);

   t0           = new VE_VMS_TIMER0(ram, intHandler, cpu, &prescaler);
   t1           = new VE_VMS_TIMER1(ram, intHandler, audio);
   baseTimer    = new VE_VMS_BASETIMER(ram, intHandler, cpu);

   video        = new VE_VMS_VIDEO(ram);
   serial       = new VE_VMS_SERIAL(ram, intHandler);

   //Re-nitialize variables
   ccount       = 0;  //Cycle count
   cycle_count  = 0;
   time_reg     = 0;
   frame_skip   = 0;
   CPS          = 0; //Real cycles per second
   prescaler    = 0;
   pcount       = 0;
   oldPRR       = -1;

   OSC          = 0;
   OCR_old      = -1; //For performance, not to calculate clock each time, unless OCR is changed.
   threadReady  = false;
   inSleepState = false;
   BIOSExists   = false;
   linkConnectorBits = 0;
   enableSound  = true;
   useT1ELD     = false; //Some mini-game programmers (Especially homebrew creators) don't use it
   cycles_left  = 0;
}
