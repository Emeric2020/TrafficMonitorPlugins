// Standalone GDI regression/preview runner. Only the reported device DPI is
// substituted, exercising the real renderer without changing Windows settings.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <cassert>
#include <iostream>

namespace {
    int test_dpi = 96;
    int DrawingDeviceCaps(HDC dc, int index)
    {
        return index == LOGPIXELSX || index == LOGPIXELSY ? test_dpi : ::GetDeviceCaps(dc, index);
    }
}
#define GetDeviceCaps DrawingDeviceCaps
#include "QoderUsagePlugin.cpp"
#undef GetDeviceCaps

namespace {
    constexpr COLORREF background = RGB(246, 247, 249);
    struct Surface
    {
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bitmap{};
        HGDIOBJ previous{};
        void* pixels{};
        int width, height;
        Surface(int w, int h) : width(w), height(h)
        {
            BITMAPINFO info{};
            info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
            bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            assert(bitmap);
            previous = SelectObject(dc, bitmap);
            Clear();
        }
        void Clear() const
        {
            RECT bounds{0, 0, width, height};
            HBRUSH brush = CreateSolidBrush(background);
            FillRect(dc, &bounds, brush);
            DeleteObject(brush);
        }
        void Save(const std::filesystem::path& path) const
        {
            GdiFlush();
            BITMAPFILEHEADER file{};
            file.bfType = 0x4d42;
            file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
            file.bfSize = file.bfOffBits + width * height * 4;
            BITMAPINFOHEADER info{sizeof(info), width, -height, 1, 32, BI_RGB};
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(&file), sizeof(file));
            out.write(reinterpret_cast<const char*>(&info), sizeof(info));
            out.write(static_cast<const char*>(pixels), width * height * 4);
            assert(out);
        }
        ~Surface()
        {
            SelectObject(dc, previous);
            DeleteObject(bitmap);
            DeleteDC(dc);
        }
    };

    void Publish(QuotaProvider provider, double remaining, double total = 100, bool available = true)
    {
        QoderSnapshot snapshot;
        snapshot.has_data = available;
        snapshot.fetched_at = 1;
        QoderQuotaRow row;
        row.quota.total = total;
        row.quota.remaining = remaining;
        snapshot.usage.rows.push_back(row);
        CQoderUsagePlugin::Instance().PublishSnapshot(std::move(snapshot), provider);
    }
}

