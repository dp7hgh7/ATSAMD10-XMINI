/**
* \file
*
* \brief Battery Information Service - Application
*
* Copyright (c) 2016 Atmel Corporation. All rights reserved.
*
* \asf_license_start
*
* \page License
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice,
*    this list of conditions and the following disclaimer.
*
* 2. Redistributions in binary form must reproduce the above copyright notice,
*    this list of conditions and the following disclaimer in the documentation
*    and/or other materials provided with the distribution.
*
* 3. The name of Atmel may not be used to endorse or promote products derived
*    from this software without specific prior written permission.
*
* 4. This software may only be redistributed and used in connection with an
*    Atmel microcontroller product.
*
* THIS SOFTWARE IS PROVIDED BY ATMEL "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT ARE
* EXPRESSLY AND SPECIFICALLY DISCLAIMED. IN NO EVENT SHALL ATMEL BE LIABLE FOR
* ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
* OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
* HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
* STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
* ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
*
* \asf_license_stop
*
*/
/*
* Support and FAQ: visit <a href="http://www.atmel.com/design-support/">Atmel Support</a>
*/

/****************************************************************************************
*							        Includes	                                     	*
****************************************************************************************/
#include <asf.h>
#include "console_serial.h"
#include "at_ble_api.h"
#include "platform.h"
#include "battery.h"
#include "timer_hw.h"
#include "ble_utils.h"
#include "battery_info.h"
#include "ble_manager.h"
#include "aon_sleep_timer_basic.h"

#define _AON_TIMER_

/* === GLOBALS ============================================================ */
#define BATTERY_UPDATE_INTERVAL	(1) //1 second
#define BATTERY_MAX_LEVEL		(100)
#define BATTERY_MIN_LEVEL		(0)

uint8_t db_mem[1024] = {0};
bat_gatt_service_handler_t bas_service_handler;

bool volatile timer_cb_done = false;
bool volatile flag = true;
bool volatile battery_flag = true;
at_ble_handle_t bat_connection_handle;

void resume_cb(void);

/**
 * \Timer callback handler called on timer expiry
 */
static void aon_sleep_timer_callback(void)
{
	timer_cb_done = true;
	send_plf_int_msg_ind(USER_TIMER_CALLBACK, TIMER_EXPIRED_CALLBACK_TYPE_DETECT, NULL, 0);
}

/* Advertisement data set and Advertisement start */
static at_ble_status_t battery_service_advertise(void)
{
	at_ble_status_t status = AT_BLE_FAILURE;
	
	if((status = ble_advertisement_data_set()) != AT_BLE_SUCCESS)
	{
		DBG_LOG("advertisement data set failed reason :%d",status);
		return status;
	}
	
	/* Start of advertisement */
	if((status = at_ble_adv_start(AT_BLE_ADV_TYPE_UNDIRECTED, AT_BLE_ADV_GEN_DISCOVERABLE, NULL, AT_BLE_ADV_FP_ANY, APP_BAS_FAST_ADV, APP_BAS_ADV_TIMEOUT, 0)) == AT_BLE_SUCCESS)
	{
		DBG_LOG("BLE Started Adv");
		return AT_BLE_SUCCESS;
	}
	else
	{
		DBG_LOG("BLE Adv start Failed reason :%d",status);
	}
	return status;
}

/* Callback registered for AT_BLE_PAIR_DONE event from stack */
static at_ble_status_t ble_paired_app_event(void *param)
{
	timer_cb_done = false;
	ALL_UNUSED(param);
	return AT_BLE_SUCCESS;
}

/* Callback registered for AT_BLE_DISCONNECTED event from stack */
static at_ble_status_t ble_disconnected_app_event(void *param)
{
	timer_cb_done = false;
	flag = true;
	
	aon_sleep_timer_service_stop();
	battery_service_advertise();
	ALL_UNUSED(param);
	return AT_BLE_SUCCESS;
}

static at_ble_status_t ble_connected_app_event(void *param)
{
	at_ble_connected_t *connected = (at_ble_connected_t *)param;
	bat_connection_handle = connected->handle;
	#if !BLE_PAIR_ENABLE
		ble_paired_app_event(param);
	#else
		ALL_UNUSED(param);
	#endif
	return AT_BLE_SUCCESS;
}

