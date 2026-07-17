#include "terminal_renderer.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace powerscope {
namespace {

bool g_virtual_terminal_enabled = false;

int DisplayCellWidth(wchar_t ch) {
    const unsigned int c = static_cast<unsigned int>(ch);
    if (c == 0) return 0;
    if (c < 0x1100) return 1;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE10 && c <= 0xFE19) || (c >= 0xFE30 && c <= 0xFE6F) ||
        (c >= 0xFF01 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6)) return 2;
    return 1;
}

std::size_t DisplayWidth(const std::wstring& text) {
    std::size_t width = 0;
    for (wchar_t ch : text) width += static_cast<std::size_t>(DisplayCellWidth(ch));
    return width;
}

std::wstring ToneCode(TextTone tone) {
    switch (tone) {
    case TextTone::Key: return L"\x1b[96m";
    case TextTone::Muted: return L"\x1b[90m";
    case TextTone::Hint: return L"\x1b[36m";
    case TextTone::Attention: return L"\x1b[33m";
    case TextTone::Severe: return L"\x1b[91m";
    case TextTone::Default: return L"\x1b[0m";
    }
    return L"\x1b[0m";
}

StyledLine Truncate(const StyledLine& line, std::size_t width) {
    StyledLine result;
    std::size_t used = 0;
    bool truncated = false;
    const std::size_t target = width > 0 ? width - 1 : 0;
    for (const auto& span : line) {
        TextSpan output{{}, span.tone};
        for (wchar_t ch : span.text) {
            const auto cell = static_cast<std::size_t>(DisplayCellWidth(ch));
            if (used + cell > target) {
                truncated = true;
                break;
            }
            output.text.push_back(ch);
            used += cell;
        }
        if (!output.text.empty()) result.push_back(std::move(output));
        if (truncated) break;
    }
    if (truncated && width > 0) result.push_back({L"…", TextTone::Muted});
    return result;
}

void SetCursorVisible(HANDLE output, bool visible) {
    CONSOLE_CURSOR_INFO cursor{};
    if (!GetConsoleCursorInfo(output, &cursor)) return;
    cursor.bVisible = visible ? TRUE : FALSE;
    SetConsoleCursorInfo(output, &cursor);
}

} // namespace

void InitializeConsoleOutput() {
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output == INVALID_HANDLE_VALUE || output == nullptr) return;
    DWORD mode = 0;
    if (GetConsoleMode(output, &mode) && SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        g_virtual_terminal_enabled = true;
    }
}

struct TerminalRenderer::Impl {
    Impl() : output(GetStdHandle(STD_OUTPUT_HANDLE)) {
        valid = output != INVALID_HANDLE_VALUE && output != nullptr;
        if (!valid) return;
        SetCursorVisible(output, false);
        Write(g_virtual_terminal_enabled ? L"\x1b[2J\x1b[H\x1b[?25l" : L"");
        if (!g_virtual_terminal_enabled) {
            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (GetConsoleScreenBufferInfo(output, &info)) {
                const DWORD cells = static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y);
                DWORD written = 0;
                FillConsoleOutputCharacterW(output, L' ', cells, COORD{0, 0}, &written);
                FillConsoleOutputAttribute(output, info.wAttributes, cells, COORD{0, 0}, &written);
                SetConsoleCursorPosition(output, COORD{0, 0});
            }
        }
    }

    ~Impl() {
        if (!valid) return;
        Write(g_virtual_terminal_enabled ? L"\x1b[0m\x1b[?25h" : L"");
        SetCursorVisible(output, true);
    }

    TerminalSize Size() const {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (valid && GetConsoleScreenBufferInfo(output, &info)) {
            return {std::max(20, static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1)),
                std::max(16, static_cast<int>(info.srWindow.Bottom - info.srWindow.Top + 1))};
        }
        return {};
    }

    void Render(const DashboardFrame& frame) {
        if (!valid) return;
        const auto size = Size();
        const std::size_t content_width = static_cast<std::size_t>(std::max(20, size.width - 1));
        const std::size_t rows = std::max(previous_rows, frame.lines.size());
        std::wstring output_text = g_virtual_terminal_enabled ? L"\x1b[H" : L"";
        if (!g_virtual_terminal_enabled) SetConsoleCursorPosition(output, COORD{0, 0});

        for (std::size_t i = 0; i < rows; ++i) {
            StyledLine line = i < frame.lines.size() ? Truncate(frame.lines[i], content_width) : StyledLine{};
            std::wstring plain;
            for (const auto& span : line) {
                plain += span.text;
                if (g_virtual_terminal_enabled) output_text += ToneCode(span.tone);
                output_text += span.text;
            }
            if (g_virtual_terminal_enabled) output_text += L"\x1b[0m";
            const auto used = DisplayWidth(plain);
            if (used < content_width) output_text.append(content_width - used, L' ');
            if (i + 1 < rows) output_text += L"\r\n";
        }
        if (g_virtual_terminal_enabled) output_text += L"\x1b[J";
        Write(output_text);
        previous_rows = frame.lines.size();
    }

    void Write(const std::wstring& text) const {
        if (!valid || text.empty()) return;
        DWORD written = 0;
        WriteConsoleW(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }

    HANDLE output = INVALID_HANDLE_VALUE;
    bool valid = false;
    std::size_t previous_rows = 0;
};

TerminalRenderer::TerminalRenderer() : impl_(std::make_unique<Impl>()) {}
TerminalRenderer::~TerminalRenderer() = default;
TerminalSize TerminalRenderer::Size() const { return impl_->Size(); }
void TerminalRenderer::Render(const DashboardFrame& frame) { impl_->Render(frame); }

} // namespace powerscope
