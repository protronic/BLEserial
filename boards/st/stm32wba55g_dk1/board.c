/*
 * Copyright (c) 2025 STMicroelectronics
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Apply the HSE trimming (load capacitance) stored in the OTP area,
 * required for correct RF operation. Same scheme as on the ST WBA
 * reference boards (see boards/st/nucleo_wba55cg in Zephyr).
 */

#include <zephyr/init.h>
#include <stm32_ll_rcc.h>
#include <stddef.h>
#include <errno.h>

typedef __PACKED_STRUCT
{
	uint8_t additional_data[8];	/*!< 64 bits of data to fill OTP slot */
	uint8_t bd_address[6];		/*!< Bluetooth Device Address */
	uint8_t hsetune;		/*!< Load capacitance applied on HSE pad */
	uint8_t index;			/*!< Structure index */
} OTP_Data_s;

#define DEFAULT_OTP_IDX     0

static HAL_StatusTypeDef OTP_Read(uint8_t index, OTP_Data_s **otp_ptr);

void board_late_init_hook(void)
{
	HAL_StatusTypeDef status;
	OTP_Data_s *otp_ptr = NULL;

	status = OTP_Read(DEFAULT_OTP_IDX, &otp_ptr);
	if (status != HAL_OK) {
		/* OTP not present in flash, apply default trim */
		LL_RCC_HSE_SetClockTrimming(0x0C);
	} else {
		LL_RCC_HSE_SetClockTrimming(otp_ptr->hsetune);
	}
}

static HAL_StatusTypeDef OTP_Read(uint8_t index, OTP_Data_s **otp_ptr)
{
	*otp_ptr = (OTP_Data_s *) (FLASH_OTP_BASE + FLASH_OTP_SIZE - 16);

	while (((*otp_ptr)->index != index) && ((*otp_ptr) != (OTP_Data_s *) FLASH_OTP_BASE)) {
		(*otp_ptr) -= 1;
	}

	if ((*otp_ptr)->index != index) {
		return HAL_ERROR;
	}

	return HAL_OK;
}