/* Callback registered for AT_BLE_NOTIFICATION_CONFIRMED event from stack */
static at_ble_status_t ble_notification_confirmed_app_event(void *param)
{
	at_ble_cmd_complete_event_t *notification_status = (at_ble_cmd_complete_event_t *)param;
	if(!notification_status->status)
	{
		flag = true;
		DBG_LOG_DEV("sending notification to the peer success");				
	}
	return AT_BLE_SUCCESS;
}

/* Callback registered for AT_BLE_CHARACTERISTIC_CHANGED event from stack */
static at_ble_status_t ble_char_changed_app_event(void *param)
{
	uint16_t device_listening;
	at_ble_characteristic_changed_t *char_handle = (at_ble_characteristic_changed_t *)param;

	if(bas_service_handler.serv_chars.client_config_handle == char_handle->char_handle)
	{
		device_listening = char_handle->char_new_value[1]<<8| char_handle->char_new_value[0];
		if(!device_listening)
		{		
			aon_sleep_timer_service_stop();			
		}
		else
		{
			aon_sleep_timer_service_init(1);
			aon_sleep_timer_service_start(aon_sleep_timer_callback);
		}			
	}	
	return bat_char_changed_event(char_handle->conn_handle,&bas_service_handler, char_handle, &flag);	
}

static const ble_event_callback_t battery_app_gap_cb[] = {
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	ble_connected_app_event,
	ble_disconnected_app_event,
	NULL,
	NULL,
	ble_paired_app_event,
	NULL,
	NULL,
	NULL,
	NULL,
	ble_paired_app_event,
	NULL,
	NULL,
	NULL,
	NULL
};

static const ble_event_callback_t battery_app_gatt_server_cb[] = {
	ble_notification_confirmed_app_event,
	NULL,
	ble_char_changed_app_event,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

void resume_cb(void)
{
	init_port_list();
	//uart_init(UART_HW_MODULE_UART1,&uart_cfg);
	serial_console_init();
}

/**
* \Battery Service Application main function
*/
int main(void)
{
	at_ble_status_t status;
	uint8_t battery_level = BATTERY_MIN_LEVEL;	
	
	platform_driver_init();
	acquire_sleep_lock();

	/* Initialize the button */
	/* button_init(); */
	
	/* Initialize serial console */
	serial_console_init();

	DBG_LOG("Initializing Battery Service Application");
	
	/* initialize the ble chip  and Set the device mac address */
	ble_device_init(NULL);
	
	/* Initialize the battery service */
	bat_init_service(&bas_service_handler, &battery_level);
	
	/* Define the primary service in the GATT server database */
	if((status = bat_primary_service_define(&bas_service_handler))!= AT_BLE_SUCCESS)
	{
		DBG_LOG("defining battery service failed %d", status);
	}
	
	battery_service_advertise();
	
	/* Register callbacks for gap related events */ 
	ble_mgr_events_callback_handler(REGISTER_CALL_BACK,
									BLE_GAP_EVENT_TYPE,
									battery_app_gap_cb);
									
	/* Register callbacks for gatt server related events */
	ble_mgr_events_callback_handler(REGISTER_CALL_BACK,
									BLE_GATT_SERVER_EVENT_TYPE,
									battery_app_gatt_server_cb);
									
	register_resume_callback(resume_cb);
	
	release_sleep_lock();
	/* Capturing the events  */ 
	while (1) {
		/* BLE Event Task */
		ble_event_task(BLE_EVENT_TIMEOUT);
		acquire_sleep_lock();
		if (timer_cb_done)
		{
			timer_cb_done = false;			
			/* send the notification and Update the battery level  */			
			if(flag){
				if(bat_update_char_value(bat_connection_handle,&bas_service_handler, battery_level, &flag) == AT_BLE_SUCCESS)
				{
					DBG_LOG("Battery Level:%d%%", battery_level);
				}
				if(battery_level == BATTERY_MAX_LEVEL)
				{
					battery_flag = false;
				}
				else if(battery_level == BATTERY_MIN_LEVEL)
				{
					battery_flag = true;
				}
				if(battery_flag)
				{
					battery_level++;
				}
				else
				{
					battery_level--;
				}
			}
		}
		release_sleep_lock();
	}	
}


