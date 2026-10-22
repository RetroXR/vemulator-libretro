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

#include <libretro.h>
#include "vmu.h"

/* Forward declarations */
extern "C" {
   RFILE* rfopen(const char *path, const char *mode);
   int64_t rfseek(RFILE* stream, int64_t offset, int origin);
   int64_t rftell(RFILE* stream);
   int rfgetc(RFILE* stream);
   int rfclose(RFILE* stream);
}

retro_environment_t environment_cb;
retro_video_refresh_t video_cb;
retro_audio_sample_t audio_cb;
retro_audio_sample_batch_t audio_batch_cb;
retro_input_poll_t input_poll_cb;
retro_input_state_t input_state_cb;

static void fallback_log(enum retro_log_level level, const char *fmt, ...) { }

static retro_log_printf_t log_cb = fallback_log;

struct retro_variable options[3] = {
   {"enable_flash_write", "Enable flash write (.bin, requires restart); enabled|disabled"},
   {"serial_link", "Serial link to another VMU; enabled|disabled"},
   { NULL, NULL }
};

static VMU *vmu;
static uint16_t *frameBuffer;
static byte *romData;

/* The cable to another VMU, hosted by the frontend. A frontend that does not
   offer one leaves link_port NULL and the core runs exactly as it always has:
   a VMU with nothing in its socket. */
static struct retro_link_interface link_iface;
static retro_link_port_t          *link_port;
static uint64_t                    link_tick;
static uint64_t                    link_safe;

/* What this core calls a tick. The VMU's RC oscillator divided by 12, which is
   what OCR selects out of reset and the rate retro_run counts cycles at. A
   guest that reprograms OCR changes the real rate and this declared one does
   not follow it -- harmless while both ends of a cable are VMUs booted the
   same way, since the frontend only uses it to convert between peers' units. */
#define LINK_CLOCK_RATE 50000

/* How far ahead of itself this core promises not to originate a transfer.
   Traded against fidelity: larger means fewer rendezvous and more emulated
   delay before a byte lands. An eighth of a frame is a couple of milliseconds
   of emulated time, far below the millisecond-scale gaps between bytes the
   BIOS leaves. */
#define LINK_HORIZON (LINK_CLOCK_RATE / FPS / 8)

/* Peers whose protocol_id differs are never cabled to each other, which is
   what keeps this core off other machines' cables. */
#define LINK_PROTOCOL_ID "vmu-sio"

/* Join the frontend's link bus, if it has one and the user wants it.
 *
 * The experimental number is probed first and the final one second, so a core
 * built today keeps working against a frontend that later adopts the
 * unflagged 94. */
static void linkAttach(void)
{
   struct retro_variable var = {0};

   link_port = NULL;
   link_tick = 0;
   link_safe = 0;

   var.key = "serial_link";
   if(environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)
         && var.value && !strcmp(var.value, "disabled"))
      return;

   memset(&link_iface, 0, sizeof(link_iface));

   if(!environment_cb(RETRO_ENVIRONMENT_GET_LINK_INTERFACE, &link_iface)
         && !environment_cb(RETRO_ENVIRONMENT_GET_LINK_INTERFACE_FINAL, &link_iface))
   {
      log_cb(RETRO_LOG_INFO,
            "[VeMUlator] frontend hosts no link bus, serial port left empty\n");
      return;
   }

   link_port = link_iface.attach(0, LINK_PROTOCOL_ID, LINK_CLOCK_RATE);

   if(!link_port)
   {
      log_cb(RETRO_LOG_WARN, "[VeMUlator] could not attach to the link bus\n");
      return;
   }

   log_cb(RETRO_LOG_INFO, "[VeMUlator] attached to the link bus as \"%s\"\n",
         LINK_PROTOCOL_ID);
}

static void linkDetach(void)
{
   if(!link_port)
      return;

   link_iface.detach(link_port);
   link_port = NULL;
}

/* Hand the bus anything the VMU clocked out since the last look. Stamped at
   the published horizon rather than the cycle the eighth edge fell on, which
   is what makes the promise in safe_tick true, at the cost of up to a horizon
   of emulated delay on a byte. */
