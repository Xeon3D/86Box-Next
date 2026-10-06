/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          The boards' displays as textures, from 88emuPlayer's
 *          ui/panel.cpp (the SC-55/SC-88 character panel, derived from
 *          Nuked-SC55, Copyright (C) 2021, 2024 nukeykt, GPLv2+) and
 *          ui/Emu88EditorLcd.h (the dot-matrix panels), without JUCE.
 */
#include "emu88_host.h"

#include "88lib/deviceModel.h"
#include "88lib/hardwareDevice.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

extern uint8_t lcd_font[240][10];

using emu88Lib::DeviceModel;
using Screen      = emu88Lib::HardwareDevice::DisplaySnapshot::Screen;
using ScreenType  = emu88Lib::HardwareDevice::DisplaySnapshot::Type;

namespace
{
    /* ---- The SC-55 / SC-88 character panel (panel.cpp) ---- */

    constexpr int kWidth  = 741;
    constexpr int kHeight = 268;

    struct TextField {
        int     ddRamStart;
        int     count;
        int32_t row;
        int32_t column;
    };

    constexpr TextField kTextFields[] = {
        { 0, 3, 11, 34 }, { 3, 16, 11, 153 },
        { 40, 3, 75, 34 }, { 43, 3, 75, 153 },
        { 49, 3, 139, 34 }, { 46, 3, 139, 153 },
        { 52, 3, 203, 34 }, { 55, 3, 203, 153 },
    };
    constexpr int kLrDdRam       = 58;
    constexpr int kLevelDdRam[2] = { 20, 60 };

