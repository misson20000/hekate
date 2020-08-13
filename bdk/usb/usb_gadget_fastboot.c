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
	FASTBOOT_STATUS_REBOOT_BOOTLOADER,
};

enum fastboot_rx_state {
	FASTBOOT_RX_STATE_INVALID,
	
	FASTBOOT_RX_STATE_IDLE,
	FASTBOOT_RX_STATE_COMMAND,
	FASTBOOT_RX_STATE_DOWNLOAD,

	FASTBOOT_RX_STATE_WAITING_TX_FOR_PROCESS,
	FASTBOOT_RX_STATE_WAITING_TX_FOR_REBOOT_BOOTLOADER,
};

enum fastboot_tx_state {
	FASTBOOT_TX_STATE_INVALID,
	
	FASTBOOT_TX_STATE_IDLE,
	FASTBOOT_TX_STATE_SEND_RESPONSE,
};

enum fastboot_response_type {
	FASTBOOT_RESPONSE_INFO,
	FASTBOOT_RESPONSE_FAIL,
	FASTBOOT_RESPONSE_OKAY,
	FASTBOOT_RESPONSE_DATA,
};

enum fastboot_disposition {
	FASTBOOT_DISPOSITION_NORMAL,
	FASTBOOT_DISPOSITION_DOWNLOAD,
	// FASTBOOT_DISPOSITION_UPLOAD,
	FASTBOOT_DISPOSITION_REBOOT_BOOTLOADER,
};

// REVIEW ME: is it ok to reuse this memory? or should I add something to memory_map.h?
static u8 *const fastboot_download_buffer = (void*) RAM_DISK_ADDR;
static const u32 fastboot_download_capacity = RAM_DISK_SZ;

#define FASTBOOT_COMMAND_BUFFER_SIZE 64

