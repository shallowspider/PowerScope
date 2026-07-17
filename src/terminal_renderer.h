#pragma once

#include "dashboard.h"

#include <memory>

namespace powerscope {

struct TerminalSize {
    int width = 100;
    int height = 30;
};

class TerminalRenderer {
public:
    TerminalRenderer();
    ~TerminalRenderer();
    TerminalRenderer(const TerminalRenderer&) = delete;
    TerminalRenderer& operator=(const TerminalRenderer&) = delete;

    TerminalSize Size() const;
    void Render(const DashboardFrame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void InitializeConsoleOutput();

} // namespace powerscope