static void linkPumpOutgoing(void)
{
   int  iface;
   byte value;

   while(vmu->serial->takeOutgoing(&iface, &value))
   {
      byte payload[2];

      /* The cable crosses. Two VMUs clip together face to face, so one unit's
         connector is the mirror of the other's and SIO0 lands on the peer's
         SIO1. Both sides drive SO0/SCK0 and listen on SIO1, which only works
         because of that crossing -- wire it straight and both ends are
         talking into each other's outputs. */
      payload[0] = (byte)(iface ^ 1);
      payload[1] = value;

      /* One horizon ahead of where this core stands, which is exactly what it
         will publish as safe_tick on the next rendezvous. A peer cannot have
         run past it, so the byte is never delivered into its past. */
      link_iface.send(link_port, link_tick + LINK_HORIZON,
            RETRO_LINK_BROADCAST, payload, sizeof(payload));
   }
}

/* Take everything waiting for us. A bit-accurate model would hold each byte
   until the clock reached the tick it carries; this one delivers on arrival,
   which can be slightly early but is never late. */
static void linkDrainIncoming(void)
{
   uint64_t tick;
   unsigned from;
   byte     payload[8];
   size_t   len = sizeof(payload);

   while(link_iface.recv(link_port, &tick, &from, payload, &len))
   {
      if(len >= 2)
         vmu->serial->deliver((int)payload[0], payload[1]);

      len = sizeof(payload);
   }
}

/* Run one frame's worth of cycles, rendezvousing with the peer as often as the
   bus makes us. Falls back to a straight run when nothing is cabled. */
static void linkRunFrame(unsigned cyclesPassed)
{
   uint64_t target = link_tick + cyclesPassed;
   unsigned peers  = 0;
   int      stalls = 0;

   if(link_iface.peers(link_port, &peers) < 0 || peers < 2)
   {
      /* Nothing on the other end. Still advance the tick so a peer that
         arrives later does not find us in the distant past. */
      unsigned i;

      vmu->serial->setCabled(false);
      vmu->linkConnectorBits = 0;

      for(i = 0; i < cyclesPassed; i++)
         vmu->runCycle();

      link_tick = target;
      link_safe = target;
      return;
   }

   vmu->serial->setCabled(true);

   /* Connector pin 6, which is how software notices a peer is there at all,
      and what it checks before going near the serial port. */
   vmu->linkConnectorBits = 0x08;

   while(link_tick < target)
   {
      uint32_t wake  = RETRO_LINK_WAKE_NONE;
      uint64_t step  = link_tick + LINK_HORIZON;
      uint64_t grant;

      /* One horizon, never the whole frame. advance() blocks until the whole
         request can be granted and a peer only promises a horizon past where
         it stands, so two cores that each asked for a frame would deadlock,
         each waiting for the other to promise a frame it cannot promise
         without first running. A horizon at a time makes them leapfrog. */
      if(step > target)
         step = target;

      linkDrainIncoming();

      link_safe = step;
      grant     = link_iface.advance(link_port, link_tick, link_safe,
            step, &wake);

      if(grant == RETRO_LINK_UNBOUNDED || grant > step)
         grant = step;

      if(grant <= link_tick)
      {
         /* No ground given. That is normal when a peer woke us to hand over a
            message, and the next pass will pick it up -- but a frontend that
            keeps saying no must not wedge the emulation thread. */
         if(wake & RETRO_LINK_WAKE_DETACHED)
            break;

         if(wake == RETRO_LINK_WAKE_NONE && ++stalls > 64)
            break;

         continue;
      }

      stalls = 0;

      while(link_tick < grant)
      {
         vmu->runCycle();
         link_tick++;
      }

      linkPumpOutgoing();
   }

   /* A grant may have fallen short of the frame. Let the machine finish it
      anyway so video and audio keep their cadence; the tick stays honest. */
   while(link_tick < target)
   {
      vmu->runCycle();
      link_tick++;
   }
}

