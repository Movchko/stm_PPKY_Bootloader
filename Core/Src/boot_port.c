#include "main.h"
#include "boot.h"
#include "spif.h"

extern SPIF_HandleTypeDef hFlash;

static uint32_t ImageBase(FwImage type_fw)
{
	switch (type_fw) {
		case MAIN: return GetMainCodeAdr();
		case UPDATE: return GetUpdateCodeAdr();
		case DEFAULT: return GetDefaultCodeAdr();
		default: return 0u;
	}
}

void BootLedToggle(void)
{
	HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
	HAL_GPIO_TogglePin(LED_APP_GPIO_Port, LED_APP_Pin);
}

uint32_t GetMainCodeAdr(void)
{
	return MAIN_FOOTER_ADDR;
}

uint32_t GetUpdateCodeAdr(void)
{
	return UPDATE_APP_ADDR;
}

uint32_t GetDefaultCodeAdr(void)
{
	return DEFAULT_APP_ADDR;
}

static void FlashOpsBegin(void)
{
	(void)HAL_ICACHE_Disable();
	HAL_FLASH_Unlock();
}

static void FlashOpsEnd(void)
{
	HAL_FLASH_Lock();
	(void)HAL_ICACHE_Invalidate();
	(void)HAL_ICACHE_Enable();
}

uint32_t ReadBootWatchDog(void)
{
	return *(volatile uint32_t *)BOOT_WD_QWORD_ADDR;
}

void WriteBootWatchDog(void)
{
	uint32_t quad_word[4] = { WATCHDOG, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
	if (ReadBootWatchDog() == WATCHDOG) {
		return;
	}
	FlashOpsBegin();
	(void)HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD, BOOT_WD_QWORD_ADDR, (uint32_t)quad_word);
	FlashOpsEnd();
}

uint32_t ReadProgramWatchDog(void)
{
	return *(volatile uint32_t *)BOOT_PROGRAM_WD_ADDR;
}

void WriteProgramWatchDog(void)
{
	uint32_t quad_word[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, WATCHDOG };

	if (ReadProgramWatchDog() == WATCHDOG) {
		return;
	}
	FlashOpsBegin();
	(void)HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD, BOOT_PROGRAM_WD_QWORD_ADDR, (uint32_t)quad_word);
	FlashOpsEnd();
}

void EraseCfg(void)
{

	//SPIF_EraseChip(&hFlash);


	uint32_t i;
	for (i = 0; i < FLASH_CFG_ERASE_BLOCKS; i++) {
		(void)SPIF_EraseBlock(&hFlash, i);
		BootLedToggle();
	}

}

static uint8_t EraseSpiSlot(uint32_t start_block)
{
	uint32_t i;
	for (i = 0; i < FLASH_FW_SLOT_BLOCKS; i++) {
		if (!SPIF_EraseBlock(&hFlash, start_block + i)) {
			return 1u;
		}
		BootLedToggle();
	}
	return 0u;
}

uint8_t EraseApp(FwImage type_fw)
{
	FLASH_EraseInitTypeDef erase = {0};
	uint32_t page_error = 0;

	if (type_fw == MAIN) {
		FlashOpsBegin();
		erase.TypeErase = FLASH_TYPEERASE_SECTORS;
		erase.Banks = FLASH_BANK_1;
		erase.Sector = BOOT_APP_BANK1_SECTOR;
		erase.NbSectors = BOOT_APP_BANK1_NB_SECTORS;
		if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) {
			FlashOpsEnd();
			return 1u;
		}
		erase.Banks = FLASH_BANK_2;
		erase.Sector = BOOT_APP_BANK2_SECTOR;
		erase.NbSectors = BOOT_APP_BANK2_NB_SECTORS;
		if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) {
			FlashOpsEnd();
			return 1u;
		}
		FlashOpsEnd();
		return 0u;
	}

	if (type_fw == UPDATE) {
		return EraseSpiSlot(FLASH_FW_UPDATE_BLOCK);
	}
	if (type_fw == DEFAULT) {
		return EraseSpiSlot(FLASH_FW_FACTORY_BLOCK);
	}
	return 1u;
}

