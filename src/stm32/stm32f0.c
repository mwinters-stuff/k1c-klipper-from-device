// Code to setup clocks and gpio on stm32f0
//
// Copyright (C) 2019  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_CLOCK_REF_8M
#include "board/armcm_boot.h" // armcm_main
#include "board/irq.h" // irq_disable
#include "command.h" // DECL_CONSTANT_STR
#include "internal.h" // enable_pclock
#include "sched.h" // sched_main

#define FREQ_PERIPH 48000000

// Enable a peripheral clock
void
enable_pclock(uint32_t periph_base)
{
    if (periph_base < SYSCFG_BASE) {
        uint32_t pos = (periph_base - APBPERIPH_BASE) / 0x400;
        RCC->APB1ENR |= 1 << pos;
        RCC->APB1ENR;
    } else if (periph_base < AHBPERIPH_BASE) {
        uint32_t pos = (periph_base - SYSCFG_BASE) / 0x400;
        RCC->APB2ENR |= 1 << pos;
        RCC->APB2ENR;
    } else {
        uint32_t pos = (periph_base - AHB2PERIPH_BASE) / 0x400;
        RCC->AHBENR |= 1 << (pos + 17);
        RCC->AHBENR;
    }
}

// Check if a peripheral clock has been enabled
int
is_enabled_pclock(uint32_t periph_base)
{
    if (periph_base < SYSCFG_BASE) {
        uint32_t pos = (periph_base - APBPERIPH_BASE) / 0x400;
        return RCC->APB1ENR & (1 << pos);
    } else if (periph_base < AHBPERIPH_BASE) {
        uint32_t pos = (periph_base - SYSCFG_BASE) / 0x400;
        return RCC->APB2ENR & (1 << pos);
    } else {
        uint32_t pos = (periph_base - AHB2PERIPH_BASE) / 0x400;
        return RCC->AHBENR & (1 << (pos + 17));
    }
}

// Return the frequency of the given peripheral clock
uint32_t
get_pclock_frequency(uint32_t periph_base)
{
    return FREQ_PERIPH;
}

// Enable a GPIO peripheral clock
void
gpio_clock_enable(GPIO_TypeDef *regs)
{
    uint32_t rcc_pos = ((uint32_t)regs - AHB2PERIPH_BASE) / 0x400;
    RCC->AHBENR |= 1 << (rcc_pos + 17);
    RCC->AHBENR;
}

// Set the mode and extended function of a pin
void
gpio_peripheral(uint32_t gpio, uint32_t mode, int pullup)
{
    GPIO_TypeDef *regs = digital_regs[GPIO2PORT(gpio)];

    // Enable GPIO clock
    gpio_clock_enable(regs);

    // Configure GPIO
    uint32_t mode_bits = mode & 0xf, func = (mode >> 4) & 0xf, od = mode >> 8;
    uint32_t pup = pullup ? (pullup > 0 ? 1 : 2) : 0;
    uint32_t pos = gpio % 16, af_reg = pos / 8;
    uint32_t af_shift = (pos % 8) * 4, af_msk = 0x0f << af_shift;
    uint32_t m_shift = pos * 2, m_msk = 0x03 << m_shift;

    regs->AFR[af_reg] = (regs->AFR[af_reg] & ~af_msk) | (func << af_shift);
    regs->MODER = (regs->MODER & ~m_msk) | (mode_bits << m_shift);
    regs->PUPDR = (regs->PUPDR & ~m_msk) | (pup << m_shift);
    regs->OTYPER = (regs->OTYPER & ~(1 << pos)) | (od << pos);
    regs->OSPEEDR = (regs->OSPEEDR & ~m_msk) | (0x02 << m_shift);
}

#define USB_BOOT_FLAG_ADDR (CONFIG_RAM_START + CONFIG_RAM_SIZE - 1024)
#define USB_BOOT_FLAG 0x55534220424f4f54 // "USB BOOT"

// Handle USB reboot requests
void
usb_request_bootloader(void)
{
    irq_disable();
    *(uint64_t*)USB_BOOT_FLAG_ADDR = USB_BOOT_FLAG;
    NVIC_SystemReset();
}

#if CONFIG_CLOCK_REF_8M
DECL_CONSTANT_STR("RESERVE_PINS_crystal", "PF0,PF1");
#endif

// Configure and enable the PLL as clock source
static void
pll_setup(void)
{
    uint32_t cfgr;
    if (CONFIG_CLOCK_REF_8M) {
        // Configure 48Mhz PLL from external 8Mhz crystal (HSE)
        RCC->CR |= RCC_CR_HSEON;
        cfgr = RCC_CFGR_PLLSRC_HSE_PREDIV | ((6 - 2) << RCC_CFGR_PLLMUL_Pos);
    } else {
        // Configure 48Mhz PLL from internal 8Mhz oscillator (HSI)
        cfgr = RCC_CFGR_PLLSRC_HSI_DIV2 | ((12 - 2) << RCC_CFGR_PLLMUL_Pos);
    }
    RCC->CFGR = cfgr;
    RCC->CR |= RCC_CR_PLLON;

    // Wait for PLL lock
    while (!(RCC->CR & RCC_CR_PLLRDY))
        ;

    // Switch system clock to PLL
    RCC->CFGR = cfgr | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS_Msk) != RCC_CFGR_SWS_PLL)
        ;

    // Select PLL as source for USB clock
    if (CONFIG_USBSERIAL)
        RCC->CFGR3 = RCC_CFGR3_USBSW;
}

// Configure and enable internal 48Mhz clock on the stm32f042
static void
hsi48_setup(void)
{
#if CONFIG_MACH_STM32F042
    // Enable HSI48
    RCC->CR2 |= RCC_CR2_HSI48ON;
    while (!(RCC->CR2 & RCC_CR2_HSI48RDY))
        ;

    // Switch system clock to HSI48
    RCC->CFGR = RCC_CFGR_SW_HSI48;
    while ((RCC->CFGR & RCC_CFGR_SWS_Msk) != RCC_CFGR_SWS_HSI48)
        ;

    // Enable USB clock recovery
    if (CONFIG_USBSERIAL) {
        enable_pclock(CRS_BASE);
        CRS->CR |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;
    }
#endif
}

// Main entry point - called from armcm_boot.c:ResetHandler()
void
armcm_main(void)
{
    if (CONFIG_USBSERIAL && CONFIG_MACH_STM32F042
        && *(uint64_t*)USB_BOOT_FLAG_ADDR == USB_BOOT_FLAG) {
        *(uint64_t*)USB_BOOT_FLAG_ADDR = 0;
        uint32_t *sysbase = (uint32_t*)0x1fffc400;
        asm volatile("mov sp, %0\n bx %1"
                     : : "r"(sysbase[0]), "r"(sysbase[1]));
    }

    SystemInit();

    // Set flash latency
    FLASH->ACR = (1 << FLASH_ACR_LATENCY_Pos) | FLASH_ACR_PRFTBE;

    // Configure main clock
    if (CONFIG_MACH_STM32F042 && CONFIG_STM32_CLOCK_REF_INTERNAL)
        hsi48_setup();
    else
        pll_setup();

    // Support alternate USB pins on stm32f042
#ifdef SYSCFG_CFGR1_PA11_PA12_RMP
    if (CONFIG_STM32F042_USB_PIN_SWAP) {
        enable_pclock(SYSCFG_BASE);
        SYSCFG->CFGR1 |= SYSCFG_CFGR1_PA11_PA12_RMP;
    }
#endif

    sched_main();
}