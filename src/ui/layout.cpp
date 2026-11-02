#include "ui/layout.h"

#include <algorithm>
#include <cmath>

namespace ui {
namespace layout {
namespace {

int sc(int v) { return int(std::lround(float(v) * kScale)); }

}  // namespace

void recompute() {
    kScreenW = sc(kDesignW);
    kScreenH = sc(kDesignH);

    kTopBarH = sc(base.topBarH);
    kBotBarH = sc(base.botBarH);
    kPanelW = sc(base.panelW);
    kPanelPadTop = sc(base.panelPadTop);
    kPanelPadX = sc(base.panelPadX);
    kPanelPadBot = sc(base.panelPadBot);
    kPanelGap = sc(base.panelGap);
    kStickBox = sc(base.stickBox);
    kDpadCell = sc(base.dpadCell);
    kAbxyCell = sc(base.abxyCell);
    kTermPadX = sc(base.termPadX);
    kTermPadTop = sc(base.termPadTop);
    kTermPadBot = sc(base.termPadBot);
    kCellW = std::max(1, sc(base.cellW));
    kCellH = std::max(1, sc(base.cellH));
    kImeRows = std::max(1, sc(base.imeRows));

    kContentY = kTopBarH;
    kContentH = kScreenH - kTopBarH - kBotBarH;
    kTermX = kPanelW;
    kTermW = kScreenW - 2 * kPanelW;
    kStickCell = std::max(1, kStickBox / 5);
    kTermCols = std::max(1, (kTermW - 2 * kTermPadX) / kCellW);
    kTermRows = std::max(1, (kContentH - kTermPadTop - kTermPadBot) / kCellH - kImeRows);
}

void setScale(float s) {
    kScale = std::clamp(s, 0.5f, 4.0f);
    recompute();
}

}  // namespace layout
}  // namespace ui
