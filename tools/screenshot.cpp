#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static bool save_bmp(const char* path, int w, int h, const void* bits) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    int row_bytes = ((w * 3 + 3) & ~3);
    int data_size = row_bytes * h;

    fh.bfType = 0x4D42;
    fh.bfSize = sizeof(fh) + sizeof(ih) + data_size;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);

    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = data_size;

    std::vector<uint8_t> out(data_size);
    auto* src = static_cast<const uint8_t*>(bits);
    for (int y = 0; y < h; y++) {
        int src_row = y;
        auto* s = src + src_row * w * 4;
        auto* d = out.data() + y * row_bytes;
        for (int x = 0; x < w; x++) {
            d[x * 3 + 0] = s[x * 4 + 0];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 2];
        }
    }

    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(out.data(), data_size, 1, f);
    fclose(f);
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: screenshot.exe \"Window Title\" output.bmp\n");
        return 1;
    }

    const char* title = argv[1];
    const char* output = argv[2];

    HWND hwnd = nullptr;
    struct FindCtx { const char* title; HWND result; };
    FindCtx ctx{title, nullptr};

    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<FindCtx*>(lp);
        char buf[256];
        GetWindowTextA(h, buf, sizeof(buf));
        if (strstr(buf, c->title)) {
            c->result = h;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    hwnd = ctx.result;
    if (!hwnd) {
        fprintf(stderr, "Window '%s' not found\n", title);
        return 1;
    }

    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        fprintf(stderr, "Window has zero size (%dx%d)\n", w, h);
        return 1;
    }

    HDC hdc_screen = GetDC(nullptr);
    HDC hdc_mem = CreateCompatibleDC(hdc_screen);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdc_mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) {
        fprintf(stderr, "CreateDIBSection failed\n");
        DeleteDC(hdc_mem);
        ReleaseDC(nullptr, hdc_screen);
        return 1;
    }

    HGDIOBJ old = SelectObject(hdc_mem, hbm);

    BOOL ok = PrintWindow(hwnd, hdc_mem, PW_RENDERFULLCONTENT);
    if (!ok) {
        HDC hdc_wnd = GetDC(hwnd);
        BitBlt(hdc_mem, 0, 0, w, h, hdc_wnd, 0, 0, SRCCOPY);
        ReleaseDC(hwnd, hdc_wnd);
    }

    SelectObject(hdc_mem, old);

    if (save_bmp(output, w, h, bits)) {
        printf("Saved %dx%d screenshot to %s\n", w, h, output);
    } else {
        fprintf(stderr, "Failed to save %s\n", output);
        DeleteObject(hbm);
        DeleteDC(hdc_mem);
        ReleaseDC(nullptr, hdc_screen);
        return 1;
    }

    DeleteObject(hbm);
    DeleteDC(hdc_mem);
    ReleaseDC(nullptr, hdc_screen);
    return 0;
}
