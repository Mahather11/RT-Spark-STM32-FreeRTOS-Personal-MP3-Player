#include "st7789_lcd.h"

#include <stddef.h>

extern "C" void boot_diag_stage(uint8_t stage);

namespace
{
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kBlue = 0x001F;
constexpr uint16_t kResetPin = GPIO_PIN_3;
constexpr uint16_t kBacklightPin = GPIO_PIN_9;

constexpr uint8_t kFont[39][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // !
    {0x00, 0x00, 0x00, 0x00, 0x00}, // unsupported punctuation
};

void configureFsmcGpio(GPIO_TypeDef *port, uint16_t pins)
{
    GPIO_InitTypeDef gpio = {};
    gpio.Pin = pins;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_FSMC;
    HAL_GPIO_Init(port, &gpio);
}
}

volatile uint8_t lcd_fsmc_init_status = 0;
volatile uint8_t lcd_id_dummy = 0;
volatile uint8_t lcd_id_byte0 = 0;
volatile uint8_t lcd_id_byte1 = 0;
volatile uint8_t lcd_id_byte2 = 0;
volatile LcdDiagnosticState lcd_diagnostic_state =
    LcdDiagnosticState::NotRun;

St7789Lcd::St7789Lcd() : mutex_(nullptr)
{
}

void St7789Lcd::attachMutex(SemaphoreHandle_t mutex)
{
    mutex_ = mutex;
}

