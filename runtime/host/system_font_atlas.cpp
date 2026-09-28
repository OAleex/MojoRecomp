#include "system_font_atlas.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace mojorecomp::host {

bool BuildSystemFontAtlas(uint32_t atlasWidth, uint32_t atlasHeight,
                          uint32_t firstGlyph, uint32_t glyphCount,
                          uint32_t columns, uint32_t cellSize,
                          int fontHeight, SystemFontAtlas& atlas) noexcept
{
    atlas = {};
    if (!atlasWidth || !atlasHeight || !glyphCount || !columns || cellSize < 8u)
        return false;

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc)
        return false;

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(atlasWidth);
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(atlasHeight);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* dibBits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bitmapInfo, DIB_RGB_COLORS,
                                      &dibBits, nullptr, 0);
    if (!bitmap || !dibBits)
    {
        if (bitmap)
            DeleteObject(bitmap);
        DeleteDC(dc);
        return false;
    }

    HFONT createdFont = CreateFontW(
        fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT font = createdFont
        ? createdFont
        : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    const HGDIOBJ oldFont = SelectObject(dc, font);
    std::memset(dibBits, 0, size_t(atlasWidth) * atlasHeight * 4u);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SetTextAlign(dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);

    atlas.advances.resize(glyphCount);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int textY = std::max(0, (int(cellSize) - int(metrics.tmHeight)) / 2);
    for (uint32_t i = 0; i < glyphCount; ++i)
    {
        const wchar_t ch = static_cast<wchar_t>(firstGlyph + i);
        const uint32_t cellX = (i % columns) * cellSize;
        const uint32_t cellY = (i / columns) * cellSize;
        SIZE extent{};
        if (!GetTextExtentPoint32W(dc, &ch, 1, &extent))
            extent.cx = 10;
        atlas.advances[i] = float(std::clamp<LONG>(
            extent.cx, 4, LONG(cellSize - 4u)));
        if (ch != L' ')
            TextOutW(dc, int(cellX) + 2, int(cellY) + textY, &ch, 1);
    }

    atlas.alpha.resize(size_t(atlasWidth) * atlasHeight);
    const auto* bgra = static_cast<const uint8_t*>(dibBits);
    for (size_t i = 0; i < atlas.alpha.size(); ++i)
    {
        const uint8_t b = bgra[i * 4u + 0u];
        const uint8_t g = bgra[i * 4u + 1u];
        const uint8_t r = bgra[i * 4u + 2u];
        atlas.alpha[i] = std::max({r, g, b});
    }

    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    if (createdFont)
        DeleteObject(createdFont);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return true;
}

} // namespace mojorecomp::host
