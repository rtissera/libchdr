/* Waveshare RP2350-PiZero: the microSD is on SPI1 with SCK=GPIO30,
 * MOSI=GPIO31, MISO=GPIO40 and CS=GPIO43 (per the board schematic; CS is
 * driven in software rather than by the SPI block). GPIO 40/43 only exist on
 * the RP2350B, which matches PICO_RP2350A 0 in the SDK's board header. */
#include "hw_config.h"

/* 25 MHz is the SD Default Speed rate, and SPI mode carries no open-drain
 * derating, so this is in spec rather than an overclock. Measured on this
 * board against 12 MHz over three I/O-heavy files: 1.219x aggregate
 * (kinst2 1.378x, Insanity 1.261x, Shadowrun 1.169x) with byte-identical
 * decoded output, and io share falling roughly in proportion to the clock.
 * An MCU SPI block plus board routing will not always meet timing here, so
 * lower it if a card misbehaves - corruption would show up as a hash
 * mismatch in the benchmark rather than as an error. */
#ifndef SD_BAUD_HZ
#define SD_BAUD_HZ (25 * 1000 * 1000)
#endif

static spi_t spi = {
    .hw_inst   = spi1,
    .sck_gpio  = 30,
    .mosi_gpio = 31,
    .miso_gpio = 40,
    /* SD Default Speed is 25 MHz and SPI mode has no open-drain derating, but
     * an MCU SPI block plus board routing often will not meet timing there;
     * ~20 MHz is the usual practical ceiling. Override to measure. */
    .baud_rate = SD_BAUD_HZ
};

static sd_spi_if_t spi_if = {
    .spi    = &spi,
    .ss_gpio = 43
};

static sd_card_t sd_card = {
    .type     = SD_IF_SPI,
    .spi_if_p = &spi_if
};

size_t sd_get_num(void) { return 1; }

sd_card_t *sd_get_by_num(size_t num)
{
    return (0 == num) ? &sd_card : NULL;
}