bool St7789Lcd::initialize()
{
    __HAL_RCC_FSMC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    configureFsmcGpio(GPIOE, GPIO_PIN_7 | GPIO_PIN_8 |
                              GPIO_PIN_9 | GPIO_PIN_10);
    configureFsmcGpio(GPIOG, GPIO_PIN_10);
    configureFsmcGpio(GPIOD, GPIO_PIN_0 | GPIO_PIN_1 |
                              GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_13 |
                              GPIO_PIN_14 | GPIO_PIN_15);

    GPIO_InitTypeDef control = {};
    control.Pin = kResetPin;
    control.Mode = GPIO_MODE_OUTPUT_PP;
    control.Pull = GPIO_NOPULL;
    control.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOD, &control);

    control.Pin = kBacklightPin;
    HAL_GPIO_Init(GPIOF, &control);
    // PF9 drives Q3, whose collector sinks LED_K. High enables the backlight.
    HAL_GPIO_WritePin(GPIOF, kBacklightPin, GPIO_PIN_SET);

    HAL_GPIO_WritePin(GPIOD, kResetPin, GPIO_PIN_RESET);
    HAL_Delay(100);
    HAL_GPIO_WritePin(GPIOD, kResetPin, GPIO_PIN_SET);
    HAL_Delay(100);
    boot_diag_stage(5);

    SRAM_HandleTypeDef sram = {};
    sram.Instance = FSMC_NORSRAM_DEVICE;
    sram.Extended = FSMC_NORSRAM_EXTENDED_DEVICE;
    sram.Init.NSBank = FSMC_NORSRAM_BANK3;
    sram.Init.DataAddressMux = FSMC_DATA_ADDRESS_MUX_DISABLE;
    sram.Init.MemoryType = FSMC_MEMORY_TYPE_SRAM;
    sram.Init.MemoryDataWidth = FSMC_NORSRAM_MEM_BUS_WIDTH_8;
    sram.Init.BurstAccessMode = FSMC_BURST_ACCESS_MODE_DISABLE;
    sram.Init.WaitSignalPolarity = FSMC_WAIT_SIGNAL_POLARITY_LOW;
    sram.Init.WrapMode = FSMC_WRAP_MODE_DISABLE;
    sram.Init.WaitSignalActive = FSMC_WAIT_TIMING_BEFORE_WS;
    sram.Init.WriteOperation = FSMC_WRITE_OPERATION_ENABLE;
    sram.Init.WaitSignal = FSMC_WAIT_SIGNAL_DISABLE;
    sram.Init.ExtendedMode = FSMC_EXTENDED_MODE_ENABLE;
    sram.Init.AsynchronousWait = FSMC_ASYNCHRONOUS_WAIT_DISABLE;
    sram.Init.WriteBurst = FSMC_WRITE_BURST_DISABLE;
    sram.Init.ContinuousClock = FSMC_CONTINUOUS_CLOCK_SYNC_ONLY;
    sram.Init.WriteFifo = 0;
    sram.Init.PageSize = FSMC_PAGE_SIZE_NONE;

    FMC_NORSRAM_TimingTypeDef read_timing = {};
    read_timing.AddressSetupTime = 15;
    read_timing.AddressHoldTime = 0;
    read_timing.DataSetupTime = 60;
    read_timing.BusTurnAroundDuration = 0;
    read_timing.CLKDivision = 0;
    read_timing.DataLatency = 0;
    read_timing.AccessMode = FSMC_ACCESS_MODE_A;

    FMC_NORSRAM_TimingTypeDef write_timing = {};
    write_timing.AddressSetupTime = 9;
    write_timing.AddressHoldTime = 0;
    write_timing.DataSetupTime = 8;
    write_timing.BusTurnAroundDuration = 0;
    write_timing.CLKDivision = 0;
    write_timing.DataLatency = 0;
    write_timing.AccessMode = FSMC_ACCESS_MODE_A;

    const HAL_StatusTypeDef fsmc_status =
        HAL_SRAM_Init(&sram, &read_timing, &write_timing);
    lcd_fsmc_init_status = static_cast<uint8_t>(fsmc_status);
    boot_diag_stage(4);
    if (fsmc_status != HAL_OK)
    {
        lcd_diagnostic_state = LcdDiagnosticState::FsmcInitFailed;
        return false;
    }

    // ST7789 read-ID transaction: discard the first byte, then capture the
    // three controller-ID bytes through the byte-wide FSMC data window.
    writeCommand(0x04);
    volatile uint8_t *const data_bus =
        reinterpret_cast<volatile uint8_t *>(DataAddress);
    lcd_id_dummy = *data_bus;
    lcd_id_byte0 = *data_bus;
    lcd_id_byte1 = *data_bus;
    lcd_id_byte2 = *data_bus;

    const bool all_zero =
        lcd_id_byte0 == 0x00 &&
        lcd_id_byte1 == 0x00 &&
        lcd_id_byte2 == 0x00;
    const bool all_ones =
        lcd_id_byte0 == 0xFF &&
        lcd_id_byte1 == 0xFF &&
        lcd_id_byte2 == 0xFF;
    if (all_zero)
    {
        lcd_diagnostic_state = LcdDiagnosticState::IdAllZero;
    }
    else if (all_ones)
    {
        lcd_diagnostic_state = LcdDiagnosticState::IdAllOnes;
    }
    else if (lcd_id_byte0 != lcd_id_byte1 ||
             lcd_id_byte1 != lcd_id_byte2)
    {
        lcd_diagnostic_state = LcdDiagnosticState::IdNonconstant;
    }
    else
    {
        lcd_diagnostic_state = LcdDiagnosticState::IdOtherConstant;
    }
    boot_diag_stage(6);

    HAL_Delay(100);

    writeCommand(0x3A); // COLMOD: RGB565
    writeData(0x65);
    writeCommand(0x36); // MADCTL: portrait, RGB order
    writeData(0x00);
    writeCommand(0xB2); // porch control
    writeData(0x0C);
    writeData(0x0C);
    writeData(0x00);
    writeData(0x33);
    writeData(0x33);
    writeCommand(0xB7); // gate control
    writeData(0x35);
    writeCommand(0xBB); // VCOM setting
    writeData(0x37);
    writeCommand(0xC0); // LCM control
    writeData(0x2C);
    writeCommand(0xC2); // VDV and VRH enable
    writeData(0x01);
    writeCommand(0xC3); // VRH set
    writeData(0x12);
    writeCommand(0xC4); // VDV set
    writeData(0x20);
    writeCommand(0xC6); // frame rate control
    writeData(0x0F);
    writeCommand(0xD0); // power control
    writeData(0xA4);
    writeData(0xA1);
    writeCommand(0xE0); // positive voltage gamma
    writeData(0xD0);
    writeData(0x04);
    writeData(0x0D);
    writeData(0x11);
    writeData(0x13);
    writeData(0x2B);
    writeData(0x3F);
    writeData(0x54);
    writeData(0x4C);
    writeData(0x18);
    writeData(0x0D);
    writeData(0x0B);
    writeData(0x1F);
    writeData(0x23);
    writeCommand(0xE1); // negative voltage gamma
    writeData(0xD0);
    writeData(0x04);
    writeData(0x0C);
    writeData(0x11);
    writeData(0x13);
    writeData(0x2C);
    writeData(0x3F);
    writeData(0x44);
    writeData(0x51);
    writeData(0x2F);
    writeData(0x1F);
    writeData(0x1F);
    writeData(0x20);
    writeData(0x23);
    writeCommand(0x21); // display inversion on
    writeCommand(0x13); // normal display mode
    writeCommand(0x11); // sleep out
    HAL_Delay(120);
    writeCommand(0x29); // display on
    HAL_Delay(20);

    return true;
}