typedef struct _usbd_gadget_fastboot_t {
	enum fastboot_status status;
	enum fastboot_rx_state rx_state;
	enum fastboot_tx_state tx_state;
	bool tight_turnaround;
	
	// +1 for null terminator because we use string functions
	char rx_buffer[FASTBOOT_COMMAND_BUFFER_SIZE + 1];
	u32 rx_length;

	u32 download_head;
	u32 download_size;
	u32 download_amount;
	
	// +1 for null terminator because we use string functions
	char tx_buffer[FASTBOOT_COMMAND_BUFFER_SIZE + 1];
	u32 tx_length;
	
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

static void fastboot_rx_enter_idle(usbd_gadget_fastboot_t *fastboot);
static void fastboot_rx_enter_command(usbd_gadget_fastboot_t *fastboot);
static void fastboot_rx_enter_download(usbd_gadget_fastboot_t *fastboot);
static void fastboot_rx_enter_waiting_tx_for_process(usbd_gadget_fastboot_t *fastboot);
static void fastboot_rx_enter_waiting_tx_for_reboot_bootloader(usbd_gadget_fastboot_t *fastboot);

static void fastboot_tx_enter_idle(usbd_gadget_fastboot_t *fastboot);
static void fastboot_tx_enter_send_response(usbd_gadget_fastboot_t *fastboot);

static void fastboot_send_response(usbd_gadget_fastboot_t *fastboot, enum fastboot_response_type type, enum fastboot_disposition disposition, const char *message)
{
	//fastboot->set_text(fastboot->label, "#C7EA46 Status:# Sending response");

	memset(fastboot->tx_buffer, 0, sizeof(fastboot->tx_buffer));
	
	switch (type)
	{
	case FASTBOOT_RESPONSE_INFO:
		strcpy(fastboot->tx_buffer, "INFO");
		break;
	case FASTBOOT_RESPONSE_FAIL:
		strcpy(fastboot->tx_buffer, "FAIL");
		break;
	case FASTBOOT_RESPONSE_OKAY:
		strcpy(fastboot->tx_buffer, "OKAY");
		break;
	case FASTBOOT_RESPONSE_DATA:
		strcpy(fastboot->tx_buffer, "DATA");
		break;
	}

	if (message != NULL)
	{
		strncpy(fastboot->tx_buffer + 4, message, sizeof(fastboot->tx_buffer) - 4);
	}

	// need to prepare for rx before we send response because it is possible for host to turn around very fast
	// and send another command before we get a chance to turn around ourselves.
	switch (disposition)
	{
	case FASTBOOT_DISPOSITION_NORMAL:
		fastboot_rx_enter_command(fastboot);
		break;
	case FASTBOOT_DISPOSITION_DOWNLOAD:
		fastboot_rx_enter_download(fastboot);
		break;
	case FASTBOOT_DISPOSITION_REBOOT_BOOTLOADER:
		fastboot_rx_enter_waiting_tx_for_reboot_bootloader(fastboot);
		break;
	}

	fastboot_tx_enter_send_response(fastboot);
}

static void fastboot_handle_command(usbd_gadget_fastboot_t *fastboot)
{
	//fastboot->set_text(fastboot->label, "#C7EA46 Status:# Handling command");

	const char *command = fastboot->rx_buffer;
	
	if (strncmp(command, "getvar:", 7) == 0)
	{
		const char *variable = command + 7;
		if (strcmp(variable, "version") == 0)
		{
			fastboot_send_response(fastboot, FASTBOOT_RESPONSE_OKAY, FASTBOOT_DISPOSITION_NORMAL, "0.4");
		}
		else if (strcmp(variable, "product") == 0)
		{
			fastboot_send_response(fastboot, FASTBOOT_RESPONSE_OKAY, FASTBOOT_DISPOSITION_NORMAL, "Nyx");
		}
		else if (strcmp(variable, "max-download-size") == 0)
		{
			char message[9];
			s_printf(message, "%08X", fastboot_download_capacity);
			fastboot_send_response(fastboot, FASTBOOT_RESPONSE_OKAY, FASTBOOT_DISPOSITION_NORMAL, message);
		}
		else
		{
			fastboot_send_response(fastboot, FASTBOOT_RESPONSE_FAIL, FASTBOOT_DISPOSITION_NORMAL, "unknown variable");
		}
	}
	else if (strcmp(command, "reboot-bootloader") == 0)
	{
		fastboot_send_response(fastboot, FASTBOOT_RESPONSE_OKAY, FASTBOOT_DISPOSITION_REBOOT_BOOTLOADER, "");
	}
	else if (strncmp(command, "download:", 9) == 0)
	{
		bool parsed_int_ok = true;
		u32 download_size = 0;
		for (int i = 9; i < 9+8; i++)
		{
			download_size<<= 4;
			char ch = command[i];
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
			fastboot_send_response(fastboot, FASTBOOT_RESPONSE_FAIL, FASTBOOT_DISPOSITION_NORMAL, "failed to parse size");
			return;
		}

		if (download_size > fastboot_download_capacity)
		{
			fastboot_send_response(fastboot, FASTBOOT_RESPONSE_FAIL, FASTBOOT_DISPOSITION_NORMAL, "download size too large");
			return;
		}
		
		fastboot->download_head = 0;
		fastboot->download_amount = 0;
		fastboot->download_size = download_size;

		char message[9];
		s_printf(message, "%08x", download_size);
		fastboot_send_response(fastboot, FASTBOOT_RESPONSE_DATA, FASTBOOT_DISPOSITION_DOWNLOAD, message);
	}
	else
	{
		char message[61] = "unknown command: ";
		strncat(message, command, sizeof(message)-strlen(message));
		message[60] = '\0';
		
		fastboot_send_response(fastboot, FASTBOOT_RESPONSE_FAIL, FASTBOOT_DISPOSITION_NORMAL, message);
	}
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

static void fastboot_rx_state_idle(usbd_gadget_fastboot_t *fastboot);
static void fastboot_rx_state_command(usbd_gadget_fastboot_t *fastboot);
static void fastboot_rx_state_download(usbd_gadget_fastboot_t *fastboot);
static void fastboot_rx_state_waiting_tx_for_process(usbd_gadget_fastboot_t *fastboot);
static void fastboot_rx_state_waiting_tx_for_reboot_bootloader(usbd_gadget_fastboot_t *fastboot);

static void fastboot_tx_state_idle(usbd_gadget_fastboot_t *fastboot);
static void fastboot_tx_state_send_response(usbd_gadget_fastboot_t *fastboot);

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
	
	fastboot.label = usbs->label;
	fastboot.set_text = usbs->set_text;
	fastboot.system_maintenance = usbs->system_maintenance;
	fastboot.reload_nyx = usbs->reload_nyx;

	if (usb_device_ep0_initialize(USB_GADGET_FASTBOOT))
		goto error;

	fastboot_handle_ep0_ctrl(&fastboot);

	fastboot_rx_enter_command(&fastboot);
	fastboot_tx_enter_idle(&fastboot);
	
	while (fastboot.status == FASTBOOT_STATUS_NORMAL)
	{
		if (!fastboot.tight_turnaround)
		{
			// Do DRAM training and update system tasks.
			_system_maintenance(&fastboot);
		}
		
		// Check for suspended USB in case the cable was pulled.
		if (usb_device_get_suspended())
			break; // Disconnected.

		fastboot_handle_ep0_ctrl(&fastboot);

		const char *rx_state_name = "invalid";
		
		switch (fastboot.rx_state)
		{
		case FASTBOOT_RX_STATE_IDLE:
			rx_state_name = "idle";
			fastboot_rx_state_idle(&fastboot);
			break;
		case FASTBOOT_RX_STATE_COMMAND:
			rx_state_name = "command";
			fastboot_rx_state_command(&fastboot);
			break;
		case FASTBOOT_RX_STATE_DOWNLOAD:
			rx_state_name = "download";
			fastboot_rx_state_download(&fastboot);
			break;
		case FASTBOOT_RX_STATE_WAITING_TX_FOR_PROCESS:
			rx_state_name = "wtx process";
			fastboot_rx_state_waiting_tx_for_process(&fastboot);
			break;
		case FASTBOOT_RX_STATE_WAITING_TX_FOR_REBOOT_BOOTLOADER:
			rx_state_name = "wtx reboot";
			fastboot_rx_state_waiting_tx_for_reboot_bootloader(&fastboot);
			break;
		default:
			fastboot_set_status(&fastboot, FASTBOOT_STATUS_INVALID_STATE);
			break;
		}

		const char *tx_state_name = "invalid";

		switch (fastboot.tx_state)
		{
		case FASTBOOT_TX_STATE_IDLE:
			tx_state_name = "idle";
			fastboot_tx_state_idle(&fastboot);
			break;
		case FASTBOOT_TX_STATE_SEND_RESPONSE:
			tx_state_name = "send response";
			fastboot_tx_state_send_response(&fastboot);
			break;
		default:
			fastboot_set_status(&fastboot, FASTBOOT_STATUS_INVALID_STATE);
			break;
		}

		if (!fastboot.tight_turnaround)
		{
			char text[128];
			s_printf(text, "#C7EA46 RX State:# %s\n#C7EA46 TX State:# %s", rx_state_name, tx_state_name);
			usbs->set_text(usbs->label, text);
		}
	}

	switch (fastboot.status)
	{
	case FASTBOOT_STATUS_NORMAL:
		usbs->set_text(usbs->label, "#C7EA46 Status:# Fastboot ended");
		goto exit;
	case FASTBOOT_STATUS_PROTOCOL_RESET:
		usbs->set_text(usbs->label, "#C7EA46 Status:# Fastboot ended (protocol reset)");
		goto exit;
	case FASTBOOT_STATUS_INVALID_STATE: {
		char text[128];
		s_printf(text, "#C7EA46 Status:# Fastboot ended (invalid state: %d/%d)", fastboot.rx_state, fastboot.tx_state);
		usbs->set_text(usbs->label, text);
		goto exit; }
	case FASTBOOT_STATUS_USB_ERROR:
		usbs->set_text(usbs->label, "#C7EA46 Status:# Fastboot ended (usb error)");
		goto exit;
	case FASTBOOT_STATUS_REBOOT_BOOTLOADER:
		usbs->set_text(usbs->label, "#C7EA46 Status:# Fastboot ended (rebooting bootloader)");
		goto exit;
	}
	
error:
	usbs->set_text(usbs->label, "#C7EA46 Status:# Timed out or canceled");
	res = 1;

exit:
	usbd_end(true, false);

	if (fastboot.status == FASTBOOT_STATUS_REBOOT_BOOTLOADER)
	{
		fastboot.reload_nyx();
	}
	
	return res;
}

// rx state entry

static void fastboot_rx_enter_idle(usbd_gadget_fastboot_t *fastboot)
{
	fastboot->rx_state = FASTBOOT_RX_STATE_IDLE;
}

static void fastboot_rx_enter_command(usbd_gadget_fastboot_t *fastboot)
{
	if (usb_device_read_ep1_out((u8*) fastboot->rx_buffer, FASTBOOT_COMMAND_BUFFER_SIZE, &fastboot->rx_length, false))
		fastboot_set_status(fastboot, FASTBOOT_STATUS_USB_ERROR);
	
	fastboot->rx_state = FASTBOOT_RX_STATE_COMMAND;
}

static void fastboot_rx_enter_waiting_tx_for_process(usbd_gadget_fastboot_t *fastboot)
{
	fastboot->rx_state = FASTBOOT_RX_STATE_WAITING_TX_FOR_PROCESS;
}

static void fastboot_rx_enter_waiting_tx_for_reboot_bootloader(usbd_gadget_fastboot_t *fastboot)
{
	fastboot->rx_state = FASTBOOT_RX_STATE_WAITING_TX_FOR_REBOOT_BOOTLOADER;
}

static void fastboot_rx_enter_download(usbd_gadget_fastboot_t *fastboot)
{
	if (fastboot->download_head < fastboot->download_size)
	{
		char buffer[64];
		s_printf(buffer, "#C7EA46 Status:# Downloading (%d/%d KiB)", fastboot->download_head / 1024, fastboot->download_size / 1024);
		fastboot->set_text(fastboot->label, buffer);

		if (usb_device_read_ep1_out(
			    fastboot_download_buffer + fastboot->download_head,
			    MIN(fastboot->download_size - fastboot->download_head,
			        USB_EP_BUFFER_MAX_SIZE),
			    &fastboot->download_amount, false))
			fastboot_set_status(fastboot, FASTBOOT_STATUS_USB_ERROR);
		
		fastboot->rx_state = FASTBOOT_RX_STATE_DOWNLOAD;
	}
	else
	{
		fastboot->tight_turnaround = false;

		fastboot_send_response(fastboot, FASTBOOT_RESPONSE_OKAY, FASTBOOT_DISPOSITION_NORMAL, "got it!");
	}
}

// tx state entry
static void fastboot_tx_enter_idle(usbd_gadget_fastboot_t *fastboot)
{
	fastboot->tx_state = FASTBOOT_TX_STATE_IDLE;
}

static void fastboot_tx_enter_send_response(usbd_gadget_fastboot_t *fastboot)
{
	if (usb_device_write_ep1_in((u8*) fastboot->tx_buffer, strlen(fastboot->tx_buffer), &fastboot->tx_length, false))
		fastboot_set_status(fastboot, FASTBOOT_STATUS_USB_ERROR);

	fastboot->tx_state = FASTBOOT_TX_STATE_SEND_RESPONSE;
}

// rx state process
static void fastboot_rx_state_idle(usbd_gadget_fastboot_t *fastboot)
{
	// we should never actually wind up here, I think?
	return;
}

static void fastboot_rx_state_command(usbd_gadget_fastboot_t *fastboot)
{
	switch (usb_device_ep1_out_reading_poll(&fastboot->rx_length))
	{
	case 0: // ok
		break;
	case 3: // still active... wait a bit longer
		return;
	default: // error
		fastboot_set_status(fastboot, FASTBOOT_STATUS_USB_ERROR);
		return;
	}

	// so we can use string functions
	fastboot->rx_buffer[fastboot->rx_length] = '\0';

	fastboot_rx_enter_waiting_tx_for_process(fastboot);
}

static void fastboot_rx_state_download(usbd_gadget_fastboot_t *fastboot)
{
	switch (usb_device_ep1_out_reading_poll(&fastboot->download_amount))
	{
	case 0: // ok
		break;
	case 3: // still active... wait a bit longer
		return;
	default: // error
		fastboot_set_status(fastboot, FASTBOOT_STATUS_USB_ERROR);
		return;
	}
	
	fastboot->download_head+= fastboot->download_amount;

	fastboot_rx_enter_download(fastboot);
}

static void fastboot_rx_state_waiting_tx_for_process(usbd_gadget_fastboot_t *fastboot)
{
	/*
	  We only stay in this state if the host does something strange with sending commands too fast.

	  Host:   "getvar:version"
	  (client handles command)
	  (client begins to read another command to be safe for fast host turnaround)
	  (client begins to send response, but does not finish)
	  Host:   "download:00001234"
	  (client needs to wait until it has finished sending first response to begin handling next command)
	 */

	if (fastboot->tx_state == FASTBOOT_TX_STATE_IDLE)
	{
		fastboot_rx_enter_idle(fastboot);
		fastboot_handle_command(fastboot);
	}
}

static void fastboot_rx_state_waiting_tx_for_reboot_bootloader(usbd_gadget_fastboot_t *fastboot)
{
	if (fastboot->tx_state == FASTBOOT_TX_STATE_IDLE)
	{
		fastboot_rx_enter_idle(fastboot);
		fastboot_set_status(fastboot, FASTBOOT_STATUS_REBOOT_BOOTLOADER);
	}
}

// tx state process
static void fastboot_tx_state_idle(usbd_gadget_fastboot_t *fastboot)
{
	return;
}

static void fastboot_tx_state_send_response(usbd_gadget_fastboot_t *fastboot)
{
	switch (usb_device_ep1_in_writing_poll(&fastboot->tx_length))
	{
	case 0: // ok
		break;
	case 3: // still active... wait a bit longer
		return;
	default: // error
		fastboot_set_status(fastboot, FASTBOOT_STATUS_USB_ERROR);
		return;
	}

	fastboot_tx_enter_idle(fastboot); // rx state machine will pick up on this if it cares
}