    constexpr uint8_t kLrGlyph[2][12][11] = {
        {
            { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
            { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
            { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
            { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
            { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
            { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
        },
        {
            { 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0 }, { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 },
            { 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0 }, { 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0 },
            { 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0 }, { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 },
            { 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0 }, { 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0 },
            { 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0 }, { 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0 },
            { 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1 }, { 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1 },
        },
    };
    constexpr int32_t kLrPos[2][2] = { { 70, 264 }, { 232, 264 } };

    constexpr uint32_t kLcdGlass      = 0xffff6f0fu;
    constexpr uint32_t kLcdOffOverlay = 0x38000000u;
    constexpr uint32_t kLcdOn         = 0xff000000u;

    inline void putPixel(uint32_t *dst, const int32_t row, const int32_t col, const uint32_t c)
    {
        if (row < 0 || row >= kHeight || col < 0 || col >= kWidth)
            return;
        dst[static_cast<size_t>(row) * kWidth + col] = c;
    }

    const uint8_t *glyph(const uint8_t ch, const uint8_t *cgRam)
    {
        return ch >= 16 ? &lcd_font[ch - 16][0] : &cgRam[(ch & 7) * 8];
    }

    void drawChar(uint32_t *dst, const int32_t row, const int32_t col, const uint8_t ch, const uint8_t *cgRam, const uint32_t on, const uint32_t off)
    {
        const uint8_t *f = glyph(ch, cgRam);
        for (int i = 0; i < 7; ++i)
            for (int j = 0; j < 5; ++j) {
                const uint32_t c = (f[i] & (1u << (4 - j))) ? on : off;
                for (int ii = 0; ii < 5; ++ii)
                    for (int jj = 0; jj < 5; ++jj)
                        putPixel(dst, row + i * 6 + ii, col + j * 6 + jj, c);
            }
    }

    void drawLevel(uint32_t *dst, const int32_t row, const int32_t col, const uint8_t ch, const uint8_t *cgRam, const int width, const uint32_t on, const uint32_t off)
    {
        const uint8_t *f = glyph(ch, cgRam);
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < width; ++j) {
                const uint32_t c = (f[i] & (1u << (4 - j))) ? on : off;
                for (int ii = 0; ii < 9; ++ii)
                    for (int jj = 0; jj < 24; ++jj)
                        putPixel(dst, row + i * 11 + ii, col + j * 26 + jj, c);
            }
    }

    void drawCharacterPanel(uint32_t *dst, const Screen *s)
    {
        std::fill(dst, dst + static_cast<size_t>(kWidth) * kHeight, 0u);
        if (!s || s->type != ScreenType::Character || !s->displayOn)
            return;
        const uint8_t *dd = s->ddRam.data();
        const uint8_t *cg = s->cgRam.data();
        for (const auto &tf : kTextFields)
            for (int i = 0; i < tf.count; ++i)
                drawChar(dst, tf.row, tf.column + i * 35, dd[tf.ddRamStart + i], cg, kLcdOn, kLcdOffOverlay);

        const uint32_t lr = (glyph(dd[kLrDdRam], cg)[0] & 1) ? kLcdOn : kLcdOffOverlay;
        for (int g = 0; g < 2; ++g)
            for (int i = 0; i < 12; ++i)
                for (int j = 0; j < 11; ++j)
                    if (kLrGlyph[g][i][j])
                        putPixel(dst, i + kLrPos[g][0], j + kLrPos[g][1], lr);

        for (int ch = 0; ch < 2; ++ch)
            for (int seg = 0; seg < 4; ++seg)
                drawLevel(dst, 71 + ch * 88, 293 + seg * 130, dd[kLevelDdRam[ch] + seg], cg, seg == 3 ? 1 : 5, kLcdOn, kLcdOffOverlay);
    }

    /* ---- The dot-matrix panels (Emu88EditorLcd.h) ---- */

    struct GraphicStyle {
        uint32_t glass, off, on;
        int      pitch, dotSize, cellWidth, cellHeight;
        int      textureWidth, textureHeight, originX, originY;
        int      windowWidthDp, windowHeightDp;
    };

    constexpr GraphicStyle kSc8850Style { kLcdGlass, kLcdGlass, kLcdOn, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0 };
    constexpr GraphicStyle kCm32pStyle { 0xff51be03u, 0xff00b578u, 0xff000000u, 8, 6, 6, 9, 800, 150, 25, 11, 218, 41 };
    constexpr GraphicStyle kCm32lStyle { 0xff51be03u, 0xff00b578u, 0xff000000u, 8, 6, 6, 9, 1000, 92, 20, 10, 218, 20 };

    struct Geometry {
        const GraphicStyle *style;
        int                 width, height; /* dot grid; 0 = the character panel */
    };

    Geometry geometryFor(const DeviceModel model, const unsigned screen)
    {
        if (!emu88Lib::deviceHasLcd(model) || (screen && !emu88Lib::deviceHasSecondLcd(model)))
            return { nullptr, -1, -1 };
        if (model == DeviceModel::Cm64)
            return screen == 0 ? Geometry { &kCm32pStyle, 96, 18 } : Geometry { &kCm32lStyle, 120, 9 };
        if (model == DeviceModel::Cm32p)
            return { &kCm32pStyle, 96, 18 };
        if (emu88Lib::isLaModel(model))
            return { &kCm32lStyle, 120, 9 };
        if (model == DeviceModel::Sc8850)
            return { &kSc8850Style, 160, 64 };
        return { nullptr, 0, 0 };
    }

    void textureSize(const Geometry &g, int &w, int &h)
    {
        if (g.width < 0) {
            w = h = 0;
        } else if (!g.style) {
            w = kWidth;
            h = kHeight;
        } else {
            w = g.style->textureWidth ? g.style->textureWidth : g.width * g.style->pitch;
            h = g.style->textureHeight ? g.style->textureHeight : g.height * g.style->pitch;
        }
    }

    inline uint32_t over(const uint32_t dst, const float alpha) /* black at alpha over an opaque pixel */
    {
        const float k = 1.0f - alpha;
        const auto  r = static_cast<uint32_t>(((dst >> 16) & 0xff) * k + 0.5f);
        const auto  g = static_cast<uint32_t>(((dst >> 8) & 0xff) * k + 0.5f);
        const auto  b = static_cast<uint32_t>((dst & 0xff) * k + 0.5f);
        return 0xff000000u | (r << 16) | (g << 8) | b;
    }

    void drawGraphicPanel(uint32_t *dst, const Geometry &geo, const Screen *s)
    {
        const GraphicStyle &st = *geo.style;
        int                 tw, th;
        textureSize(geo, tw, th);
        std::fill(dst, dst + static_cast<size_t>(tw) * th, st.glass);

        const auto isDot = [&](const int x, const int y) {
            return st.cellWidth == 0 || (x % st.cellWidth < st.cellWidth - 1 && y % st.cellHeight < st.cellHeight - 1);
        };
        const auto fillDot = [&](const int x, const int y, const uint32_t c) {
            for (int yy = 0; yy < st.dotSize; ++yy)
                for (int xx = 0; xx < st.dotSize; ++xx) {
                    const int px = st.originX + x * st.pitch + xx;
                    const int py = st.originY + y * st.pitch + yy;
                    if (px >= 0 && px < tw && py >= 0 && py < th)
                        dst[static_cast<size_t>(py) * tw + px] = c;
                }
        };

        if (st.off != st.glass)
            for (int y = 0; y < geo.height; ++y)
                for (int x = 0; x < geo.width; ++x)
                    if (isDot(x, y))
                        fillDot(x, y, st.off);

        if (s && s->type == ScreenType::Graphic && s->displayOn && s->width == geo.width && s->height == geo.height && s->mono.size() == static_cast<size_t>(s->width) * s->height)
            for (int y = 0; y < geo.height; ++y)
                for (int x = 0; x < geo.width; ++x)
                    if (s->mono[static_cast<size_t>(y) * geo.width + x])
                        fillDot(x, y, st.on);

        if (!st.windowWidthDp)
            return;
        /* The CM windows' two inner shadows: black at 30% offset (+2, +2) dp and at 15% by
           (-2, -2) dp, each blurred by a 1 dp Gaussian. */
        const float ww    = static_cast<float>(st.windowWidthDp);
        const float wh    = static_cast<float>(st.windowHeightDp);
        const float scale = 1.0f / std::sqrt(2.0f);
        const auto  covered = [scale](const float p, const float lo, const float hi) {
            return 0.5f * (std::erf((hi - p) * scale) - std::erf((lo - p) * scale));
        };
        for (int y = 0; y < th; ++y) {
            const float py = (static_cast<float>(y) + 0.5f) * wh / static_cast<float>(th);
            for (int x = 0; x < tw; ++x) {
                const float px          = (static_cast<float>(x) + 0.5f) * ww / static_cast<float>(tw);
                const float topLeft     = 0.30f * (1.0f - covered(px, 2.0f, ww + 2.0f) * covered(py, 2.0f, wh + 2.0f));
                const float bottomRight = 0.15f * (1.0f - covered(px, -2.0f, ww - 2.0f) * covered(py, -2.0f, wh - 2.0f));
                auto       &d           = dst[static_cast<size_t>(y) * tw + x];
                d                       = over(d, 1.0f - (1.0f - topLeft) * (1.0f - bottomRight));
            }
        }
    }
}

void
emu88_lcd_draw(const DeviceModel model, const unsigned screen, const Screen *snapshot, uint32_t *argb)
{
    const auto geo = geometryFor(model, screen);
    if (geo.width < 0)
        return;
    if (!geo.style)
        drawCharacterPanel(argb, snapshot);
    else
        drawGraphicPanel(argb, geo, snapshot);
}

extern "C" int
emu88h_lcd_size(const int model, const unsigned screen, int *w, int *h)
{
    int tw = 0, th = 0;
    if (model >= 0 && emu88Lib::isDeviceModelValue(static_cast<uint32_t>(model)))
        textureSize(geometryFor(static_cast<DeviceModel>(model), screen), tw, th);
    if (w)
        *w = tw;
    if (h)
        *h = th;
    return tw > 0 && th > 0;
}