int wmain(int argc, wchar_t** argv)
{
    const std::filesystem::path output = argc > 1 ? argv[1] : L".";
    std::filesystem::create_directories(output);
    auto& plugin = CQoderUsagePlugin::Instance();
    const wchar_t* ids[] = {L"QoderUsageQuota9", L"QoderCNUsageQuota", L"WorkBuddyInternationalCredits", L"WorkBuddyCredits"};
    for (int i = 0; i < 4; ++i)
    {
        assert(plugin.GetItem(i));
        assert(std::wstring(plugin.GetItem(i)->GetItemId()) == ids[i]);
        assert(plugin.GetItem(i)->IsDoubleLineExclusive() == 0);
        assert(plugin.GetItem(i)->OnItemInfo(IPluginItem::IIT_TASKBAR_SHARED_BORDER, nullptr, nullptr));
    }
    assert(!plugin.GetItem(4));
    for (int dpi : {96, 120, 144, 192})
    {
        test_dpi = dpi;
        int row_dip = 0;
        assert(plugin.GetItem(0)->OnItemInfo(IPluginItem::IIT_TASKBAR_MIN_ROW_HEIGHT, &row_dip, nullptr));
        assert(row_dip == 22);
        const int row_height = MulDiv(row_dip, dpi, 96);
        const int track_height = MulDiv(4, dpi, 96);
        const int text_height = row_height - 4 - track_height - MulDiv(1, dpi, 96);
        Surface surface(MulDiv(480, dpi, 96), row_height * 8);
        HFONT font = CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
        const HGDIOBJ previous = SelectObject(surface.dc, font);
        const int x = 8;
        Publish(QuotaProvider::WorkBuddyChina, 2200, 2200);
        const int width = plugin.GetItem(3)->GetItemWidthEx(surface.dc);
        const double balances[] = {100, 80.74, 50, 20, 0, 125, 0, 0};
        for (int row = 0; row < 8; ++row)
        {
            const int provider_index = row == 5 ? 2 : row % 4;
            const auto provider = static_cast<QuotaProvider>(provider_index);
            const bool has_data = row != 7;
            Publish(provider, balances[row], row == 6 ? 0 : 100, has_data);
            auto* item = plugin.GetItem(provider_index);
            const int y = row * row_height;
            const COLORREF text_before = GetTextColor(surface.dc);
            RECT clip_bounds{};
            const int clip_before = GetClipBox(surface.dc, &clip_bounds);
            item->DrawItem(surface.dc, x, y, width, row_height, row % 2 != 0);
            assert(GetCurrentObject(surface.dc, OBJ_FONT) == font);
            assert(GetTextColor(surface.dc) == text_before);
            RECT clip{};
            assert(GetClipBox(surface.dc, &clip) == clip_before);
            const int top = y;
            // Outline, one untouched pixel, then fill. Check outside the text inset.
            assert(GetPixel(surface.dc, x, top) == RGB(0, 0, 0));
            assert(GetPixel(surface.dc, x + 1, top + 1) == background);
            assert(GetPixel(surface.dc, x - 1, top) == background);
            assert(GetPixel(surface.dc, x + width, top) == background);
            const COLORREF first_fill = GetPixel(surface.dc, x + 2, top + 2);
            const COLORREF expected = row == 0 || row == 1 || row == 5 ? RGB(116, 170, 132)
                : row == 2 ? RGB(213, 174, 75)
                : row == 3 ? RGB(207, 128, 123) : RGB(194, 202, 211);
            assert(first_fill == RGB(236, 240, 242));
            if (row == 1)
            {
                const int edge = top + row_height - 2 - static_cast<int>(std::lround(track_height * 0.8074));
                assert(GetPixel(surface.dc, x + 2, edge - 1) == RGB(194, 202, 211));
                assert(GetPixel(surface.dc, x + 2, edge) == RGB(116, 170, 132));
            }
            assert(GetPixel(surface.dc, x + width - 2, top + 1) == background);
            assert(GetPixel(surface.dc, x + 2, top + row_height - 3) == expected);
            HFONT compact_font = QuotaFont(surface.dc, text_height);
            const HGDIOBJ host_font = SelectObject(surface.dc, compact_font);
            const auto summary = CompactSummary(plugin.Snapshot(provider), provider);
            SIZE text{};
            GetTextExtentPoint32W(surface.dc, summary.c_str(), static_cast<int>(summary.size()), &text);
            assert(text.cx <= width - 2 * QuotaScale(surface.dc, 4));
            assert(text.cy <= text_height);
            SelectObject(surface.dc, host_font);
            DeleteObject(compact_font);
        }
        Publish(QuotaProvider::Qoder, 2180, 2700);
        Publish(QuotaProvider::WorkBuddyChina, 2200, 2200);
        const int column_width = (std::max)(plugin.GetItem(0)->GetItemWidthEx(surface.dc),
            plugin.GetItem(3)->GetItemWidthEx(surface.dc));
        Surface column(column_width + 16, 2 * row_height + 8);
        const HGDIOBJ column_font = SelectObject(column.dc, font);
        plugin.GetItem(0)->DrawItem(column.dc, 8, 4, column_width, row_height + 1, false);
        plugin.GetItem(3)->DrawItem(column.dc, 8, 4 + row_height, column_width, row_height, false);
        const int separator = 4 + row_height;
        for (int pixel = 8; pixel < 8 + column_width; ++pixel)
            assert(GetPixel(column.dc, pixel, separator) == RGB(0, 0, 0));
        for (int pixel = 9; pixel < 7 + column_width; ++pixel)
        {
            assert(GetPixel(column.dc, pixel, separator - 1) == background);
            assert(GetPixel(column.dc, pixel, separator + 1) == background);
        }
        const auto color_runs = [&](int scan_y, int expected) {
            int runs = 0;
            bool colored = false;
            int previous_end = -1;
            for (int pixel = 10; pixel < column_width + 6; ++pixel)
            {
                const bool next = GetPixel(column.dc, pixel, scan_y) == RGB(116, 170, 132);
                if (next && !colored)
                {
                    if (previous_end >= 0) assert(pixel - previous_end == 1);
                    ++runs;
                }
                if (!next && colored) previous_end = pixel;
                colored = next;
            }
            assert(runs == expected);
        };
        color_runs(separator - 1 - track_height, 21); // gray is exposed at the top of the partial bottom-strip cell
        color_runs(separator - 2, 22); // its remaining 80 credits stay anchored to the bottom
        color_runs(2 * row_height + 2 - track_height, 22);
        color_runs(2 * row_height + 1, 22);
        const int full_grid_width = ((column_width - 4 - 21) / 22) * 22 + 21;
        for (int pixel = 10 + full_grid_width; pixel < column_width + 6; ++pixel)
            assert(GetPixel(column.dc, pixel, 2 * row_height + 1) == RGB(194, 202, 211));
        const auto equal_widths = [&](int scan_y, int expected_count) {
            std::vector<int> widths;
            int start = -1;
            int previous_end = -1;
            for (int pixel = 10; pixel <= column_width + 6; ++pixel)
            {
                const bool colored = pixel < column_width + 6 && GetPixel(column.dc, pixel, scan_y) == RGB(116, 170, 132);
                if (colored && start < 0)
                {
                    if (previous_end >= 0) assert(pixel - previous_end == 1);
                    start = pixel;
                }
                if (!colored && start >= 0)
                {
                    widths.push_back(pixel - start);
                    previous_end = pixel;
                    start = -1;
                }
            }
            assert(static_cast<int>(widths.size()) == expected_count);
            for (int cell : widths) assert(cell == widths.front());
        };
        equal_widths(separator - 2, 22);
        equal_widths(2 * row_height + 1, 22);
        // The entire text panel stays free of progress colors and gray grid gaps.
        for (int scan_y = 6; scan_y < separator - 1 - track_height; ++scan_y)
            for (int pixel = 10; pixel < column_width + 6; ++pixel)
            {
                const COLORREF color = GetPixel(column.dc, pixel, scan_y);
                assert(color != RGB(116, 170, 132));
                assert(color != RGB(194, 202, 211));
            }
        column.Save(output / (L"column-" + std::to_wstring(dpi) + L"dpi.bmp"));
        Publish(QuotaProvider::Qoder, 2700, 2700);
        plugin.GetItem(0)->DrawItem(column.dc, 8, 4, column_width, row_height + 1, false);
        color_runs(separator - 1 - track_height, 27);
        equal_widths(separator - 2, 27);
        SelectObject(column.dc, column_font);
        surface.Save(output / (L"quota-" + std::to_wstring(dpi) + L"dpi.bmp"));
        const DWORD handles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int i = 0; i < 1000; ++i)
            plugin.GetItem(0)->DrawItem(surface.dc, 0, 0, width, row_height, false);
        assert(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == handles);
        // Short/narrow rectangles must clip safely and restore all DC state.
        plugin.GetItem(0)->DrawItem(surface.dc, 0, 0, 3, 3, false);
        SelectObject(surface.dc, previous);
        DeleteObject(font);
        std::wcout << dpi << L" DPI: text panel / bottom strip, strict equal-width cells, 1px gaps, palette, depletion direction and GDI state passed\n";
    }
    return 0;
}