uint8_t SetApp(FwImage dst_type_fw, FwImage src_type_fw)
{
	uint32_t quad_word[4];
	uint32_t total_words;
	uint32_t i;

	if (EraseApp(dst_type_fw) != 0u) {
		return 1u;
	}

	total_words = (GetAppSize(src_type_fw) + 3u) / 4u;
	for (i = 0; i < total_words; i += 4u) {
		uint32_t j;
		quad_word[0] = 0xFFFFFFFFu;
		quad_word[1] = 0xFFFFFFFFu;
		quad_word[2] = 0xFFFFFFFFu;
		quad_word[3] = 0xFFFFFFFFu;
		for (j = 0; j < 4u; j++) {
			if ((i + j) < total_words) {
				quad_word[j] = GetWord(src_type_fw, (int32_t)(i + j));
			}
		}
		if (SetWord(dst_type_fw, (int32_t)(i / 4u), &quad_word[0], 4u) != 0u) {
			return 1u;
		}
		if ((i & 0xFFu) == 0u) {
			BootLedToggle();
		}
	}
	return 0u;
}

void ResetMCU(void)
{
	NVIC_SystemReset();
}

void BootReadBytes(FwImage type_fw, uint32_t byte_offset, uint8_t *dst, uint32_t len)
{
	uint32_t addr = ImageBase(type_fw) + byte_offset;
	if ((dst == NULL) || (len == 0u)) {
		return;
	}
	if (type_fw == MAIN) {
		memcpy(dst, (const void *)addr, len);
	} else {
		(void)SPIF_ReadAddress(&hFlash, addr, dst, len);
	}
}

uint32_t GetWord(FwImage type_fw, int32_t offset)
{
	uint32_t word = 0;
	uint32_t addr = ImageBase(type_fw);
	if (offset < 0) {
		return 0u;
	}
	addr += (uint32_t)offset * 4u;
	if (type_fw == MAIN) {
		word = *(volatile uint32_t *)addr;
	} else {
		(void)SPIF_ReadAddress(&hFlash, addr, (uint8_t *)&word, sizeof(word));
	}
	return word;
}

uint8_t GetByte(FwImage type_fw, int32_t offset)
{
	uint8_t byte = 0;
	uint32_t addr = ImageBase(type_fw);
	if (offset < 0) {
		return 0u;
	}
	addr += (uint32_t)offset;
	if (type_fw == MAIN) {
		byte = *(volatile uint8_t *)addr;
	} else {
		(void)SPIF_ReadAddress(&hFlash, addr, &byte, 1u);
	}
	return byte;
}

uint8_t SetWord(FwImage type_fw, int32_t offset, uint32_t *word, uint8_t num_word)
{
	uint32_t addr = ImageBase(type_fw);
	uint32_t flash_addr;

	if ((offset < 0) || (word == NULL) || (num_word != 4u)) {
		return 1u;
	}

	flash_addr = addr + ((uint32_t)offset * 16u);
	if ((flash_addr & 0xFu) != 0u) {
		return 1u;
	}

	if (type_fw == MAIN) {
		FlashOpsBegin();
		if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD, flash_addr, (uint32_t)word) != HAL_OK) {
			FlashOpsEnd();
			return 1u;
		}
		FlashOpsEnd();
		return 0u;
	}

	if (!SPIF_WriteAddress(&hFlash, flash_addr, (uint8_t *)word, 16u)) {
		return 1u;
	}
	return 0u;
}

typedef void (*pFunction)(void);

void CallApplication(void)
{
	uint32_t appStack;
	pFunction appEntry;

	__disable_irq();
	SysTick->CTRL = 0;
	(void)HAL_ICACHE_Disable();
	(void)HAL_ICACHE_Invalidate();
	HAL_MPU_Disable();
	HAL_RCC_DeInit();
	HAL_DeInit();
	__set_BASEPRI(0);
	__set_CONTROL(0);

	for (uint8_t i = 0; i < (sizeof(NVIC->ICER) / 4); i++) {
		NVIC->ICER[i] = 0xFFFFFFFFu;
		NVIC->ICPR[i] = 0xFFFFFFFFu;
	}

	appStack = *(volatile uint32_t *)MAIN_APP_START_ADDR;
	appEntry = (pFunction)(*(volatile uint32_t *)(MAIN_APP_START_ADDR + 4u));

	SCB->VTOR = MAIN_APP_START_ADDR;
	__DSB();
	__ISB();
	__set_MSP(appStack);
	__enable_irq();
	appEntry();
}