bool St7789Lcd::lock()
{
    return mutex_ != nullptr &&
           xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
}

void St7789Lcd::unlock()
{
    xSemaphoreGive(mutex_);
}

void St7789Lcd::writeCommand(uint8_t command)
{
    *reinterpret_cast<volatile uint8_t *>(CommandAddress) = command;
}

void St7789Lcd::writeData(uint8_t data)
{
    *reinterpret_cast<volatile uint8_t *>(DataAddress) = data;
}

void St7789Lcd::writeData16(uint16_t data)
{
    writeData(static_cast<uint8_t>(data >> 8));
    writeData(static_cast<uint8_t>(data));
}

void St7789Lcd::setAddressWindow(uint16_t x0, uint16_t y0,
                                  uint16_t x1, uint16_t y1)
{
    writeCommand(0x2A);
    writeData(static_cast<uint8_t>(x0 >> 8));
    writeData(static_cast<uint8_t>(x0));
    writeData(static_cast<uint8_t>(x1 >> 8));
    writeData(static_cast<uint8_t>(x1));
    writeCommand(0x2B);
    writeData(static_cast<uint8_t>(y0 >> 8));
    writeData(static_cast<uint8_t>(y0));
    writeData(static_cast<uint8_t>(y1 >> 8));
    writeData(static_cast<uint8_t>(y1));
    writeCommand(0x2C);
}

void St7789Lcd::clearLocked(uint16_t color)
{
    fillRectLocked(0, 0, Width, Height, color);
}

void St7789Lcd::fillRectLocked(uint16_t x, uint16_t y, uint16_t width,
                               uint16_t height, uint16_t color)
{
    if (x >= Width || y >= Height || width == 0 || height == 0)
    {
        return;
    }
    const uint16_t x1 = static_cast<uint16_t>(
        (x + width > Width) ? Width - 1 : x + width - 1);
    const uint16_t y1 = static_cast<uint16_t>(
        (y + height > Height) ? Height - 1 : y + height - 1);
    setAddressWindow(x, y, x1, y1);
    const uint32_t pixels =
        static_cast<uint32_t>(x1 - x + 1) * (y1 - y + 1);
    for (uint32_t i = 0; i < pixels; ++i)
    {
        writeData16(color);
    }
}

bool St7789Lcd::clear(uint16_t color)
{
    if (!lock())
    {
        return false;
    }
    clearLocked(color);
    unlock();
    return true;
}

bool St7789Lcd::fillRect(uint16_t x, uint16_t y, uint16_t width,
                         uint16_t height, uint16_t color)
{
    if (!lock())
    {
        return false;
    }
    fillRectLocked(x, y, width, height, color);
    unlock();
    return true;
}

bool St7789Lcd::showSolidColor(uint16_t color)
{
    if (!lock())
    {
        return false;
    }
    clearLocked(color);
    unlock();
    return true;
}

