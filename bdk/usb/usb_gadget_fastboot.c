/*
 * USB Gadget FastBoot driver for Tegra X1
 * 
 * Implements FastBoot 0.4, as described by this document
 *   https://android.googlesource.com/platform/system/core/+/refs/heads/master/fastboot/README.md
 *
 * Copyright (c) 2020 misson20000
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include <memory_map.h>
#include <mem/heap.h>
#include <usb/usbd.h>
#include <utils/util.h>
#include <utils/sprintf.h>

enum fastboot_status {
	FASTBOOT_STATUS_NORMAL,
	FASTBOOT_STATUS_PROTOCOL_RESET,
	FASTBOOT_STATUS_INVALID_STATE,
	FASTBOOT_STATUS_USB_ERROR,
};

enum fastboot_state {
	FASTBOOT_STATE_INITIAL,
	FASTBOOT_STATE_RECEIVE_HOST_COMMAND,
	FASTBOOT_STATE_SEND_RESPONSE,
	FASTBOOT_STATE_DOWNLOAD,
};

enum fastboot_response_type {
	FASTBOOT_RESPONSE_INFO,
	FASTBOOT_RESPONSE_FAIL,
	FASTBOOT_RESPONSE_OKAY,
	FASTBOOT_RESPONSE_DATA,
};

enum fastboot_disposition {
	FASTBOOT_DISPOSITION_CANCEL, // go back to RECEIVE_HOST_COMMAND
	FASTBOOT_DISPOSITION_DOWNLOAD,
};

// REVIEW ME: is it ok to reuse this memory? or should I add something to memory_map.h?
static u8 *const fastboot_download_buffer = (void*) RAM_DISK_ADDR;
static const u32 fastboot_download_capacity = RAM_DISK_SZ;

typedef struct _usbd_gadget_fastboot_t {
	enum fastboot_status status;
	enum fastboot_state state;

	union {
		struct {
			enum fastboot_disposition disposition;
		} send_response;

		struct {
			int code;
		} usb_error;
	};
	
	u32 buffer_used;
	char buffer[65];

	u32 download_head;
	u32 download_size;
	u32 download_amount;

	void (*system_maintenance)(bool);
	void *label;
	void (*set_text)(void *, const char *);
	void (*reload_nyx)();
} usbd_gadget_fastboot_t;


static bool fastboot_set_status(usbd_gadget_fastboot_t *fastboot, enum fastboot_status new_status)
{
	if (fastboot->status <= new_status)
	{
		fastboot->status = new_status;
		return true;
	}
	return false;
}

static void fastboot_handle_ep0_ctrl(usbd_gadget_fastboot_t *fastboot)
{
	if (usbd_handle_ep0_pending_control_transfer())
		fastboot_set_status(fastboot, FASTBOOT_STATUS_PROTOCOL_RESET);
}

static bool fastboot_check_code(usbd_gadget_fastboot_t *fastboot, int res)
{
	if (res == 0)
	{
		return true;
	}

	// return false so we exit out to main loop and yield, but leave status
	// normal so we try again.
	if (res == 3)
	{
		return false;
	}

	if (fastboot_set_status(fastboot, FASTBOOT_STATUS_USB_ERROR))
	{
		fastboot->usb_error.code = res;
	}
	
	return false;
}

static void fastboot_enter_receive_host_command(usbd_gadget_fastboot_t *fastboot)
{
	//fastboot->set_text(fastboot->label, "#C7EA46 Status:# Ready");

	fastboot->state = FASTBOOT_STATE_RECEIVE_HOST_COMMAND;
}

static void fastboot_enter_send_response(usbd_gadget_fastboot_t *fastboot, enum fastboot_response_type type, enum fastboot_disposition disposition, const char *message)
{
	//fastboot->set_text(fastboot->label, "#C7EA46 Status:# Sending response");
	switch (type)
	{
	case FASTBOOT_RESPONSE_INFO:
		strcpy(fastboot->buffer, "INFO");
		break;
	case FASTBOOT_RESPONSE_FAIL:
		strcpy(fastboot->buffer, "FAIL");
		break;
	case FASTBOOT_RESPONSE_OKAY:
		strcpy(fastboot->buffer, "OKAY");
		break;
	case FASTBOOT_RESPONSE_DATA:
		strcpy(fastboot->buffer, "DATA");
		break;
	}

	if (message != NULL)
	{
		strncpy(fastboot->buffer + 4, message, sizeof(fastboot->buffer) - 4);
	}

	fastboot->send_response.disposition = disposition;

	fastboot->state = FASTBOOT_STATE_SEND_RESPONSE;
}

static void fastboot_enter_download(usbd_gadget_fastboot_t *fastboot)
{
	if (fastboot->download_head < fastboot->download_size)
	{
		char buffer[64];
		s_printf(buffer, "#C7EA46 Status:# Downloading (%d/%d KiB)", fastboot->download_head / 1024, fastboot->download_size / 1024);
		fastboot->set_text(fastboot->label, buffer);
		
		fastboot->state = FASTBOOT_STATE_DOWNLOAD;
	}
	else
	{
		fastboot_enter_send_response(fastboot, FASTBOOT_RESPONSE_OKAY, FASTBOOT_DISPOSITION_CANCEL, "got it!");
	}
}

static void fastboot_handle_command(usbd_gadget_fastboot_t *fastboot)
{
	fastboot->set_text(fastboot->label, "#C7EA46 Status:# Handling command");
	
	fastboot->buffer[fastboot->buffer_used] = '\0';

	if (strncmp(fastboot->buffer, "getvar:", 7) == 0)
	{
		const char *variable = fastboot->buffer + 7;
		if (strcmp(variable, "version") == 0)
		{
			fastboot_enter_send_response(fastboot, FASTBOOT_RESPONSE_OKAY, FASTBOOT_DISPOSITION_CANCEL, "0.4");
		}
		else if (strcmp(variable, "product") == 0)
		{
			fastboot_enter_send_response(fastboot, FASTBOOT_RESPONSE_OKAY, FASTBOOT_DISPOSITION_CANCEL, "Nyx");
		}
		else if (strcmp(variable, "max-download-size") == 0)
		{
			char message[9];
			s_printf(message, "%08X", fastboot_download_capacity);
			fastboot_enter_send_response(fastboot, FASTBOOT_RESPONSE_OKAY, FASTBOOT_DISPOSITION_CANCEL, message);
		}
		else
		{
			fastboot_enter_send_response(fastboot, FASTBOOT_RESPONSE_FAIL, FASTBOOT_DISPOSITION_CANCEL, "unknown variable");
		}
	}
	else if (strcmp(fastboot->buffer, "reboot-bootloader") == 0)
	{
		fastboot_enter_send_response(fastboot, FASTBOOT_RESPONSE_OKAY, FASTBOOT_DISPOSITION_CANCEL, "");
		usbd_end(false, true);
		fastboot->reload_nyx();
	}
	else if (strncmp(fastboot->buffer, "download:", 9) == 0)
	{
		bool parsed_int_ok = true;
		u32 download_size = 0;
		for (int i = 9; i < 9+8; i++)
		{
			download_size<<= 4;
			char ch = fastboot->buffer[i];
			if (ch >= '0' && ch <= '9')
			{
				download_size|= ch - '0';
			}
			else if (ch >= 'a' && ch <= 'f')
			{
				download_size|= ch - 'a' + 0xa;
			}
			else if (ch >= 'A' && ch <= 'F')
			{
				download_size|= ch - 'A' + 0xa;
			}
			else {
				parsed_int_ok = false;
				break;
			}
		}

		if (!parsed_int_ok)
		{
			fastboot_enter_send_response(fastboot, FASTBOOT_RESPONSE_FAIL, FASTBOOT_DISPOSITION_CANCEL, "failed to parse size");
			return;
		}

		if (download_size > fastboot_download_capacity)
		{
			fastboot_enter_send_response(fastboot, FASTBOOT_RESPONSE_FAIL, FASTBOOT_DISPOSITION_CANCEL, "download size too large");
			return;
		}
		
		fastboot->download_head = 0;
		fastboot->download_amount = 0;
		fastboot->download_size = download_size;

		char message[9];
		s_printf(message, "%08x", download_size);
		fastboot_enter_send_response(fastboot, FASTBOOT_RESPONSE_DATA, FASTBOOT_DISPOSITION_DOWNLOAD, message);
	}
	else
	{
		char buffer[61] = "unknown command: ";
		strncat(buffer, fastboot->buffer, sizeof(buffer)-strlen(buffer));
		buffer[60] = '\0';
		
		fastboot_enter_send_response(fastboot, FASTBOOT_RESPONSE_FAIL, FASTBOOT_DISPOSITION_CANCEL, buffer);
	}
}

static void fastboot_state_initial(usbd_gadget_fastboot_t *fastboot)
{
	fastboot_enter_receive_host_command(fastboot);
}

static void fastboot_state_receive_host_command(usbd_gadget_fastboot_t *fastboot)
{
  // hurts to be synchronous
	if (!fastboot_check_code(fastboot, usb_device_read_ep1_out((u8*) fastboot->buffer, sizeof(fastboot->buffer)-1, &fastboot->buffer_used, true)))
		return; // timed out... try again in a little bit

	fastboot_handle_command(fastboot);
}

static void fastboot_state_send_response(usbd_gadget_fastboot_t *fastboot)
{
	// hurts to be synchronous
	if (!fastboot_check_code(fastboot, usb_device_write_ep1_in((u8*) fastboot->buffer, strlen(fastboot->buffer), &fastboot->buffer_used, true)))
		return; // timed out... try again in a little bit

	switch (fastboot->send_response.disposition)
	{
	case FASTBOOT_DISPOSITION_CANCEL:
		fastboot_enter_receive_host_command(fastboot);
		break;
	case FASTBOOT_DISPOSITION_DOWNLOAD:
		fastboot_enter_download(fastboot);
		break;
	}
}

static void fastboot_state_download(usbd_gadget_fastboot_t *fastboot)
{
	// hurts to be synchronous
	if (!fastboot_check_code(fastboot, usb_device_read_ep1_out(fastboot_download_buffer + fastboot->download_head, fastboot->download_size - fastboot->download_head, &fastboot->download_amount, true)))
		return; // timed out... try again in a little bit

	fastboot->download_head+= fastboot->download_amount;

	fastboot_enter_download(fastboot);
}

static inline void _system_maintenance(usbd_gadget_fastboot_t *fastboot)
{
	static u32 timer_dram = 0;
	static u32 timer_status_bar = 0;

	u32 time = get_tmr_ms();

	if (timer_dram < time)
	{
		minerva_periodic_training();
		timer_dram = get_tmr_ms() + 100;
	}
	else if (timer_status_bar < time)
	{
		fastboot->system_maintenance(true);
		timer_status_bar = get_tmr_ms() + 30000;
	}
}

int usb_device_gadget_fastboot(usb_ctxt_t *usbs)
{
	int res = 0;

	usbs->set_text(usbs->label, "#C7EA46 Status:# Started USB");

	if (usb_device_init())
	{
		usbd_end(false, true);
		return true;
	}

	usbd_gadget_fastboot_t fastboot;
	memset(&fastboot, 0, sizeof(fastboot));

	fastboot.status = FASTBOOT_STATUS_NORMAL;
	fastboot.state = FASTBOOT_STATE_INITIAL;
	
	fastboot.label = usbs->label;
	fastboot.set_text = usbs->set_text;
	fastboot.system_maintenance = usbs->system_maintenance;
	fastboot.reload_nyx = usbs->reload_nyx;

	if (usb_device_ep0_initialize(USB_GADGET_FASTBOOT))
		goto error;

	while (fastboot.status == FASTBOOT_STATUS_NORMAL)
	{
		
		// Check for suspended USB in case the cable was pulled.
		if (usb_device_get_suspended())
			break; // Disconnected.

		fastboot_handle_ep0_ctrl(&fastboot);

		switch (fastboot.state)
		{
		case FASTBOOT_STATE_INITIAL:
			fastboot_state_initial(&fastboot);
			break;
		case FASTBOOT_STATE_RECEIVE_HOST_COMMAND:
			// Do DRAM training and update system tasks here where fast turnaround is not required.
			_system_maintenance(&fastboot);
			fastboot_state_receive_host_command(&fastboot);
			break;
		case FASTBOOT_STATE_SEND_RESPONSE:
			fastboot_state_send_response(&fastboot);
			break;
		case FASTBOOT_STATE_DOWNLOAD:
			fastboot_state_download(&fastboot);
			break;
		default:
			fastboot_set_status(&fastboot, FASTBOOT_STATUS_INVALID_STATE);
			break;
		}
	}

	usbs->set_text(usbs->label, "#C7EA46 Status:# Fastboot ended");
	goto exit;

error:
	usbs->set_text(usbs->label, "#C7EA46 Status:# Timed out or canceled");
	res = 1;

exit:
	usbd_end(true, false);

	return res;
}
