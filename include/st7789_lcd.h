#ifndef ST7789_LCD_H
#define ST7789_LCD_H

#include "stm32f4xx_hal.h"

extern "C"
{
#include "FreeRTOS.h"
#include "semphr.h"
}

enum class LcdDiagnosticState : uint8_t
{
    NotRun = 0,
    FsmcInitFailed = 1,
    IdAllZero = 2,
    IdAllOnes = 3,
    IdNonconstant = 4,
    IdOtherConstant = 5,
};

// Inspect these fields in a debugger after St7789Lcd::initialize().
extern volatile uint8_t lcd_fsmc_init_status;
extern volatile uint8_t lcd_id_dummy;
extern volatile uint8_t lcd_id_byte0;
extern volatile uint8_t lcd_id_byte1;
extern volatile uint8_t lcd_id_byte2;
extern volatile LcdDiagnosticState lcd_diagnostic_state;

class St7789Lcd
{
public:
    static constexpr uint16_t Width = 240;
    static constexpr uint16_t Height = 240;

    St7789Lcd();

    bool initialize();
    void attachMutex(SemaphoreHandle_t mutex);
    bool showTestScreen();
    bool showSolidColor(uint16_t color);
    bool clear(uint16_t color);
    bool fillRect(uint16_t x, uint16_t y, uint16_t width,
                  uint16_t height, uint16_t color);
    bool drawText(uint16_t x, uint16_t y, const char *text,
                  uint16_t color, uint16_t background);
    bool drawTextScaled(uint16_t x, uint16_t y, const char *text,
                        uint16_t color, uint16_t background,
                        uint8_t scale);

private:
    // The official Spark LCD driver places the 8-bit register write at the
    // upper byte lane immediately below the A18-selected data window.
    static constexpr uint32_t CommandAddress = 0x6803FFFEUL;
    static constexpr uint32_t DataAddress = 0x68040000UL;

    bool lock();
    void unlock();
    void writeCommand(uint8_t command);
    void writeData(uint8_t data);
    void writeData16(uint16_t data);
    void setAddressWindow(uint16_t x0, uint16_t y0,
                           uint16_t x1, uint16_t y1);
    void clearLocked(uint16_t color);
    void fillRectLocked(uint16_t x, uint16_t y, uint16_t width,
                        uint16_t height, uint16_t color);
    void drawTextLocked(uint16_t x, uint16_t y, const char *text,
                        uint16_t color, uint16_t background);
    void drawTextScaledLocked(uint16_t x, uint16_t y, const char *text,
                              uint16_t color, uint16_t background,
                              uint8_t scale);
    void drawGlyph(uint16_t x, uint16_t y, char character,
                   uint16_t color, uint16_t background);

    SemaphoreHandle_t mutex_;
};

#endif