RETRO_API void retro_set_environment(retro_environment_t env)
{
   struct retro_vfs_interface_info vfs_iface_info;

   environment_cb = env;

   env(RETRO_ENVIRONMENT_SET_VARIABLES, options);

   {
      struct retro_log_callback log;
      log.log = NULL;
      if (env(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log) && log.log)
         log_cb = log.log;
      else
         log_cb = fallback_log;
   }

   /* Take the frontend's VFS when it offers one. Every file this core touches
      already goes through RFILE - the ROM in retro_load_game, the flash save
      file, VMU::loadBIOS - so this hand-off is all that is needed for paths
      stdio cannot open on its own. Android's SAF hands out content:// URIs,
      and this core is need_fullpath, so it opens those paths itself. */
   vfs_iface_info.required_interface_version = 1;
   vfs_iface_info.iface                      = NULL;
   if (env(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
      filestream_vfs_init(&vfs_iface_info);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t vr)
{
	video_cb = vr;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t sample)
{
	audio_cb = sample;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t batch)
{
	audio_batch_cb = batch;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t ipoll)
{
	input_poll_cb = ipoll;
}

RETRO_API void retro_set_input_state(retro_input_state_t istate)
{
	input_state_cb = istate;
}

RETRO_API void retro_init(void)
{
	frameBuffer = (uint16_t*)calloc(SCREEN_WIDTH*SCREEN_HEIGHT, sizeof(uint16_t));
	vmu         = new VMU(frameBuffer);
}

RETRO_API void retro_deinit(void)
{
	delete vmu;
	if(frameBuffer)
      free(frameBuffer);
	if(romData)
      free(romData);
   frameBuffer = NULL;
   romData     = NULL;
}

RETRO_API unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
	info->library_name     = "VeMUlator";
	info->library_version  = "0.1";
	info->valid_extensions = "vms|bin|dci";
	info->need_fullpath    = true;
	info->block_extract    = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
	info->geometry.base_width   = SCREEN_WIDTH;
	info->geometry.base_height  = SCREEN_HEIGHT;
	info->geometry.max_width    = SCREEN_WIDTH;
	info->geometry.max_height   = SCREEN_HEIGHT;
	info->geometry.aspect_ratio = 0;
	
	info->timing.fps            = FPS;
	info->timing.sample_rate    = SAMPLE_RATE;
}

RETRO_API void retro_set_controller_port_device(
      unsigned port, unsigned device)
{
	
}
 
void processInput()
{
   byte P3_reg, P3_int;
   int pressFlag = 0;

   input_poll_cb();

   if(!vmu->cpu->P3_taken)
      return;	//Don't accept new input until previous is processed

   P3_reg = vmu->ram->readByte_RAW(P3);
   P3_int = vmu->ram->readByte_RAW(P3INT);
   P3_reg = ~P3_reg;	//Active low

   //Up
   if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
   {
      P3_reg |= 1;
      pressFlag++;
   }
   else P3_reg &= 0xFE;

   //Down
   if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
   {
      P3_reg |= 2;
      pressFlag++;
   }
   else P3_reg &= 0xFD;

   //Left
   if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
   {
      P3_reg |= 4;
      pressFlag++;
   }
   else P3_reg &= 0xFB;

   //Right
   if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
   {
      P3_reg |= 8;
      pressFlag++;
   }
   else P3_reg &= 0xF7;

   //A
   if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))
   {
      P3_reg |= 16;
      pressFlag++;
   }
   else P3_reg &= 0xEF;

   //B
   if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
   {
      P3_reg |= 32;
      pressFlag++;
   }
   else P3_reg &= 0xDF;

   //Start
   if(input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))
   {
      //Clicking MODE without a BIOS causes hang
      //P3_reg |= 64;
      //pressFlag++;
   }
   //else P3_reg &= 0xBF;

   P3_reg = ~P3_reg;

   vmu->ram->writeByte_RAW(P3, P3_reg);

   if(pressFlag)
   {
      vmu->ram->writeByte_RAW(P3INT, P3_int | 2);
      vmu->intHandler->setP3();
      vmu->cpu->P3_taken = false;
   }

}

RETRO_API void retro_reset(void)
{
	vmu->reset();
}

