/* Waveshare RP2350-PiZero: the microSD is on SPI1 with SCK=GPIO30,
 * MOSI=GPIO31, MISO=GPIO40 and CS=GPIO43 (per the board schematic; CS is
 * driven in software rather than by the SPI block). GPIO 40/43 only exist on
 * the RP2350B, which matches PICO_RP2350A 0 in the SDK's board header. */
#include "hw_config.h"

static spi_t spi = {
    .hw_inst   = spi1,
    .sck_gpio  = 30,
    .mosi_gpio = 31,
    .miso_gpio = 40,
    .baud_rate = 12 * 1000 * 1000   /* conservative to start; raise once it mounts */
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