void St7789Lcd::drawGlyph(uint16_t x, uint16_t y, char character,
                          uint16_t color, uint16_t background)
{
    uint8_t glyphIndex = 0;
    if (character >= '0' && character <= '9')
    {
        glyphIndex = static_cast<uint8_t>(character - '0' + 1);
    }
    else if (character >= 'A' && character <= 'Z')
    {
        glyphIndex = static_cast<uint8_t>(character - 'A' + 11);
    }
    else if (character == '!')
    {
        glyphIndex = 37;
    }

    setAddressWindow(x, y, x + 5, y + 7);
    for (uint8_t row = 0; row < 8; ++row)
    {
        for (uint8_t column = 0; column < 6; ++column)
        {
            const bool pixel = column < 5 &&
                ((kFont[glyphIndex][column] >> row) & 0x01U) != 0;
            writeData16(pixel ? color : background);
        }
    }
}

void St7789Lcd::drawTextLocked(uint16_t x, uint16_t y, const char *text,
                               uint16_t color, uint16_t background)
{
    if (text == nullptr)
    {
        return;
    }
    while (*text != '\0' && x + 6 <= Width)
    {
        char character = *text++;
        if (character >= 'a' && character <= 'z')
        {
            character = static_cast<char>(character - 'a' + 'A');
        }
        drawGlyph(x, y, character, color, background);
        x = static_cast<uint16_t>(x + 6);
    }
}

void St7789Lcd::drawTextScaledLocked(
    uint16_t x, uint16_t y, const char *text, uint16_t color,
    uint16_t background, uint8_t scale)
{
    if (text == nullptr || scale == 0)
    {
        return;
    }
    while (*text != '\0' &&
           x + static_cast<uint16_t>(6U * scale) <= Width)
    {
        char character = *text++;
        if (character >= 'a' && character <= 'z')
        {
            character = static_cast<char>(character - 'a' + 'A');
        }
        uint8_t glyphIndex = 0;
        if (character >= '0' && character <= '9')
        {
            glyphIndex = static_cast<uint8_t>(character - '0' + 1);
        }
        else if (character >= 'A' && character <= 'Z')
        {
            glyphIndex = static_cast<uint8_t>(character - 'A' + 11);
        }
        else if (character == '!')
        {
            glyphIndex = 37;
        }
        const uint16_t glyphWidth = static_cast<uint16_t>(6U * scale);
        const uint16_t glyphHeight = static_cast<uint16_t>(8U * scale);
        setAddressWindow(
            x, y, static_cast<uint16_t>(x + glyphWidth - 1),
            static_cast<uint16_t>(y + glyphHeight - 1));
        for (uint8_t row = 0; row < 8; ++row)
        {
            for (uint8_t vertical = 0; vertical < scale; ++vertical)
            {
                for (uint8_t column = 0; column < 6; ++column)
                {
                    const bool pixel = column < 5 &&
                        ((kFont[glyphIndex][column] >> row) & 0x01U) != 0;
                    for (uint8_t horizontal = 0; horizontal < scale;
                         ++horizontal)
                    {
                        writeData16(pixel ? color : background);
                    }
                }
            }
        }
        x = static_cast<uint16_t>(x + glyphWidth);
    }
}

bool St7789Lcd::drawText(uint16_t x, uint16_t y, const char *text,
                         uint16_t color, uint16_t background)
{
    if (!lock())
    {
        return false;
    }
    drawTextLocked(x, y, text, color, background);
    unlock();
    return true;
}

bool St7789Lcd::drawTextScaled(uint16_t x, uint16_t y, const char *text,
                               uint16_t color, uint16_t background,
                               uint8_t scale)
{
    if (!lock())
    {
        return false;
    }
    drawTextScaledLocked(x, y, text, color, background, scale);
    unlock();
    return true;
}

bool St7789Lcd::showTestScreen()
{
    if (!lock())
    {
        return false;
    }
    clearLocked(kRed);
    unlock();
    return true;
}