RETRO_API void retro_run(void)
{
   unsigned i;
   unsigned int cyclesPassed;
	processInput();
	
	//Cycles passed since last screen refresh
	cyclesPassed = vmu->cpu->getCurrentFrequency() / FPS;
	
#ifdef VMU_LINK_SELFTEST
	/* Compiled out of every normal build. Stands in for guest code that drives
	   the serial port, so the link transport can be exercised end to end
	   without a mini-game that does it. */
	{
		static unsigned selftestFrame;

		/* CTRL has to be seen going 0 -> 1 by a cycle-sampling model, so the
		   clear and the set are put in different frames. Guest code gets this
		   for free: instructions take cycles. */
		switch(++selftestFrame % 40)
		{
			case 0:   /* clock a byte out */
				vmu->ram->writeByte_RAW(P1DDR, 0x05);
				vmu->ram->writeByte_RAW(P1FCR, 0x05);
				vmu->ram->writeByte_RAW(SBR,   0xF0);
				vmu->ram->writeByte_RAW(SBUF0, (byte)(selftestFrame / 40));
				vmu->ram->writeByte_RAW(SCON0, 0x08);
				break;
			case 10:  /* back to idle */
				vmu->ram->writeByte_RAW(SCON0, 0x00);
				break;
			case 20:  /* listen for one */
				vmu->ram->writeByte_RAW(P1DDR, 0x00);
				vmu->ram->writeByte_RAW(SCON0, 0x08);
				break;
			case 35:
				vmu->ram->writeByte_RAW(SCON0, 0x00);
				break;
		}
	}
#endif

	if(link_port)
		linkRunFrame(cyclesPassed);
	else
		for(i = 0; i < cyclesPassed; i++)
			vmu->runCycle();

	//Video
	vmu->video->drawFrame(frameBuffer);
	if(vmu->ram->readByte_RAW(MCR) & 8)
      video_cb(frameBuffer, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * 2);
	
	//Audio
	vmu->audio->generateSignal(audio_cb);
}

RETRO_API size_t retro_serialize_size(void) { return 0; }
RETRO_API bool retro_serialize(void *data, size_t size) {return false;}
RETRO_API bool retro_unserialize(const void *data, size_t size) {return false;}

RETRO_API void retro_cheat_reset(void) { }

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
   size_t i, romSize;
   //Set environment variables
   enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
   environment_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);

   //Opening file
   RFILE *rom = rfopen(game->path, "rb");
   if(!rom)
      return false;
   rfseek(rom, 0, SEEK_END);
   romSize = rftell(rom);
   rfseek(rom, 0 , SEEK_SET);

   romData = (byte *)malloc(romSize);
   for(i = 0; i < romSize; i++)
      romData[i] = rfgetc(rom);

   rfclose(rom);

   //Check extension
   char *path = (char *)malloc(strlen(game->path) + 1);
   strcpy(path, game->path);
   char *ext = strchr(path, '.');

   //Check needed variables
   struct retro_variable var = {0};
   var.key = "enable_flash_write";
   environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);

   //Loading ROM
   if(!strcmp(ext, ".bin") || !strcmp(ext, ".BIN")) 
   {
      //Check if user wants core to be able to write to flash
      if(!strcmp(var.value, "enabled")) vmu->flash->loadROM(romData, romSize, 0, game->path, true);
      else vmu->flash->loadROM(romData, romSize, 0, game->path, false);
   }
   else if(!strcmp(ext, ".vms") || !strcmp(ext, ".VMS")) vmu->flash->loadROM(romData, romSize, 1, game->path, false);
   else if(!strcmp(ext, ".dci") || !strcmp(ext, ".DCI")) vmu->flash->loadROM(romData, romSize, 2, game->path, false);

   free(path);

   //Joining the link bus, if the frontend hosts one
   linkAttach();

   //Initializing system
   vmu->startCPU();

   return true;
}

RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
	return false;
}

RETRO_API void retro_unload_game(void)
{
	linkDetach();
	vmu->reset();
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void *retro_get_memory_data(unsigned id) { return NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return 0; }
